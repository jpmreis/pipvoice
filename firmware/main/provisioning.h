/* provisioning: PIN-protected WiFi setup mode - one flow that adapts:
 *  - no connection: SoftAP + DNS hijack (all names -> 192.168.4.1) + a
 *    tiny HTTP server (portal.html, /scan, /save, /status). Saving a
 *    network joins it live - no reboot, the phone stays on the AP.
 *  - STA has an IP (online or captive-portal-blocked): same AP + server,
 *    but DNS forwards upstream and NAPT relays the phone out the STA
 *    side, so a hotel sign-in page pops on the phone by itself (and
 *    authorizes Pip's MAC). 192.168.4.1 still serves the setup page.
 * app_main bridges the two: a PORTAL verdict during setup calls
 * provisioning_relay_now(); ONLINE closes the whole flow. */
#pragma once
#include <stdbool.h>

/* Fills ap_ssid (>=33) and ap_pass (>=65) for the on-screen QR code. */
bool provisioning_start(char *ap_ssid, char *ap_pass);
void provisioning_stop(void);
void provisioning_relay_now(void);  /* new network turned out captive */
