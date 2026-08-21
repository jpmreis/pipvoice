/* net_mqtt: subscribes to dev/{id}/notify; fires callback on any publish
 * with the (NUL-terminated, possibly truncated) payload. */
#pragma once
#include <stdbool.h>

typedef void (*mqtt_notify_cb_t)(const char *payload);

void net_mqtt_init(mqtt_notify_cb_t cb);
void net_mqtt_start(void);      /* call once WiFi is up */
void net_mqtt_stop(void);

/* "this device is recording to <user>" - QoS-0 enqueue on the device's
 * own presence/<id> topic (broker ACL allows only that); silently a
 * no-op while offline. Safe from any task, including UI callbacks. */
void net_mqtt_publish_presence(const char *to_username, bool start);
