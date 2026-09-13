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

/* Microphone front end. The 1.8 records through the ES8311's single
 * analog mic input; the 1.75-B and 2.16 have an ES7210 ADC with a
 * two-MEMS array, read as one stereo I2S stream (L = MIC1, R = MIC2)
 * and folded to mono by the dual-mic front end in audio.c. */
#if defined(PIP_BOARD_AMOLED_1_75B) || defined(PIP_BOARD_AMOLED_2_16)
#define PIP_MIC_CHANNELS 2
/* Wake-word cutoff for the ES7210 array. The embedded model cutoff
 * (0.45) was set on the 1.8's analog mic; on the 1.75-B bench
 * (2026-09-13, 4 sessions) clean "Hey Pip" scored 0.45-0.58 with a
 * long tail of 0.21-0.35 that the 0.45 cutoff dropped, while ordinary
 * speech and sound-alikes never exceeded 0.11. 0.30 catches the tail
 * with ~3x margin over the false-wake floor. The confirm model keeps
 * its embedded cutoff. The 2.16 has the same mic hardware but is not
 * benched yet - revisit when it is. */
#define PIP_WAKE_CUTOFF 0.30f
#else
#define PIP_MIC_CHANNELS 1
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
