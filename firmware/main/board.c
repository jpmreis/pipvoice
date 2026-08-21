#include "board.h"
#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

/* AXP2101 PMU. The BSP has no PMU driver; register layout follows the
 * AXP2101 datasheet (cross-checked against XPowersLib as used by the
 * official Waveshare 90_axp2101_pmu example). */
#define AXP2101_ADDR             0x34
#define AXP2101_REG_STATUS1      0x00   /* bit3: battery present            */
#define AXP2101_REG_STATUS2      0x01   /* bits7:5 == 001 -> charging       */
#define AXP2101_REG_ADC_CTRL     0x30   /* per-channel ADC enable bits      */
#define AXP2101_REG_IPRECHG      0x61   /* bits1:0: precharge, 25 mA units  */
#define AXP2101_REG_ICC          0x62   /* bits4:0: constant charge current */
#define AXP2101_REG_ITERM        0x63   /* bits3:0: termination, 25 mA units*/
#define AXP2101_REG_CV           0x64   /* bits2:0: charge target voltage   */
#define AXP2101_REG_SOC          0xA4   /* battery percentage from gauge    */
#define AXP2101_REG_INTEN2       0x41   /* IRQ enable 2                     */
#define AXP2101_REG_INTSTS2      0x49   /* IRQ status 2, write-1-to-clear.
                                           Lower nibble = power key (bit0
                                           pos edge, bit1 neg edge, bit2
                                           long, bit3 short); upper nibble
                                           is vbus/battery events - seen
                                           empirically as 0x8b on a boot
                                           after USB replug + key press    */
#define PKEY_BITS                0x0F

#define BOOT_BTN_GPIO            0      /* BOOT side button, active low     */

static i2c_master_dev_handle_t s_pmu;
static bool s_touch_ok;

/* CO5300/SH8601 over QSPI requires 2-pixel-aligned flush windows */
static void rounder_cb(lv_area_t *area)
{
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
}

static uint8_t pmu_read(uint8_t reg, uint8_t dflt)
{
    if (!s_pmu) return dflt;
    uint8_t val = dflt;
    if (i2c_master_transmit_receive(s_pmu, &reg, 1, &val, 1, 100) != ESP_OK)
        return dflt;
    return val;
}

static void pmu_update(uint8_t reg, uint8_t clear_mask, uint8_t set_mask)
{
    if (!s_pmu) return;
    uint8_t cur = 0;
    if (i2c_master_transmit_receive(s_pmu, &reg, 1, &cur, 1, 100) != ESP_OK)
        return;
    uint8_t buf[2] = { reg, (uint8_t)((cur & ~clear_mask) | set_mask) };
    i2c_master_transmit(s_pmu, buf, sizeof(buf), 100);
}

static void pmu_init(void)
{
    /* ADC: enable vbat/vbus/vsys/die-temp measurement, disable the TS pin.
     * The recommended battery has no NTC; leaving TS measurement enabled
     * makes the charger misbehave. */
    pmu_update(AXP2101_REG_ADC_CTRL, 1u << 1, 0x1D);

    /* Charger limits for the recommended 400 mAh cell (same values as the
     * official PMU example): 50 mA precharge, 400 mA constant current,
     * 25 mA termination, 4.2 V target. */
    pmu_update(AXP2101_REG_IPRECHG, 0x03, 2);    /* 2 * 25 mA  */
    pmu_update(AXP2101_REG_ICC,     0x1F, 10);   /* 400 mA     */
    pmu_update(AXP2101_REG_ITERM,   0x0F, 1);    /* 1 * 25 mA  */
    pmu_update(AXP2101_REG_CV,      0x07, 3);    /* 4.2 V      */

    /* power-key events: some AXP2101 variants only latch IRQ status
     * bits that are enabled */
    pmu_update(AXP2101_REG_INTEN2, PKEY_BITS, PKEY_BITS);
}

void board_init(void)
{
    /* Panel reset (EXIO0), panel power (EXIO1) and touch reset (EXIO2)
     * hang off the TCA9554 expander. The BSP never drives them and the
     * power-on defaults leave touch held in reset ("Touch not found"
     * abort). Pulse low 20 ms then high, as Waveshare's board_variant.c
     * does before probing; SD_CS (EXIO7) stays high/inactive. */
    esp_io_expander_handle_t io_exp = bsp_io_expander_init();
    if (io_exp) {
        const uint32_t rst = IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 |
                             IO_EXPANDER_PIN_NUM_2;
        esp_io_expander_set_dir(io_exp, rst | IO_EXPANDER_PIN_NUM_7,
                                IO_EXPANDER_OUTPUT);
        esp_io_expander_set_level(io_exp, IO_EXPANDER_PIN_NUM_7, 1);
        esp_io_expander_set_level(io_exp, rst, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        esp_io_expander_set_level(io_exp, rst | IO_EXPANDER_PIN_NUM_7, 1);
        vTaskDelay(pdMS_TO_TICKS(150));
    } else {
        ESP_LOGW(TAG, "IO expander init failed; touch may stay in reset");
    }

    /* V1 board: SH8601 panel + FT3168 touch. BSP >=2.0.1 drives both HW
     * revisions through the CO5300 driver and auto-probes the touch chip
     * (CST816S on V2, FT5x06-compatible FT3168 on V1).
     *
     * We deliberately do NOT use bsp_display_start(): it registers the
     * panel via lvgl_port_add_disp_rgb, whose flush path signals
     * lv_disp_flush_ready immediately after queueing the (async) QSPI DMA
     * - LVGL then reuses the draw buffer mid-transfer and the screen
     * scrambles. Wire the same BSP primitives through lvgl_port_add_disp,
     * which waits for the panel IO's color-done callback, and get a
     * DMA-capable internal draw buffer while at it. */
    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    if (lvgl_port_init(&port_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port init failed");
        abort();
    }

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    if (bsp_display_new(NULL, &panel, &io) != ESP_OK || !panel || !io) {
        ESP_LOGE(TAG, "display init failed");
        abort();
    }

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = BSP_LCD_H_RES * 20,   /* pixels; ~15 KB internal DMA -
                                              * internal RAM is contended: the
                                              * audio task needs a contiguous
                                              * 32 KB stack after this */
        .double_buffer = false,
        .hres = BSP_LCD_H_RES,
        .vres = BSP_LCD_V_RES,
        .monochrome = false,
        .rounder_cb = rounder_cb,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = true,
            .sw_rotate = true,
            .swap_bytes = true,
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (!disp) {
        ESP_LOGE(TAG, "display start failed");
        abort();
    }

    esp_lcd_touch_handle_t tp = NULL;
    if (bsp_touch_new(NULL, &tp) == ESP_OK && tp) {
        const lvgl_port_touch_cfg_t touch_cfg = { .disp = disp, .handle = tp };
        lvgl_port_add_touch(&touch_cfg);
        s_touch_ok = true;
    } else {
        ESP_LOGW(TAG, "touch init failed; UI will be view-only");
    }

    board_set_brightness(200);

    /* PMU shares the BSP I2C bus */
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bsp_i2c_get_handle(), &dev, &s_pmu) != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101 not reachable; battery UI will show 100%%");
        s_pmu = NULL;
    }
    pmu_init();

    /* BOOT side button (strapping pin - safe as input after boot) */
    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << BOOT_BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn);

    /* clear any power-key event latched before boot */
    board_pwr_key_event();
}

bool board_touch_ok(void) { return s_touch_ok; }
bool board_pmu_ok(void)   { return s_pmu != NULL; }

bool board_lock(uint32_t timeout_ms) { return bsp_display_lock(timeout_ms); }
void board_unlock(void)              { bsp_display_unlock(); }

void board_set_brightness(uint8_t v)
{
    /* Brightness is a command on the panel's QSPI IO handle, which the LVGL
     * flush task also uses - serialize with the display lock. The lock is
     * recursive, so calls from LVGL callbacks are fine. */
    if (!bsp_display_lock(250)) {
        ESP_LOGW(TAG, "brightness: display lock timeout");
        return;
    }
    bsp_display_brightness_set((v * 100) / 255);  /* BSP takes percent */
    bsp_display_unlock();
}

bool board_pwr_key_event(void)
{
    if (!s_pmu) return false;
    uint8_t sts = pmu_read(AXP2101_REG_INTSTS2, 0);
    if (!(sts & PKEY_BITS)) return false;
    uint8_t buf[2] = { AXP2101_REG_INTSTS2, (uint8_t)(sts & PKEY_BITS) };
    i2c_master_transmit(s_pmu, buf, sizeof(buf), 100);          /* W1C */
    ESP_LOGI(TAG, "pwr key event, INTSTS2=0x%02x", sts);
    return true;
}

bool board_boot_btn_pressed(void)
{
    return gpio_get_level(BOOT_BTN_GPIO) == 0;
}

bool board_battery_present(void)
{
    /* default to "present" when the PMU is unreachable so the UI keeps
     * its old behaviour (battery shown at 100%) */
    return (pmu_read(AXP2101_REG_STATUS1, 1u << 3) & (1u << 3)) != 0;
}

uint8_t board_battery_pct(void)
{
    /* Gauge percentage is only meaningful with a battery attached;
     * report full when running on USB alone. */
    if (!board_battery_present())
        return 100;
    uint8_t soc = pmu_read(AXP2101_REG_SOC, 100);
    return soc > 100 ? 100 : soc;
}

bool board_battery_charging(void)
{
    return ((pmu_read(AXP2101_REG_STATUS2, 0) >> 5) & 0x07) == 0x01;
}
