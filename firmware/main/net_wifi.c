#include "net_wifi.h"
#include "config.h"
#include "net_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include <string.h>

static const char *TAG = "wifi";

static wifi_state_cb_t s_cb;
static bool            s_connected;      /* probe passed - really online */
static bool            s_has_ip;
static net_state_t     s_last = NET_STATE_DOWN;
static bool            s_enabled;
static uint32_t        s_backoff_ms = 1000;
static TimerHandle_t   s_retry_timer;
static esp_netif_t    *s_sta_netif;

/* known networks (config store, most-recent-first); rotate on failures */
static wifi_cred_t s_nets[CFG_MAX_WIFI];
static uint8_t     s_net_count, s_net_idx, s_net_fails;
static bool        s_pinned;         /* live-join: stick to slot 0 only */
static bool        s_pin_failed;
#define NET_FAILS_BEFORE_ROTATE 2
#define PIN_FAILS_BEFORE_GIVEUP 5

bool net_wifi_is_connected(void) { return s_connected; }
bool net_wifi_has_ip(void)       { return s_has_ip; }
net_state_t net_wifi_state(void) { return s_last; }
bool net_wifi_join_failed(void)  { return s_pin_failed; }
esp_netif_t *net_wifi_netif(void){ return s_sta_netif; }

/* report a state to the app only on transitions (disconnect storms and
 * the portal re-probe loop would otherwise spam toasts/MQTT restarts) */
static void report(net_state_t st)
{
    if (st == s_last) return;
    s_last = st;
    s_connected = (st == NET_STATE_ONLINE);
    if (s_cb) s_cb(st);
}

/* probe verdict from the net_check task */
static void on_net_check(net_check_result_t r)
{
    if (!s_enabled || !s_has_ip) return;   /* link went away meanwhile */
    report(r == NET_CHECK_ONLINE ? NET_STATE_ONLINE : NET_STATE_PORTAL);
}

static void apply_net(void)
{
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, s_nets[s_net_idx].ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, s_nets[s_net_idx].pass,
            sizeof(wc.sta.password));
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    ESP_LOGI(TAG, "using network '%s' (%u of %u)",
             s_nets[s_net_idx].ssid, s_net_idx + 1, s_net_count);
}

static void retry_cb(TimerHandle_t t)
{
    (void)t;
    if (s_enabled) esp_wifi_connect();
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_has_ip = false;
        net_check_stop();
        report(NET_STATE_DOWN);
        if (s_enabled) {
            s_net_fails++;
            if (s_pinned) {
                /* live-join: never rotate away from the network being set
                 * up, but do tell the setup page when it looks hopeless */
                if (s_net_fails >= PIN_FAILS_BEFORE_GIVEUP)
                    s_pin_failed = true;
            } else if (s_net_fails >= NET_FAILS_BEFORE_ROTATE &&
                       s_net_count > 1) {
                /* after a couple of failures, try the next known network -
                 * a box moved to another house finds its way home by itself */
                s_net_fails = 0;
                s_net_idx = (s_net_idx + 1) % s_net_count;
                apply_net();
            }
            ESP_LOGI(TAG, "retry in %lu ms", (unsigned long)s_backoff_ms);
            xTimerChangePeriod(s_retry_timer, pdMS_TO_TICKS(s_backoff_ms), 0);
            xTimerStart(s_retry_timer, 0);
            /* flat quick retries during live-join: someone is watching.
             * Otherwise cap at 60 s - a phone hotspot that stopped
             * beaconing and came back must not look dead for minutes. */
            s_backoff_ms = s_pinned ? 3000
                         : s_backoff_ms >= 30000 ? 60000 : s_backoff_ms * 2;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_has_ip = true;
        s_backoff_ms = 1000;
        s_net_fails = 0;
        s_pinned = false;         /* joined: normal rotation resumes */
        s_pin_failed = false;
        ESP_LOGI(TAG, "got IP - probing for internet");
        if (s_net_idx != 0 && s_net_idx < s_net_count) {
            /* promote the network that actually worked to most-recent */
            config_save_wifi(s_nets[s_net_idx].ssid, s_nets[s_net_idx].pass);
            s_net_count = config_wifi_list(s_nets, CFG_MAX_WIFI);
            s_net_idx = 0;
        }
        static bool sntp_started;   /* init once; re-init fails on reconnect */
        if (!sntp_started) {
            esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            if (esp_netif_sntp_init(&sntp) == ESP_OK) sntp_started = true;
        }
        /* ONLINE (or PORTAL) is reported once the probe answers */
        net_check_kick();
    }
}

void net_wifi_init(wifi_state_cb_t cb)
{
    s_cb = cb;
    net_check_init(on_net_check);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    /* DHCP hostname (router client lists) - default is "espressif" */
    if (g_cfg.device_id[0])
        esp_netif_set_hostname(s_sta_netif, g_cfg.device_id);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               on_wifi_event, NULL));
    s_retry_timer = xTimerCreate("wifi_retry", pdMS_TO_TICKS(1000), pdFALSE,
                                 NULL, retry_cb);
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));   /* modem sleep    */
}

void net_wifi_start(void)
{
    s_net_count = config_wifi_list(s_nets, CFG_MAX_WIFI);
    if (s_net_count == 0) {
        ESP_LOGW(TAG, "no credentials; provisioning needed");
        return;
    }
    s_net_idx = 0;
    s_net_fails = 0;
    s_pinned = false;
    s_pin_failed = false;

    s_enabled = true;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    apply_net();
    ESP_ERROR_CHECK(esp_wifi_start());
}

void net_wifi_stop(void)
{
    s_enabled = false;
    xTimerStop(s_retry_timer, 0);
    net_check_stop();
    esp_wifi_stop();
    s_has_ip = false;
    s_pinned = false;
    s_pin_failed = false;
    report(NET_STATE_DOWN);
}

/* Someone touched the device: a sleeping hotspot may be back - retry
 * right away instead of waiting out the backoff. Timer-queue call only,
 * safe from any task (including the LVGL wake path). */
void net_wifi_retry_now(void)
{
    if (!s_enabled || s_has_ip) return;
    s_backoff_ms = 1000;
    xTimerChangePeriod(s_retry_timer, 1, 0);   /* fires immediately */
}

/* Setup saved a new network: connect to it now, alongside the running
 * setup AP (the wifi driver is already started, in APSTA). */
void net_wifi_join_new(void)
{
    s_net_count = config_wifi_list(s_nets, CFG_MAX_WIFI);
    s_net_idx = 0;
    s_net_fails = 0;
    s_pinned = true;
    s_pin_failed = false;
    s_backoff_ms = 1000;
    s_enabled = true;
    if (s_has_ip)
        esp_wifi_disconnect();   /* handler retries onto the new config */
    apply_net();
    esp_wifi_connect();          /* harmless error if still disconnecting */
}
