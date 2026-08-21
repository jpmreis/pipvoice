/* config: identity, secrets and tunables from NVS (namespace "pipcfg").
 * Provisioned at flash time from the admin-generated NVS CSV via
 * nvs_partition_gen (see flash_device.sh). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define CFG_MAX_WIFI 8

typedef struct {
    char ssid[33];
    char pass[65];
} wifi_cred_t;

typedef struct {
    char device_id[32];
    char device_name[24];
    char server_base[96];      /* https://voice.example.com/v1 */
    char auth_token[64];
    char mqtt_host[64];
    uint16_t mqtt_port;
    char mqtt_user[32];
    char mqtt_pass[64];
    char pin_salt[24];
    char pin_hash[65];         /* hex sha256(salt+pin) */
    char wifi_ssid[33];
    char wifi_pass[65];
    uint16_t max_message_s;
    uint8_t  speaker_volume;   /* 0..100 */
    uint8_t  brightness;       /* 0..255 */
    char     theme_name[24];   /* background theme; "" = none (black)      */
    bool     theme_black_text; /* text drawn directly on the background   */
} app_config_t;

extern app_config_t g_cfg;

void config_load(void);

/* True iff this box is provisioned against the hosted instance
 * (pipvoice.com) rather than a self-hosted server. The single place
 * firmware may branch hosted-vs-self-hosted; server twin: db.hosted(). */
bool config_is_hosted(void);

/* WiFi credential store: every network ever used, most-recent-first.
 * config_save_wifi() upserts to the front and becomes the active network
 * (g_cfg.wifi_ssid/pass mirror entry 0). No network is seeded by default:
 * a box with zero networks auto-opens the WiFi-setup QR. */
uint8_t config_wifi_list(wifi_cred_t *out, uint8_t cap);   /* returns count */
void config_save_wifi(const char *ssid, const char *pass);
void config_save_volume(uint8_t v);
void config_save_brightness(uint8_t b);
void config_save_theme(const char *name, bool black_text);
bool config_check_pin(const char *pin);
