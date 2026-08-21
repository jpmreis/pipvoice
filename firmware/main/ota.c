#include "ota.h"
#include "config.h"
#include "net_wifi.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "ota";
#define CHECK_INTERVAL_MS (24 * 60 * 60 * 1000)

/* the binary endpoint requires the same bearer token as the manifest */
static esp_err_t ota_http_init_cb(esp_http_client_handle_t http)
{
    char auth[96];
    snprintf(auth, sizeof(auth), "Bearer %s", g_cfg.auth_token);
    return esp_http_client_set_header(http, "Authorization", auth);
}

static void check_once(void)
{
    char url[128];
    snprintf(url, sizeof(url), "%s/firmware", g_cfg.server_base);
    esp_http_client_config_t cfg = {
        .url = url, .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return;
    char auth[96];
    snprintf(auth, sizeof(auth), "Bearer %s", g_cfg.auth_token);
    esp_http_client_set_header(c, "Authorization", auth);

    char body[512] = {0};
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        int total = 0, r;   /* read to EOF: one read may return early on TLS */
        while ((r = esp_http_client_read(c, body + total,
                                         (int)sizeof(body) - 1 - total)) > 0)
            total += r;
        body[total > 0 ? total : 0] = 0;
        esp_http_client_close(c);
    }
    esp_http_client_cleanup(c);
    if (!body[0]) return;

    cJSON *root = cJSON_Parse(body);
    if (!root) return;
    cJSON *ver = cJSON_GetObjectItem(root, "version");
    cJSON *fw  = cJSON_GetObjectItem(root, "url");
    const esp_app_desc_t *running = esp_app_get_description();

    if (cJSON_IsString(ver) && cJSON_IsString(fw) &&
        strcmp(ver->valuestring, running->version) != 0) {
        ESP_LOGI(TAG, "updating %s -> %s", running->version, ver->valuestring);
        esp_http_client_config_t ota_http = {
            .url = fw->valuestring,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        esp_https_ota_config_t ota_cfg = {
            .http_config = &ota_http,
            .http_client_init_cb = ota_http_init_cb,
        };
        if (esp_https_ota(&ota_cfg) == ESP_OK) {
            ESP_LOGI(TAG, "update ok, rebooting");
            esp_restart();
        }
    }
    cJSON_Delete(root);
}

static TaskHandle_t s_task;

static void ota_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(60 * 1000));       /* settle after boot */
    for (;;) {
        if (net_wifi_is_connected()) check_once();
        /* daily check, or earlier when ota_kick() delivers a notify */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CHECK_INTERVAL_MS));
    }
}

void ota_init(void)
{
    if (xTaskCreate(ota_task, "ota", 8192, NULL, 1, &s_task) != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "ota task create FAILED - no auto-update");
    }
}

void ota_kick(void)
{
    if (s_task) xTaskNotifyGive(s_task);
}
