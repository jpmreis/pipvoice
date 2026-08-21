/* power: screen dim/off on inactivity, wake on touch, battery polling. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef void (*battery_cb_t)(uint8_t pct, bool charging, bool present);

void power_init(battery_cb_t bat_cb);
void power_user_activity(void);      /* call on any wake source (touch/IMU) */
void power_set_brightness(uint8_t v);/* new "full" brightness (settings)    */
void power_hold(bool on);            /* keep screen awake (recording/play)  */
