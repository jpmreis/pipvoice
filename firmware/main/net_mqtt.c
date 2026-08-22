#include "net_mqtt.h"
#include "config.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "mqtt";

static esp_mqtt_client_handle_t s_client;
static mqtt_notify_cb_t         s_cb;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    esp_mqtt_event_handle_t ev = data;
    switch ((esp_mqtt_event_id_t)id) {
        case MQTT_EVENT_CONNECTED: {
            char topic[64];
            snprintf(topic, sizeof(topic), "dev/%s/notify", g_cfg.device_id);
            esp_mqtt_client_subscribe(s_client, topic, 1);
            ESP_LOGI(TAG, "subscribed %s", topic);
            break;
        }
        case MQTT_EVENT_DATA: {
            /* 384: the largest event is now a new-message notify carrying
             * the sender, colour, timestamp and duration (~190 B with long
             * names); reaction events are ~140 B. Truncating mid-JSON is
             * not fatal - the parse fails and sync falls back to listing
             * the inbox - but it costs the fast path, so leave headroom.
             * This is the MQTT task's stack; keep it modest. */
            char payload[384] = {0};
            int n = ev->data_len;
            if (n > (int)sizeof(payload) - 1) n = sizeof(payload) - 1;
            if (n > 0) memcpy(payload, ev->data, n);
            ESP_LOGI(TAG, "notify received: %s", payload);
            if (s_cb) s_cb(payload);
            break;
        }
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "disconnected");   /* client auto-reconnects */
            break;
        default:
            break;
    }
}

void net_mqtt_init(mqtt_notify_cb_t cb) { s_cb = cb; }

void net_mqtt_start(void)
{
    if (s_client || !g_cfg.mqtt_host[0]) return;

    char uri[96];
    snprintf(uri, sizeof(uri), "mqtts://%s:%u", g_cfg.mqtt_host, g_cfg.mqtt_port);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.username = g_cfg.mqtt_user,
        .credentials.authentication.password = g_cfg.mqtt_pass,
        .credentials.client_id = g_cfg.device_id,
        .session.keepalive = 60,
    };
    s_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_mqtt_client_start(s_client);
}

void net_mqtt_stop(void)
{
    if (!s_client) return;
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
}

void net_mqtt_publish_presence(const char *to_username, bool start)
{
    /* enqueue (not publish): non-blocking and thread-safe, so this is
     * allowed from UI callbacks and the audio task - the MQTT task does
     * the actual I/O. QoS 0: presence is refreshed every 10 s anyway. */
    if (!s_client) return;                 /* offline: no presence, fine */
    char topic[64], payload[96];
    snprintf(topic, sizeof(topic), "presence/%s", g_cfg.device_id);
    snprintf(payload, sizeof(payload), "{\"to\":\"%s\",\"state\":\"%s\"}",
             to_username, start ? "start" : "stop");
    esp_mqtt_client_enqueue(s_client, topic, payload, 0, 0, 0, true);
}
