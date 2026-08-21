/* net_check: internet-reachability probe. "Got an IP" is not "online" -
 * hotel/captive-portal networks hand out IPs but intercept traffic until
 * a browser sign-in. net_wifi kicks a probe after every GOT_IP and only
 * reports ONLINE once it passes. */
#pragma once
#include <stdbool.h>

typedef enum {
    NET_CHECK_ONLINE,     /* probe passed - real internet          */
    NET_CHECK_PORTAL,     /* an HTTP interceptor answered instead  */
} net_check_result_t;

typedef void (*net_check_cb_t)(net_check_result_t result);

void net_check_init(net_check_cb_t cb);
void net_check_kick(void);        /* probe now (after GOT_IP)             */
void net_check_stop(void);        /* link lost: stop probing              */
void net_check_watch(bool on);    /* keep re-probing every few seconds
                                     (relay mode: notice the sign-in)     */
/* Portal sign-in URL captured from the probe's redirect ("" if the
 * portal intercepts without redirecting). */
const char *net_check_portal_url(void);
