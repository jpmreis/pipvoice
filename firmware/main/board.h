/* board: thin wrapper over the Waveshare BSP + AXP2101 fuel gauge. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

/* Hardware model this image is compiled for (server boards.py key).
 * Printed in the boot PIP-HW line so the web flasher can detect a
 * wrong-model flash, and compared against the NVS `board` key the
 * server bakes at provisioning. Per-model builds will override this
 * from CMake; until then there is exactly one. */
#ifndef PIP_BOARD_NAME
#define PIP_BOARD_NAME "amoled-1.8"
#endif

void board_init(void);                         /* display+touch+LVGL up      */
bool board_lock(uint32_t timeout_ms);          /* LVGL mutex                 */
void board_unlock(void);
void board_set_brightness(uint8_t val_0_255);
uint8_t board_battery_pct(void);
bool board_battery_charging(void);
bool board_battery_present(void);
bool board_pwr_key_event(void);    /* PMU short-press since last call     */
bool board_boot_btn_pressed(void); /* BOOT button currently held          */
bool board_touch_ok(void);         /* probe results for the PIP-HW line   */
bool board_pmu_ok(void);
