/* Pip UI - WiFi setup: QR to join the device's AP. What the phone sees
 * depends on the device's state (provisioning.c): with no connection it
 * gets the setup portal; with a live connection it is relayed onto the
 * network, so a hotel sign-in pops by itself and 192.168.4.1 still
 * serves the setup page. */
#include "ui_internal.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t *s_scr;
static lv_obj_t *s_qr;
static lv_obj_t *s_ssid_lbl;
static lv_obj_t *s_pass_lbl;

static void exit_clicked(lv_event_t *e)
{
    LV_UNUSED(e);
    if (g_ui.cb.wifi_setup_exit) g_ui.cb.wifi_setup_exit();
    nav_to(SCR_HOME, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
}

lv_obj_t *scr_wifi_setup_create(void)
{
    s_scr = mk_screen();
    mk_header(s_scr, "WiFi setup", true, exit_clicked);

    lv_obj_t *hint = lv_label_create(s_scr);
    lv_label_set_text(hint, "1. Scan with your phone\n"
                            "2. Follow the page that opens\n"
                            "No page? Open 192.168.4.1");
    lv_obj_set_style_text_font(hint, FONT_SMALL, 0);
    lv_obj_set_style_text_color(hint, COL_TEXT_DIM, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 58);

#if LV_USE_QRCODE
    s_qr = lv_qrcode_create(s_scr);
    lv_qrcode_set_size(s_qr, 210);
    lv_qrcode_set_dark_color(s_qr, COL_BG);
    lv_qrcode_set_light_color(s_qr, COL_TEXT);
    lv_obj_align(s_qr, LV_ALIGN_CENTER, 0, 24);
    /* white quiet zone so scanners lock on against the black screen */
    lv_obj_set_style_border_color(s_qr, COL_TEXT, 0);
    lv_obj_set_style_border_width(s_qr, 8, 0);
#endif

    s_ssid_lbl = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_ssid_lbl, FONT_BODY, 0);
    lv_obj_align(s_ssid_lbl, LV_ALIGN_BOTTOM_MID, 0, -46);

    s_pass_lbl = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_pass_lbl, FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_pass_lbl, COL_TEXT_DIM, 0);
    lv_obj_align(s_pass_lbl, LV_ALIGN_BOTTOM_MID, 0, -18);

    return s_scr;
}

void scr_wifi_setup_show(const char *ssid, const char *pass)
{
#if LV_USE_QRCODE
    char payload[160];
    snprintf(payload, sizeof(payload), "WIFI:T:WPA;S:%s;P:%s;;", ssid, pass);
    lv_qrcode_update(s_qr, payload, (uint32_t)strlen(payload));
#endif
    lv_label_set_text_fmt(s_ssid_lbl, "Network: %s", ssid);
    lv_label_set_text_fmt(s_pass_lbl, "Password: %s", pass);
}
