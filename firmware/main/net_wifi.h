/* net_wifi: station-mode manager with reconnect backoff.
 * "Connected" means the internet probe passed (net_check), not just that
 * an AP handed us an IP - captive-portal networks report NET_STATE_PORTAL. */
#pragma once
#include "esp_netif.h"
#include <stdbool.h>

typedef enum {
    NET_STATE_DOWN,      /* no association / lost IP                     */
    NET_STATE_PORTAL,    /* on WiFi but a captive portal blocks traffic  */
    NET_STATE_ONLINE,    /* internet reachable                           */
} net_state_t;

typedef void (*wifi_state_cb_t)(net_state_t state);

void net_wifi_init(wifi_state_cb_t cb);
void net_wifi_start(void);            /* connect using NVS creds            */
void net_wifi_stop(void);             /* for provisioning mode              */
bool net_wifi_is_connected(void);     /* online (probe passed)              */
bool net_wifi_has_ip(void);           /* associated + DHCP done (any state) */
net_state_t net_wifi_state(void);     /* last reported state                */
esp_netif_t *net_wifi_netif(void);    /* STA netif (for the setup relay)    */

/* Live-join for WiFi setup: connect the STA to the newest saved network
 * right now, while the setup AP stays up (no reboot). Rotation to other
 * known networks is disabled so a wrong password can't silently land the
 * device back on the old network mid-setup. */
void net_wifi_join_new(void);
bool net_wifi_join_failed(void);      /* live-join gave up (bad password?)  */

/* User activity while offline: skip the remaining backoff and retry now
 * (phone hotspots stop beaconing when idle and wake with their owner). */
void net_wifi_retry_now(void);
