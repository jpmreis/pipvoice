#include "net_check.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "netcheck";

/* Plain HTTP on purpose: a portal cannot intercept HTTPS without cert
 * errors, but it happily rewrites plain HTTP - any answer other than the
 * expected one means a portal grabbed the request. */
#define PROBE_URL_204   "http://connectivitycheck.gstatic.com/generate_204"
#define PROBE_URL_APPLE "http://captive.apple.com/hotspot-detect.html"

#define RETRY_MS           5000   /* portal state / transient failures    */
#define TRANSPORT_RETRIES  3      /* then fail open (see below)           */

static net_check_cb_t    s_cb;
static SemaphoreHandle_t s_lock;
static TaskHandle_t      s_task;
static volatile bool     s_active;
static volatile bool     s_watch;
static char              s_portal_url[160];   /* portal's redirect target */

const char *net_check_portal_url(void) { return s_portal_url; }

/* returns HTTP status, or <0 on transport failure (DNS/TCP/timeout);
 * on a 30x, redir (if given) receives the Location target */
static int http_probe(const char *url, char *body, size_t cap,
                      char *redir, size_t rcap)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 8000,
        .disable_auto_redirect = true,   /* a 30x IS the signal */
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;

    int status = -1;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        if (body) {
            int total = 0, r;
            while ((r = esp_http_client_read(c, body + total,
                                             (int)(cap - 1 - total))) > 0)
                total += r;
            body[total] = 0;
        }
        status = esp_http_client_get_status_code(c);
        if (redir && status >= 300 && status < 400 &&
            esp_http_client_set_redirection(c) == ESP_OK)
            esp_http_client_get_url(c, redir, rcap);
        esp_http_client_close(c);
    }
    esp_http_client_cleanup(c);
    return status;
}

/* On-demand task: created by a kick, deletes itself once a final verdict
 * is delivered. Internal RAM is too tight to park 6 KB of stack here
 * permanently - and the probe window (GOT_IP -> verdict) is exactly when
 * sync/MQTT are not competing for the heap yet. */
static void check_task(void *arg)
{
    (void)arg;
top:;
    uint8_t soft_fails = 0;
    bool again = true;                /* the kick that woke us: probe now */
    while (s_active && (again || s_watch)) {
        net_check_result_t res;
        int status = http_probe(PROBE_URL_204, NULL, 0,
                                s_portal_url, sizeof(s_portal_url));
        if (status == 204) {
            res = NET_CHECK_ONLINE;
        } else if (status > 0) {
            res = NET_CHECK_PORTAL;
        } else {
            /* transport failure: some portals firewall unknown hosts
             * instead of redirecting - try a second, differently-hosted
             * probe before concluding anything */
            static char body[600];
            status = http_probe(PROBE_URL_APPLE, body, sizeof(body), NULL, 0);
            if (status == 200 && strstr(body, "Success")) {
                res = NET_CHECK_ONLINE;
            } else if (status > 0) {
                res = NET_CHECK_PORTAL;
            } else if (++soft_fails < TRANSPORT_RETRIES) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(RETRY_MS));
                continue;
            } else {
                /* fail open: both probe hosts unreachable is far more
                 * likely a dead uplink than both being down; behave like
                 * the pre-netcheck firmware rather than blocking sync
                 * forever on a probe-host outage */
                ESP_LOGW(TAG, "probes unreachable - assuming online");
                res = NET_CHECK_ONLINE;
            }
        }
        soft_fails = 0;
        again = (res == NET_CHECK_PORTAL);
        if (res == NET_CHECK_ONLINE) s_portal_url[0] = 0;
        ESP_LOGI(TAG, "probe: %s (http %d)",
                 res == NET_CHECK_ONLINE ? "online" : "captive portal",
                 status);
        if (s_active && s_cb) s_cb(res);
        if (s_active && (again || s_watch))
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(RETRY_MS));
    }

    /* free the stack - unless a kick raced our exit */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (ulTaskNotifyTake(pdTRUE, 0) > 0 && s_active) {
        xSemaphoreGive(s_lock);
        goto top;
    }
    s_task = NULL;
    xSemaphoreGive(s_lock);
    vTaskDelete(NULL);
}

void net_check_init(net_check_cb_t cb)
{
    s_cb = cb;
    s_lock = xSemaphoreCreateMutex();
}

void net_check_kick(void)
{
    s_active = true;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_task) {
        xTaskNotifyGive(s_task);
    } else if (xTaskCreate(check_task, "net_check", 6144, NULL, 3,
                           &s_task) != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "probe task create failed - assuming online");
        if (s_cb) s_cb(NET_CHECK_ONLINE);   /* fail open, never block sync */
    }
    xSemaphoreGive(s_lock);
}

void net_check_stop(void) { s_active = false; }

void net_check_watch(bool on) { s_watch = on; }
