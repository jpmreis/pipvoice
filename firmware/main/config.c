#include "config.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mbedtls/sha256.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "config";
app_config_t g_cfg;

/* every WiFi network ever used, most-recent-first; entry 0 is active */
static wifi_cred_t s_wifi[CFG_MAX_WIFI];
static uint8_t     s_wifi_count;

static void get_str(nvs_handle_t h, const char *key, char *dst, size_t cap,
                    const char *dflt)
{
    size_t len = cap;
    if (nvs_get_str(h, key, dst, &len) != ESP_OK) {
        strlcpy(dst, dflt, cap);
    }
}

static uint32_t get_u32(nvs_handle_t h, const char *key, uint32_t dflt)
{
    uint32_t v = dflt;
    nvs_get_u32(h, key, &v);
    return v;
}

/* THE single hosted-vs-self-hosted check on the firmware side: this box
 * talks to the hosted instance iff its provisioned server URL points at
 * pipvoice.com. The server twin is hosted() in server/app/db.py. */
bool config_is_hosted(void)
{
    const char *host = strstr(g_cfg.server_base, "//");
    host = host ? host + 2 : g_cfg.server_base;
    const char *dom = strstr(host, "pipvoice.com");
    if (!dom) return false;
    /* the match must be the whole host (or a subdomain of it), not a
     * substring of some other domain, and must end where the host ends */
    if (dom != host && dom[-1] != '.') return false;
    char end = dom[strlen("pipvoice.com")];
    return end == '\0' || end == '/' || end == ':';
}

/* ---------------- WiFi credential store ---------------- */
static int wifi_find(const char *ssid)
{
    for (uint8_t i = 0; i < s_wifi_count; i++)
        if (!strcmp(s_wifi[i].ssid, ssid)) return i;
    return -1;
}

/* move/insert (ssid,pass) to the front; oldest entry falls off when full */
static void wifi_upsert_front(const char *ssid, const char *pass)
{
    int at = wifi_find(ssid);
    if (at < 0)
        at = (s_wifi_count < CFG_MAX_WIFI) ? s_wifi_count++ : CFG_MAX_WIFI - 1;
    for (int i = at; i > 0; i--) s_wifi[i] = s_wifi[i - 1];
    memset(&s_wifi[0], 0, sizeof(s_wifi[0]));
    strlcpy(s_wifi[0].ssid, ssid, sizeof(s_wifi[0].ssid));
    strlcpy(s_wifi[0].pass, pass, sizeof(s_wifi[0].pass));
}

static void wifi_list_write(nvs_handle_t h)
{
    nvs_set_blob(h, "wifi_nets", s_wifi,
                 s_wifi_count * sizeof(wifi_cred_t));
}

uint8_t config_wifi_list(wifi_cred_t *out, uint8_t cap)
{
    uint8_t n = s_wifi_count < cap ? s_wifi_count : cap;
    memcpy(out, s_wifi, n * sizeof(wifi_cred_t));
    return n;
}

void config_load(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open("pipcfg", NVS_READWRITE, &h));
    get_str(h, "device_id",   g_cfg.device_id,   sizeof(g_cfg.device_id),   "pip-unset");
    get_str(h, "device_name", g_cfg.device_name, sizeof(g_cfg.device_name), "Unnamed");
    get_str(h, "server_base", g_cfg.server_base, sizeof(g_cfg.server_base),
            "https://voice.example.com/v1");
    get_str(h, "auth_token",  g_cfg.auth_token,  sizeof(g_cfg.auth_token),  "");
    get_str(h, "mqtt_host",   g_cfg.mqtt_host,   sizeof(g_cfg.mqtt_host),   "");
    g_cfg.mqtt_port = (uint16_t)get_u32(h, "mqtt_port", 8883);
    get_str(h, "mqtt_user",   g_cfg.mqtt_user,   sizeof(g_cfg.mqtt_user),   "");
    get_str(h, "mqtt_pass",   g_cfg.mqtt_pass,   sizeof(g_cfg.mqtt_pass),   "");
    get_str(h, "pin_salt",    g_cfg.pin_salt,    sizeof(g_cfg.pin_salt),    "salt");
    get_str(h, "pin_hash",    g_cfg.pin_hash,    sizeof(g_cfg.pin_hash),    "");
    get_str(h, "wifi_ssid",   g_cfg.wifi_ssid,   sizeof(g_cfg.wifi_ssid),   "");
    get_str(h, "wifi_pass",   g_cfg.wifi_pass,   sizeof(g_cfg.wifi_pass),   "");
    g_cfg.max_message_s  = (uint16_t)get_u32(h, "max_msg_s", 90);
    g_cfg.speaker_volume = (uint8_t)get_u32(h, "volume", 100);
    g_cfg.brightness     = (uint8_t)get_u32(h, "brightness", 255);
    get_str(h, "theme", g_cfg.theme_name, sizeof(g_cfg.theme_name), "");
    g_cfg.theme_black_text = get_u32(h, "theme_fg", 0) != 0;
    g_cfg.voice_enabled    = get_u32(h, "voice_en", 0) != 0;

    /* WiFi network list; migrate the legacy single-network keys into it.
     * Zero networks is a valid state: app_main auto-opens WiFi setup. */
    size_t blen = sizeof(s_wifi);
    if (nvs_get_blob(h, "wifi_nets", s_wifi, &blen) == ESP_OK &&
        blen % sizeof(wifi_cred_t) == 0)
        s_wifi_count = blen / sizeof(wifi_cred_t);
    if (s_wifi_count == 0 && g_cfg.wifi_ssid[0]) {
        wifi_upsert_front(g_cfg.wifi_ssid, g_cfg.wifi_pass);
        wifi_list_write(h);
        nvs_commit(h);
    }
    if (s_wifi_count) {          /* active network mirrors entry 0 */
        strlcpy(g_cfg.wifi_ssid, s_wifi[0].ssid, sizeof(g_cfg.wifi_ssid));
        strlcpy(g_cfg.wifi_pass, s_wifi[0].pass, sizeof(g_cfg.wifi_pass));
    }

    nvs_close(h);
    ESP_LOGI(TAG, "loaded config for %s (%s), %u wifi network(s)",
             g_cfg.device_id, g_cfg.device_name, s_wifi_count);
}

static void save_str(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open("pipcfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, key, val);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void save_u32(const char *key, uint32_t val)
{
    nvs_handle_t h;
    if (nvs_open("pipcfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, key, val);
        nvs_commit(h);
        nvs_close(h);
    }
}

void config_save_wifi(const char *ssid, const char *pass)
{
    strlcpy(g_cfg.wifi_ssid, ssid, sizeof(g_cfg.wifi_ssid));
    strlcpy(g_cfg.wifi_pass, pass, sizeof(g_cfg.wifi_pass));
    wifi_upsert_front(ssid, pass);
    nvs_handle_t h;
    if (nvs_open("pipcfg", NVS_READWRITE, &h) == ESP_OK) {
        /* legacy single-network keys kept in sync for firmware rollbacks */
        nvs_set_str(h, "wifi_ssid", ssid);
        nvs_set_str(h, "wifi_pass", pass);
        wifi_list_write(h);
        nvs_commit(h);
        nvs_close(h);
    }
}

bool config_forget_wifi(const char *ssid)
{
    int at = wifi_find(ssid);
    if (at < 0) return false;
    for (uint8_t i = (uint8_t)at; i + 1 < s_wifi_count; i++)
        s_wifi[i] = s_wifi[i + 1];
    memset(&s_wifi[--s_wifi_count], 0, sizeof(s_wifi[0]));

    /* the active network mirrors entry 0 (empty when nothing is left:
     * that is the "auto-open the setup QR" state on the next boot) */
    strlcpy(g_cfg.wifi_ssid, s_wifi_count ? s_wifi[0].ssid : "",
            sizeof(g_cfg.wifi_ssid));
    strlcpy(g_cfg.wifi_pass, s_wifi_count ? s_wifi[0].pass : "",
            sizeof(g_cfg.wifi_pass));

    nvs_handle_t h;
    if (nvs_open("pipcfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "wifi_ssid", g_cfg.wifi_ssid);
        nvs_set_str(h, "wifi_pass", g_cfg.wifi_pass);
        if (s_wifi_count) wifi_list_write(h);
        else              nvs_erase_key(h, "wifi_nets");
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "forgot network '%s', %u left", ssid, s_wifi_count);
    return true;
}

void config_save_volume(uint8_t v)     { g_cfg.speaker_volume = v; save_u32("volume", v); }
void config_save_brightness(uint8_t b) { g_cfg.brightness = b;     save_u32("brightness", b); }
void config_save_voice(bool enabled)   { g_cfg.voice_enabled = enabled;
                                         save_u32("voice_en", enabled ? 1 : 0); }

void config_save_theme(const char *name, bool black_text)
{
    strlcpy(g_cfg.theme_name, name, sizeof(g_cfg.theme_name));
    g_cfg.theme_black_text = black_text;
    save_str("theme", name);
    save_u32("theme_fg", black_text ? 1 : 0);
}

bool config_check_pin(const char *pin)
{
    if (!g_cfg.pin_hash[0]) return true;   /* no PIN provisioned yet */

    unsigned char digest[32];
    char buf[64];
    snprintf(buf, sizeof(buf), "%s%s", g_cfg.pin_salt, pin);
    mbedtls_sha256((const unsigned char *)buf, strlen(buf), digest, 0);

    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(&hex[i * 2], "%02x", digest[i]);
    return strcmp(hex, g_cfg.pin_hash) == 0;
}
