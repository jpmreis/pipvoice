#include "net_http.h"
#include "board.h"      /* PIP_BOARD_NAME for theme rendition selection */
#include "config.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static const char *TAG = "http";

#define RESP_MAX (16 * 1024)

static esp_http_client_handle_t client_new(const char *path_fmt, const char *arg,
                                           esp_http_client_method_t method)
{
    char url[160];
    if (arg) snprintf(url, sizeof(url), "%s%s%s%s", g_cfg.server_base,
                      path_fmt, arg, strstr(path_fmt, "%s") ? "" : "");
    else     snprintf(url, sizeof(url), "%s%s", g_cfg.server_base, path_fmt);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return NULL;

    char auth[96];
    snprintf(auth, sizeof(auth), "Bearer %s", g_cfg.auth_token);
    esp_http_client_set_header(c, "Authorization", auth);
    return c;
}

/* perform request, read body into caller buffer; returns status or <0 */
static int perform_read(esp_http_client_handle_t c, char *body, size_t cap)
{
    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) return -1;
    esp_http_client_fetch_headers(c);
    int total = 0, r;
    while (body && (r = esp_http_client_read(c, body + total,
                                             (int)(cap - 1 - total))) > 0)
        total += r;
    if (body) body[total] = 0;
    int status = esp_http_client_get_status_code(c);
    esp_http_client_close(c);
    return status;
}

/* ---------------- contacts ---------------- */
bool http_get_contacts(ui_contact_t *out, uint8_t cap, uint8_t *count)
{
    esp_http_client_handle_t c = client_new("/contacts", NULL, HTTP_METHOD_GET);
    if (!c) return false;
    static char body[RESP_MAX];
    int status = perform_read(c, body, sizeof(body));
    esp_http_client_cleanup(c);
    if (status != 200) { ESP_LOGW(TAG, "contacts -> %d", status); return false; }

    cJSON *root = cJSON_Parse(body);
    if (!root) return false;
    uint8_t n = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, root) {
        if (n >= cap) break;
        cJSON *id = cJSON_GetObjectItem(item, "device_id");
        cJSON *nm = cJSON_GetObjectItem(item, "name");
        cJSON *co = cJSON_GetObjectItem(item, "color");
        if (!cJSON_IsString(id) || !cJSON_IsString(nm)) continue;
        /* no self-filter here: contact ids are usernames, not device ids;
         * the server already excludes self from /contacts */
        strlcpy(out[n].id, id->valuestring, UI_ID_LEN);
        strlcpy(out[n].name, nm->valuestring, UI_NAME_LEN);
        out[n].color = cJSON_IsString(co)
                     ? (uint32_t)strtoul(co->valuestring, NULL, 16) : 0x4FC3F7;
        n++;
    }
    cJSON_Delete(root);
    *count = n;
    return true;
}

/* ---------------- background themes ---------------- */
bool http_get_themes(ui_theme_info_t *out, uint8_t cap, uint8_t *count)
{
    esp_http_client_handle_t c = client_new("/themes", NULL, HTTP_METHOD_GET);
    if (!c) return false;
    static char body[2048];
    int status = perform_read(c, body, sizeof(body));
    esp_http_client_cleanup(c);
    if (status != 200) { ESP_LOGW(TAG, "themes -> %d", status); return false; }

    cJSON *root = cJSON_Parse(body);
    if (!root) return false;
    uint8_t n = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, root) {
        if (n >= cap) break;
        cJSON *nm = cJSON_GetObjectItem(item, "name");
        cJSON *lb = cJSON_GetObjectItem(item, "label");
        cJSON *fg = cJSON_GetObjectItem(item, "fg");
        cJSON *vr = cJSON_GetObjectItem(item, "ver");
        if (!cJSON_IsString(nm)) continue;
        strlcpy(out[n].name, nm->valuestring, UI_NAME_LEN);
        strlcpy(out[n].label, cJSON_IsString(lb) ? lb->valuestring
                                                 : nm->valuestring, UI_NAME_LEN);
        strlcpy(out[n].ver, cJSON_IsString(vr) ? vr->valuestring : "0",
                UI_THEME_VER_LEN);
        out[n].black_text = cJSON_IsString(fg)
                          && strcmp(fg->valuestring, "black") == 0;
        n++;
    }
    cJSON_Delete(root);
    *count = n;
    return true;
}

static bool download_to_file(const char *path, const char *dest_path)
{
    esp_http_client_handle_t c = client_new(path, NULL, HTTP_METHOD_GET);
    if (!c) return false;

    bool ok = false;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        if (esp_http_client_get_status_code(c) == 200) {
            FILE *f = fopen(dest_path, "wb");
            if (f) {
                char buf[1024];
                int r;
                ok = true;
                while ((r = esp_http_client_read(c, buf, sizeof(buf))) > 0)
                    if (fwrite(buf, 1, r, f) != (size_t)r) { ok = false; break; }
                fclose(f);
            }
        }
        esp_http_client_close(c);
    }
    esp_http_client_cleanup(c);
    return ok;
}

/* board= selects the panel-sized rendition (server themes.py; an old
 * server ignores the query and serves the 1.8 bin - theme.c's byte-count
 * guard rejects it on other panels rather than painting garbage) */
bool http_download_theme(const char *name, const char *dest_path)
{
    char path[112];
    snprintf(path, sizeof(path), "/themes/%s/device.bin?board=%s",
             name, PIP_BOARD_NAME);
    return download_to_file(path, dest_path);
}

bool http_download_theme_thumb(const char *name, const char *dest_path)
{
    char path[112];
    snprintf(path, sizeof(path), "/themes/%s/thumb.bin?board=%s",
             name, PIP_BOARD_NAME);
    return download_to_file(path, dest_path);
}

/* ---------------- upload (multipart) ---------------- */
bool http_upload_message(const char *vmsg_path, const char *recipient_id,
                         uint16_t duration_s)
{
    FILE *f = fopen(vmsg_path, "rb");
    if (!f) return false;
    struct stat st;
    stat(vmsg_path, &st);

    const char *bound = "----vmsgboundary7d9f2a";
    char head[512], tail[64];   /* worst-case head with 39-char recipient is ~340 */
    int head_len = snprintf(head, sizeof(head),
        "--%s\r\nContent-Disposition: form-data; name=\"recipient_id\"\r\n\r\n%s\r\n"
        "--%s\r\nContent-Disposition: form-data; name=\"duration\"\r\n\r\n%u\r\n"
        "--%s\r\nContent-Disposition: form-data; name=\"audio\"; "
        "filename=\"msg.vmsg\"\r\nContent-Type: application/octet-stream\r\n\r\n",
        bound, recipient_id, bound, duration_s, bound);
    int tail_len = snprintf(tail, sizeof(tail), "\r\n--%s--\r\n", bound);

    esp_http_client_handle_t c = client_new("/messages", NULL, HTTP_METHOD_POST);
    if (!c) { fclose(f); return false; }
    char ctype[64];
    snprintf(ctype, sizeof(ctype), "multipart/form-data; boundary=%s", bound);
    esp_http_client_set_header(c, "Content-Type", ctype);

    int total = head_len + (int)st.st_size + tail_len;
    bool ok = false;
    if (esp_http_client_open(c, total) == ESP_OK) {
        esp_http_client_write(c, head, head_len);
        char buf[1024];
        size_t r;
        while ((r = fread(buf, 1, sizeof(buf), f)) > 0)
            esp_http_client_write(c, buf, (int)r);
        esp_http_client_write(c, tail, tail_len);
        esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        ok = (status >= 200 && status < 300);
        if (!ok) ESP_LOGW(TAG, "upload -> %d", status);
        esp_http_client_close(c);
    }
    fclose(f);
    esp_http_client_cleanup(c);
    return ok;
}

/* ---------------- inbox list ---------------- */
bool http_get_inbox(http_inbox_item_t *out, uint8_t cap, uint8_t *count)
{
    esp_http_client_handle_t c = client_new("/inbox", NULL, HTTP_METHOD_GET);
    if (!c) return false;
    static char body[RESP_MAX];
    int status = perform_read(c, body, sizeof(body));
    esp_http_client_cleanup(c);
    if (status != 200) return false;

    cJSON *root = cJSON_Parse(body);
    if (!root) return false;
    uint8_t n = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, root) {
        if (n >= cap) break;
        cJSON *id = cJSON_GetObjectItem(item, "id");
        cJSON *sid = cJSON_GetObjectItem(item, "sender_id");
        cJSON *snm = cJSON_GetObjectItem(item, "sender_name");
        cJSON *sco = cJSON_GetObjectItem(item, "sender_color");
        cJSON *ts = cJSON_GetObjectItem(item, "when");
        cJSON *ep = cJSON_GetObjectItem(item, "ts");
        cJSON *du = cJSON_GetObjectItem(item, "duration");
        cJSON *re = cJSON_GetObjectItem(item, "reaction");
        if (!cJSON_IsString(id)) continue;
        http_inbox_item_t *m = &out[n++];
        strlcpy(m->id, id->valuestring, UI_ID_LEN);
        strlcpy(m->sender_id, cJSON_IsString(sid) ? sid->valuestring : "?",
                UI_ID_LEN);
        strlcpy(m->sender_name, cJSON_IsString(snm) ? snm->valuestring : "?",
                UI_NAME_LEN);
        m->sender_color = cJSON_IsString(sco)
                        ? (uint32_t)strtoul(sco->valuestring, NULL, 16) : 0x999999;
        /* keep the raw epoch: rendering happens at display time so the
         * clock is right even if the TZ arrives after the download */
        m->ts = cJSON_IsNumber(ep) ? (uint32_t)ep->valuedouble : 0;
        strlcpy(m->when, cJSON_IsString(ts) ? ts->valuestring : "", 20);
        m->duration_s = cJSON_IsNumber(du) ? (uint16_t)du->valueint : 0;
        strlcpy(m->reaction, cJSON_IsString(re) ? re->valuestring : "",
                sizeof(m->reaction));
    }
    cJSON_Delete(root);
    *count = n;
    return true;
}

/* ---------------- audio download ---------------- */
bool http_download_audio(const char *msg_id, const char *dest_path)
{
    char path[96];
    snprintf(path, sizeof(path), "/messages/%s/audio", msg_id);
    esp_http_client_handle_t c = client_new(path, NULL, HTTP_METHOD_GET);
    if (!c) return false;

    bool ok = false;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        if (esp_http_client_get_status_code(c) == 200) {
            FILE *f = fopen(dest_path, "wb");
            if (f) {
                char buf[1024];
                int r;
                while ((r = esp_http_client_read(c, buf, sizeof(buf))) > 0)
                    fwrite(buf, 1, r, f);
                /* A weak link drops connections mid-body, and this used to
                 * call any partial file a success: the box kept a truncated
                 * .vmsg forever (the inbox mirror never re-downloads what it
                 * already has) and played a message that cut off. Only a
                 * complete body counts; a partial one is unlinked so the
                 * next sync fetches it again. */
                ok = (fclose(f) == 0) &&
                     esp_http_client_is_complete_data_received(c);
                if (!ok) {
                    ESP_LOGW(TAG, "audio %s truncated, will retry", msg_id);
                    remove(dest_path);
                }
            }
        }
        esp_http_client_close(c);
    }
    esp_http_client_cleanup(c);
    return ok;
}

/* ---------------- ack / delete ---------------- */
static bool simple_call(const char *fmt, const char *msg_id,
                        esp_http_client_method_t method)
{
    char path[96];
    snprintf(path, sizeof(path), fmt, msg_id);
    esp_http_client_handle_t c = client_new(path, NULL, method);
    if (!c) return false;
    int status = perform_read(c, NULL, 0);
    esp_http_client_cleanup(c);
    return status >= 200 && status < 300;
}

bool http_ack_message(const char *id)    { return simple_call("/messages/%s/ack", id, HTTP_METHOD_POST); }

bool http_delete_message(const char *id)
{
    char path[96];
    snprintf(path, sizeof(path), "/messages/%s", id);
    esp_http_client_handle_t c = client_new(path, NULL, HTTP_METHOD_DELETE);
    if (!c) return false;
    int status = perform_read(c, NULL, 0);
    esp_http_client_cleanup(c);
    /* 404 = already gone server-side (expired/acked elsewhere): success,
     * otherwise the pending delete would be retried forever */
    return (status >= 200 && status < 300) || status == 404;
}

/* ---------------- reactions ---------------- */
/* tiny JSON POST; 404 counts as success (message gone server-side: the
 * pending marker must be dropped, mirroring http_delete_message) */
static bool json_post(const char *path, const char *body)
{
    esp_http_client_handle_t c = client_new(path, NULL, HTTP_METHOD_POST);
    if (!c) return false;
    esp_http_client_set_header(c, "Content-Type", "application/json");
    int status = -1;
    int blen = (int)strlen(body);
    if (esp_http_client_open(c, blen) == ESP_OK) {
        esp_http_client_write(c, body, blen);
        esp_http_client_fetch_headers(c);
        status = esp_http_client_get_status_code(c);
        esp_http_client_close(c);
    }
    esp_http_client_cleanup(c);
    if (!((status >= 200 && status < 300) || status == 404))
        ESP_LOGW(TAG, "%s -> %d", path, status);
    return (status >= 200 && status < 300) || status == 404;
}

bool http_post_reaction(const char *msg_id, const char *key)
{
    char path[96], body[64];
    snprintf(path, sizeof(path), "/messages/%s/reaction", msg_id);
    snprintf(body, sizeof(body), "{\"reaction\":\"%s\"}", key ? key : "");
    return json_post(path, body);
}

bool http_post_reactions_seen(const char *username)
{
    char body[64];
    snprintf(body, sizeof(body), "{\"from\":\"%s\"}", username);
    return json_post("/reactions/seen", body);
}

bool http_get_reactions(ui_reaction_t *out, uint8_t cap, uint8_t *count)
{
    esp_http_client_handle_t c = client_new("/reactions", NULL, HTTP_METHOD_GET);
    if (!c) return false;
    /* PSRAM, not another 16 KB static: this runs rarely and the response
     * is small (a handful of unseen reactions) */
    const size_t bufsz = 8192;
    char *body = heap_caps_malloc(bufsz, MALLOC_CAP_SPIRAM);
    if (!body) { esp_http_client_cleanup(c); return false; }
    int status = perform_read(c, body, bufsz);
    esp_http_client_cleanup(c);
    if (status != 200) { free(body); return false; }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return false;
    uint8_t n = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, root) {
        if (n >= cap) break;
        cJSON *fr = cJSON_GetObjectItem(item, "from");
        cJSON *fn = cJSON_GetObjectItem(item, "from_name");
        cJSON *re = cJSON_GetObjectItem(item, "reaction");
        if (!cJSON_IsString(fr) || !cJSON_IsString(re)) continue;
        bool dup = false;      /* newest first: keep the latest per sender */
        for (uint8_t i = 0; i < n && !dup; i++)
            dup = !strcmp(out[i].from, fr->valuestring);
        if (dup) continue;
        strlcpy(out[n].from, fr->valuestring, UI_ID_LEN);
        strlcpy(out[n].from_name, cJSON_IsString(fn) ? fn->valuestring : "?",
                UI_NAME_LEN);
        strlcpy(out[n].key, re->valuestring, UI_REACT_KEY_LEN);
        n++;
    }
    cJSON_Delete(root);
    *count = n;
    return true;
}

/* ---------------- per-device config (voice control) ---------------- */
bool http_get_device_config(bool *voice_enabled, http_prompt_t *out,
                            uint8_t cap, uint8_t *count)
{
    esp_http_client_handle_t c = client_new("/device", NULL, HTTP_METHOD_GET);
    if (!c) return false;
    static char body[4096];
    int status = perform_read(c, body, sizeof(body));
    esp_http_client_cleanup(c);
    if (status != 200) {
        /* old server (404) or trouble: keep current state */
        if (status != 404) ESP_LOGW(TAG, "device config -> %d", status);
        return false;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) return false;
    *voice_enabled = cJSON_IsTrue(cJSON_GetObjectItem(root, "voice"));
    uint8_t n = 0;
    const cJSON *list = cJSON_GetObjectItem(root, "prompts");
    const cJSON *item;
    cJSON_ArrayForEach(item, list) {
        if (n >= cap) break;
        cJSON *key = cJSON_GetObjectItem(item, "key");
        cJSON *ver = cJSON_GetObjectItem(item, "ver");
        if (!cJSON_IsString(key) || !cJSON_IsString(ver)) continue;
        strlcpy(out[n].key, key->valuestring, sizeof(out[n].key));
        strlcpy(out[n].ver, ver->valuestring, sizeof(out[n].ver));
        n++;
    }
    cJSON_Delete(root);
    *count = n;
    return true;
}

bool http_download_prompt(const char *key, const char *ver,
                          const char *dest_path)
{
    char path[96];
    snprintf(path, sizeof(path), "/voice/%s.vmsg?v=%s", key, ver);
    return download_to_file(path, dest_path);
}

/* ---------------- timezone from public IP ---------------- */
bool http_set_tz_from_ip(void)
{
    /* ip-api.com serves plain http; worldtimeapi does not answer on it.
     * "offset" is seconds east of UTC, DST already applied. */
    esp_http_client_config_t cfg = {
        .url = "http://ip-api.com/json/?fields=status,offset",
        .timeout_ms = 8000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;
    static char body[512];
    int status = perform_read(c, body, sizeof(body));
    esp_http_client_cleanup(c);
    if (status != 200 || !strstr(body, "\"status\":\"success\"")) {
        ESP_LOGW(TAG, "tz lookup failed (status %d)", status);
        return false;
    }

    char *p = strstr(body, "\"offset\":");
    if (!p) return false;
    long off = strtol(p + 9, NULL, 10);
    if (off < -14 * 3600 || off > 14 * 3600) return false;

    /* POSIX TZ sign is inverted: UTC+01:00 -> "LOC-1:00". The zone name
     * must be >= 3 chars or newlib's tzset silently keeps UTC. */
    long a = off < 0 ? -off : off;
    char tz[32];
    snprintf(tz, sizeof(tz), "LOC%c%ld:%02ld", off >= 0 ? '-' : '+',
             a / 3600, (a % 3600) / 60);
    setenv("TZ", tz, 1);
    tzset();
    ESP_LOGI(TAG, "timezone set from IP: UTC%+ld:%02ld", off / 3600,
             (a % 3600) / 60);
    return true;
}
