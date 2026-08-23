/* Pip UI - splash: animated "Pip" wordmark + "Hello <owner>" greeting.
 * Shown at boot and when waking from full display-off (see ui_splash_show),
 * and as the landing half of the ambient touch-wake, where the wandering
 * amber dot glides home into the wordmark (scr_splash_show_from). */
#include "ui_internal.h"

#define SPLASH_TOTAL_MS  UI_SPLASH_MS   /* app_main waits this out */
#define DOT_REST_TY      12      /* dot settles at the wordmark baseline */

static lv_obj_t   *s_scr;
static lv_obj_t   *s_letters[3];
static lv_obj_t   *s_dot;
static lv_obj_t   *s_hello;
static lv_timer_t *s_exit_timer;

/* ------------- anim plumbing ------------- */
static void a_tx(void *o, int32_t v)  { lv_obj_set_style_translate_x(o, v, 0); }
static void a_ty(void *o, int32_t v)  { lv_obj_set_style_translate_y(o, v, 0); }
static void a_opa(void *o, int32_t v) { lv_obj_set_style_opa(o, v, 0); }

/* landing squash for the glide-home dot: one stretch-flat-and-recover,
 * value is 0..300 ms into the curve (pivot: dot's bottom center) */
static void a_settle(void *o, int32_t v)
{
    int32_t sx, sy;
    if (v <= 120) { sx = 100 + (22 * v) / 120;         sy = 100 - (20 * v) / 120; }
    else          { sx = 122 - (22 * (v - 120)) / 180; sy = 80 + (20 * (v - 120)) / 180; }
    lv_obj_set_style_transform_scale_x(o, (sx * 256) / 100, 0);
    lv_obj_set_style_transform_scale_y(o, (sy * 256) / 100, 0);
}

static void anim(lv_obj_t *o, int32_t start, int32_t end, uint32_t delay,
                 uint32_t dur, lv_anim_exec_xcb_t exec, lv_anim_path_cb_t path)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_values(&a, start, end);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_duration(&a, dur);
    lv_anim_set_exec_cb(&a, exec);
    lv_anim_set_path_cb(&a, path);
    lv_anim_start(&a);
}

static void exit_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    s_exit_timer = NULL;
    if (lv_screen_active() == s_scr)     /* unless a button navigated away */
        nav_to(SCR_HOME, LV_SCR_LOAD_ANIM_FADE_ON);
}

/* ------------- build ------------- */
lv_obj_t *scr_splash_create(void)
{
    s_scr = mk_screen();

    lv_obj_t *cont = lv_obj_create(s_scr);
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(cont, 3, 0);
    lv_obj_align(cont, LV_ALIGN_CENTER, 0, -28);

    static const char *txt[3] = { "P", "i", "p" };
    for (int i = 0; i < 3; i++) {
        s_letters[i] = lv_label_create(cont);
        lv_label_set_text(s_letters[i], txt[i]);
        lv_obj_set_style_text_font(s_letters[i], FONT_BIG, 0);
        lv_obj_set_style_text_color(s_letters[i], COL_TEXT, 0);
        lv_obj_set_style_opa(s_letters[i], LV_OPA_TRANSP, 0);
    }

    s_dot = lv_obj_create(cont);
    lv_obj_remove_style_all(s_dot);
    lv_obj_set_size(s_dot, 14, 14);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_dot, COL_ACCENT, 0);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(s_dot, LV_OPA_TRANSP, 0);
    lv_obj_set_style_transform_pivot_x(s_dot, 7, 0);
    lv_obj_set_style_transform_pivot_y(s_dot, 14, 0);

    s_hello = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_hello, FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_hello, COL_TEXT_DIM, 0);
    lv_obj_set_style_opa(s_hello, LV_OPA_TRANSP, 0);
    lv_label_set_text(s_hello, "Hello");
    lv_obj_align(s_hello, LV_ALIGN_BOTTOM_MID, 0, -56);

    return s_scr;
}

/* ------------- show: reset + arm the animations ------------- */

/* everything except the dot: letters drop in, greeting fades up, exit
 * timer re-arms - shared by both entries below */
static void show_base(void)
{
    if (g_ui.owner_name[0])
        lv_label_set_text_fmt(s_hello, "Hello %s", g_ui.owner_name);
    lv_obj_align(s_hello, LV_ALIGN_BOTTOM_MID, 0, -56);

    /* letters drop in from above, staggered, with a springy overshoot */
    for (int i = 0; i < 3; i++) {
        lv_obj_set_style_opa(s_letters[i], LV_OPA_TRANSP, 0);
        uint32_t d = 80 + (uint32_t)i * 140;
        anim(s_letters[i], -240, 0, d, 450, a_ty, lv_anim_path_overshoot);
        anim(s_letters[i], LV_OPA_TRANSP, LV_OPA_COVER, d, 180, a_opa,
             lv_anim_path_linear);
    }

    /* greeting fades up from the bottom */
    lv_obj_set_style_opa(s_hello, LV_OPA_TRANSP, 0);
    anim(s_hello, 24, 0, 1000, 450, a_ty, lv_anim_path_ease_out);
    anim(s_hello, LV_OPA_TRANSP, LV_OPA_COVER, 1000, 450, a_opa,
         lv_anim_path_linear);

    if (s_exit_timer) lv_timer_delete(s_exit_timer);
    s_exit_timer = lv_timer_create(exit_cb, SPLASH_TOTAL_MS, NULL);
    lv_timer_set_repeat_count(s_exit_timer, 1);
}

/* a previous show may have left translate/scale on the dot */
static void dot_style_reset(void)
{
    lv_anim_delete(s_dot, NULL);
    lv_obj_set_style_translate_x(s_dot, 0, 0);
    lv_obj_set_style_transform_scale_x(s_dot, LV_SCALE_NONE, 0);
    lv_obj_set_style_transform_scale_y(s_dot, LV_SCALE_NONE, 0);
}

void scr_splash_show(void)
{
    show_base();

    /* the amber dot bounces down and settles as the wordmark's period */
    dot_style_reset();
    lv_obj_set_style_opa(s_dot, LV_OPA_TRANSP, 0);
    anim(s_dot, -260, DOT_REST_TY, 560, 650, a_ty, lv_anim_path_bounce);
    anim(s_dot, LV_OPA_TRANSP, LV_OPA_COVER, 560, 120, a_opa,
         lv_anim_path_linear);
}

/* Ambient touch-wake entry: the dot is already on the panel at (from_x,
 * from_y) - the wandering full stop - so instead of dropping from above
 * it starts exactly there and glides home while the letters fall. The
 * caller swaps screens with a time-0 load, so the frame after the swap
 * shows the dot where the ambient screen last drew it: seamless. */
void scr_splash_show_from(int32_t from_x, int32_t from_y)
{
    show_base();

    dot_style_reset();
    lv_obj_update_layout(s_scr);          /* need the dot's rest coords */
    lv_area_t rest;
    lv_obj_get_coords(s_dot, &rest);
    int32_t dx = from_x - rest.x1;
    int32_t dy = from_y - rest.y1;

    lv_obj_set_style_opa(s_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_translate_x(s_dot, dx, 0);
    lv_obj_set_style_translate_y(s_dot, dy, 0);
    anim(s_dot, dx, 0, 350, 850, a_tx, lv_anim_path_ease_in_out);
    anim(s_dot, dy, DOT_REST_TY, 350, 850, a_ty, lv_anim_path_ease_in_out);
    /* one little squash as it lands as the full stop */
    anim(s_dot, 0, 300, 1200, 300, a_settle, lv_anim_path_linear);
}
