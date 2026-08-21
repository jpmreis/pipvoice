#include "provisioning.h"
#include "config.h"
#include "net_check.h"
#include "net_wifi.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "prov";

extern const char portal_html_start[] asm("_binary_portal_html_start");
extern const char portal_html_end[]   asm("_binary_portal_html_end");

static httpd_handle_t s_httpd;
static TaskHandle_t   s_dns_task;
static esp_netif_t   *s_ap_netif;
static volatile bool  s_running;
static volatile bool  s_dns_alive;
static volatile bool  s_relay_mode;   /* NAPT up, DNS forwards upstream */
static esp_timer_handle_t s_idle_timer;

#define SETUP_IDLE_TIMEOUT_US (10ULL * 60 * 1000 * 1000)

static void idle_timeout_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "setup mode idle timeout - restarting");
    esp_restart();
}

/* any portal activity re-arms the idle timer, so the device only reboots
 * when the user actually walked away */
static void idle_timer_touch(void)
{
    if (s_idle_timer)
        esp_timer_restart(s_idle_timer, SETUP_IDLE_TIMEOUT_US);
}

/* ---------------- relay: NAPT the phone out the STA side -----------------
 * Used whenever the STA has a link during setup: behind a captive portal
 * the phone's own connectivity checks surface the hotel's sign-in page,
 * and the sign-in authorizes Pip's MAC/IP because the traffic leaves
 * through Pip's STA interface. */
static bool relay_enable(void)
{
    if (s_relay_mode) return true;
    esp_netif_t *sta = net_wifi_netif();
    if (!sta || !s_ap_netif) return false;
    esp_netif_set_default_netif(sta);
    esp_err_t rc = esp_netif_napt_enable(s_ap_netif);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "napt enable: %s", esp_err_to_name(rc));
        return false;
    }
    s_relay_mode = true;          /* dns task switches to forwarding */
    net_check_watch(true);        /* poll until the sign-in unlocks us */
    net_check_kick();
    ESP_LOGI(TAG, "relay up - phone traffic now leaves via the STA side");
    return true;
}

static void relay_disable(void)
{
    if (!s_relay_mode) return;
    s_relay_mode = false;
    net_check_watch(false);
    if (s_ap_netif) esp_netif_napt_disable(s_ap_netif);
}

/* ---------------- DNS: hijack or forward -------------------------------
 * The AP's DHCP hands out 192.168.4.1 as resolver either way (default
 * dhcps behavior). In hijack mode every A query answers 192.168.4.1 so
 * the phone's captive check pops our setup page; in relay mode queries
 * are proxied to the STA's real resolver so the phone reaches the
 * network (and a hotel portal can intercept it). */
static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    /* timeout so the loop notices s_running=false; without it the task
     * blocks in recvfrom forever and leaks the port-53 socket */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 500 * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "dns bind failed (errno %d)", errno);
        close(sock);
        s_dns_alive = false;
        vTaskDelete(NULL);
        return;
    }

    int up = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct timeval utv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(up, SOL_SOCKET, SO_RCVTIMEO, &utv, sizeof(utv));

    static uint8_t buf[512];
    while (s_running) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int len = recvfrom(sock, buf, sizeof(buf) - 16, 0,
                           (struct sockaddr *)&src, &slen);
        if (len < 12) continue;

        esp_netif_dns_info_t dns;
        if (s_relay_mode &&
            esp_netif_get_dns_info(net_wifi_netif(), ESP_NETIF_DNS_MAIN,
                                   &dns) == ESP_OK &&
            dns.ip.u_addr.ip4.addr != 0) {
            /* forward to the network's real resolver, relay the answer */
            struct sockaddr_in upstream = {
                .sin_family = AF_INET,
                .sin_port = htons(53),
                .sin_addr.s_addr = dns.ip.u_addr.ip4.addr,
            };
            sendto(up, buf, len, 0, (struct sockaddr *)&upstream,
                   sizeof(upstream));
            int rlen = recvfrom(up, buf, sizeof(buf), 0, NULL, NULL);
            if (rlen > 0)
                sendto(sock, buf, rlen, 0, (struct sockaddr *)&src, slen);
        } else {
            /* hijack: answer every A query with 192.168.4.1. TTL kept
             * short so phones drop the poisoned entries within seconds
             * of the flip to relay mode (a long TTL left the phone's
             * browser stuck resolving everything to us for a minute). */
            buf[2] = 0x81; buf[3] = 0x80;          /* response, no error   */
            buf[6] = buf[4]; buf[7] = buf[5];      /* ancount = qdcount    */
            uint8_t answer[16] = { 0xC0, 0x0C,     /* ptr to question name */
                0x00, 0x01, 0x00, 0x01,            /* type A, class IN     */
                0x00, 0x00, 0x00, 0x05,            /* TTL 5s               */
                0x00, 0x04, 192, 168, 4, 1 };
            memcpy(buf + len, answer, sizeof(answer));
            sendto(sock, buf, len + sizeof(answer), 0,
                   (struct sockaddr *)&src, slen);
        }
    }
    close(up);
    close(sock);
    s_dns_alive = false;
    vTaskDelete(NULL);
}

/* stop portal services; safe to call in any state. Keeps trying until the
 * httpd really stopped - a leaked instance keeps port 80 bound and every
 * later setup attempt fails with EADDRINUSE. */
static void portal_teardown(void)
{
    if (s_idle_timer) {
        esp_timer_stop(s_idle_timer);
        esp_timer_delete(s_idle_timer);
        s_idle_timer = NULL;
    }
    s_running = false;
    if (s_httpd) {
        esp_err_t rc = ESP_FAIL;
        for (int i = 0; i < 5 && rc != ESP_OK; i++) {
            rc = httpd_stop(s_httpd);
            if (rc != ESP_OK) {
                ESP_LOGE(TAG, "httpd_stop: %s (attempt %d)",
                         esp_err_to_name(rc), i + 1);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
        s_httpd = NULL;
    }
    for (int i = 0; i < 15 && s_dns_alive; i++)
        vTaskDelay(pdMS_TO_TICKS(100));
    if (s_dns_alive)
        ESP_LOGE(TAG, "dns task did not exit");
    s_dns_task = NULL;
}

/* decode application/x-www-form-urlencoded in place: '+' -> ' ', %XX -> byte */
static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++, o++) {
        if (*s == '+') {
            *o = ' ';
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) &&
                                isxdigit((unsigned char)s[2])) {
            char hex[3] = { s[1], s[2], 0 };
            *o = (char)strtol(hex, NULL, 16);
            s += 2;
        } else {
            *o = *s;
        }
    }
    *o = 0;
}

/* ---------------- HTTP handlers ---------------- */
static esp_err_t root_get(httpd_req_t *req)
{
    idle_timer_touch();
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, portal_html_start,
                           portal_html_end - portal_html_start);
}

static esp_err_t scan_get(httpd_req_t *req)
{
    idle_timer_touch();
    wifi_scan_config_t sc = { .show_hidden = false };
    esp_wifi_scan_start(&sc, true);
    uint16_t n = 12;
    static wifi_ap_record_t recs[12];
    esp_wifi_scan_get_ap_records(&n, recs);

    /* n = name, o = open network (no password field on the page) */
    char json[768] = "[";
    size_t off = 1;
    for (uint16_t i = 0; i < n && off < sizeof(json) - 56; i++) {
        off += snprintf(json + off, sizeof(json) - off,
                        "%s{\"n\":\"%s\",\"o\":%d}", i ? "," : "",
                        (const char *)recs[i].ssid,
                        recs[i].authmode == WIFI_AUTH_OPEN ? 1 : 0);
    }
    strlcat(json, "]", sizeof(json));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t save_post(httpd_req_t *req)
{
    idle_timer_touch();
    char body[160] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) return httpd_resp_send_500(req);

    char ssid[33] = {0}, pass[65] = {0};
    httpd_query_key_value(body, "ssid", ssid, sizeof(ssid));
    httpd_query_key_value(body, "pass", pass, sizeof(pass));
    /* portal.html sends application/x-www-form-urlencoded; httpd_query_key_value
     * does no decoding, so undo %XX and '+' for both fields */
    url_decode(ssid);
    url_decode(pass);

    ESP_LOGI(TAG, "saving credentials for '%s', joining live", ssid);
    config_save_wifi(ssid, pass);
    httpd_resp_sendstr(req, "OK");

    /* no reboot: join the new network next to the running AP so the
     * phone stays connected. The page polls /status; if the network
     * turns out captive, app_main flips us into relay mode and this
     * same phone gets the sign-in page. */
    net_wifi_join_new();
    return ESP_OK;
}

/* setup progress for portal.html */
static esp_err_t status_get(httpd_req_t *req)
{
    idle_timer_touch();
    const char *st;
    bool portal = false;
    if (net_wifi_join_failed())      st = "failed";
    else switch (net_wifi_state()) {
        case NET_STATE_ONLINE:  st = "online"; break;
        case NET_STATE_PORTAL:  st = "portal"; portal = true; break;
        default: st = net_wifi_has_ip() ? "checking" : "connecting";
    }
    /* pass the portal's own sign-in URL along so the page can link it */
    const char *url = portal ? net_check_portal_url() : "";
    if (strchr(url, '"')) url = "";     /* keep the JSON well-formed */
    char json[224];
    snprintf(json, sizeof(json), "{\"state\":\"%s\",\"url\":\"%s\"}", st, url);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

/* ---------------- start/stop ---------------- */
bool provisioning_start(char *ap_ssid, char *ap_pass)
{
    if (s_running || s_httpd || s_dns_alive) {
        ESP_LOGW(TAG, "stale setup session found; cleaning up first");
        portal_teardown();
        relay_disable();
    }

    /* random 8-char password */
    static const char cs[] = "abcdefghjkmnpqrstuvwxyz23456789";
    for (int i = 0; i < 8; i++)
        ap_pass[i] = cs[esp_random() % (sizeof(cs) - 1)];
    ap_pass[8] = 0;
    snprintf(ap_ssid, 33, "Pip-%s", g_cfg.device_name);

    /* keep a live STA link (relay variant); without one, classic setup */
    bool keep_sta = net_wifi_has_ip();
    if (!keep_sta) net_wifi_stop();

    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_ap_netif) return false;

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.ap.ssid, ap_ssid, sizeof(wc.ap.ssid));
    strlcpy((char *)wc.ap.password, ap_pass, sizeof(wc.ap.password));
    wc.ap.ssid_len = strlen(ap_ssid);
    wc.ap.max_connection = 2;
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    /* no channel: in APSTA the AP sits on the STA's channel */

    /* mode first: AP config is rejected while no AP interface exists.
     * With a live STA the AP pops up on stale config for a few ms until
     * set_config lands - harmless. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    if (!keep_sta)
        ESP_ERROR_CHECK(esp_wifi_start());   /* already running otherwise */

    s_running = true;
    s_dns_alive = true;
    xTaskCreate(dns_task, "dns_portal", 4096, NULL, 3, &s_dns_task);

    /* with a live link, relay right away: a captive portal pops on the
     * phone by itself; the setup page stays at 192.168.4.1 */
    if (keep_sta && !relay_enable())
        ESP_LOGW(TAG, "relay unavailable; setup page still works by IP");

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.uri_match_fn = httpd_uri_match_wildcard;
    esp_err_t rc = httpd_start(&s_httpd, &hc);
    if (rc != ESP_OK) {
        /* undo everything so the next attempt starts clean */
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(rc));
        s_httpd = NULL;
        portal_teardown();
        relay_disable();
        if (keep_sta) {
            esp_wifi_set_mode(WIFI_MODE_STA);   /* drop AP, keep STA */
        } else {
            esp_wifi_stop();
            esp_wifi_set_mode(WIFI_MODE_STA);
            net_wifi_start();
        }
        return false;
    }

    static const httpd_uri_t u_scan   = { .uri = "/scan",   .method = HTTP_GET,  .handler = scan_get };
    static const httpd_uri_t u_save   = { .uri = "/save",   .method = HTTP_POST, .handler = save_post };
    static const httpd_uri_t u_status = { .uri = "/status", .method = HTTP_GET,  .handler = status_get };
    static const httpd_uri_t u_any    = { .uri = "/*",      .method = HTTP_GET,  .handler = root_get };
    httpd_register_uri_handler(s_httpd, &u_scan);
    httpd_register_uri_handler(s_httpd, &u_save);
    httpd_register_uri_handler(s_httpd, &u_status);
    httpd_register_uri_handler(s_httpd, &u_any);   /* captive: everything -> portal */

    const esp_timer_create_args_t targs = {
        .callback = idle_timeout_cb, .name = "prov_idle" };
    esp_timer_create(&targs, &s_idle_timer);
    esp_timer_start_once(s_idle_timer, SETUP_IDLE_TIMEOUT_US);

    ESP_LOGI(TAG, "AP '%s' up, portal at 192.168.4.1%s", ap_ssid,
             s_relay_mode ? " (relay)" : "");
    return true;
}

void provisioning_stop(void)
{
    ESP_LOGI(TAG, "setup mode exit");
    portal_teardown();
    relay_disable();
    if (net_wifi_has_ip()) {
        /* the STA is on a working (or portal-blocked) network - keep it */
        esp_wifi_set_mode(WIFI_MODE_STA);
    } else {
        esp_wifi_stop();
        esp_wifi_set_mode(WIFI_MODE_STA);
        net_wifi_start();
    }
}

void provisioning_relay_now(void)
{
    if (s_running) relay_enable();
}
