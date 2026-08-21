/* ota: daily firmware update check against the VPS manifest. */
#pragma once
void ota_init(void);   /* spawns a low-priority daily check task */
void ota_kick(void);   /* check now (server pushed a firmware notify) */
