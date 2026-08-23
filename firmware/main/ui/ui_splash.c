/* Pip UI - splash: animated "Pip" wordmark + "Hello <owner>" greeting.
 * Shown at boot and when waking from full display-off (see ui_splash_show),
 * and as the landing half of the ambient touch-wake, where the wandering
 * amber dot glides home into the wordmark (scr_splash_show_from). */
#include "ui_internal.h"

#define SPLASH_TOTAL_MS  UI_SPLASH_MS   /* app_main waits this out */
#define DOT_REST_TY      12      /* dot settles at the wordmark baseline */
#define DOT_SIZE         14

static lv_obj_t   *s_scr;
static lv_obj_t   *s_letters[3];
static lv_obj_t   *s_dot;
static lv_obj_t   *s_travel;    /* see scr_splash_show_from() */
static lv_obj_t   *s_hello;
static lv_timer_t *s_exit_timer;

/* ------------- anim plumbing ------------- */
static void a_ty(void *o, int32_t v)  { lv_obj_set_style_translate_y(o, v, 0); }
static void a_opa(void *o, int32_t v) { lv_obj_set_style_opa(o, v, 0); }
/* absolute moves, for the screen-level traveller (no layout to fight) */
static void a_x(void *o, int32_t v)   { lv_obj_set_x(o, v); }
static void a_y(void *o, int32_t v)   { lv_obj_set_y(o, v); }

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
    lv_obj_set_size(s_dot, DOT_SIZE, DOT_SIZE);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_dot, COL_ACCENT, 0);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(s_dot, LV_OPA_TRANSP, 0);
    lv_obj_set_style_transform_pivot_x(s_dot, DOT_SIZE / 2, 0);
    lv_obj_set_style_transform_pivot_y(s_dot, DOT_SIZE, 0);

    /* The travelling twin of the full stop, used only by the ambient
     * hand-off. It has to be a child of the screen and not of the wordmark
     * row: LVGL clips a child to its parent's box unless the parent carries
     * LV_OBJ_FLAG_OVERFLOW_VISIBLE, and `cont` is a tight box around "Pip",
     * so a dot translated out to the far side of the panel simply is not
     * drawn. That is what made the wandering dot vanish at the swap. */
    s_travel = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_travel);
    lv_obj_set_size(s_travel, DOT_SIZE, DOT_SIZE);
    lv_obj_set_style_radius(s_travel, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_travel, COL_ACCENT, 0);
    lv_obj_set_style_bg_opa(s_travel, LV_OPA_COVER, 0);
    lv_obj_set_style_transform_pivot_x(s_travel, DOT_SIZE / 2, 0);
    lv_obj_set_style_transform_pivot_y(s_travel, DOT_SIZE, 0);
    lv_obj_add_flag(s_travel, LV_OBJ_FLAG_HIDDEN);

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

/* a previous show may have left translate/scale on either dot, and the
 * traveller parked wherever its glide ended */
static void dot_style_reset(void)
{
    lv_anim_delete(s_dot, NULL);
    lv_obj_set_style_translate_y(s_dot, 0, 0);
    lv_obj_set_style_transform_scale_x(s_dot, LV_SCALE_NONE, 0);
    lv_obj_set_style_transform_scale_y(s_dot, LV_SCALE_NONE, 0);
    lv_anim_delete(s_travel, NULL);
    lv_obj_add_flag(s_travel, LV_OBJ_FLAG_HIDDEN);
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

/* the traveller has arrived: hand the pose back to the real full stop,
 * which lands with one little squash */
static void travel_done(lv_anim_t *a)
{
    LV_UNUSED(a);
    lv_obj_add_flag(s_travel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_translate_y(s_dot, DOT_REST_TY, 0);
    lv_obj_set_style_opa(s_dot, LV_OPA_COVER, 0);
    anim(s_dot, 0, 300, 0, 300, a_settle, lv_anim_path_linear);
}

/* Ambient touch-wake entry: the dot is already on the panel at (from_x,
 * from_y) - the wandering full stop - so instead of dropping from above it
 * starts exactly there and glides home while the letters fall. The caller
 * swaps screens with a time-0 load, so the frame after the swap shows a dot
 * where the ambient screen last drew one: seamless.
 *
 * The glide runs on s_travel, the screen-level twin, because the wordmark's
 * own dot is clipped to the flex row it lives in (see scr_splash_create).
 * The real dot stays transparent until the traveller reaches the baseline
 * and travel_done() swaps them - same size, same colour, same pixel, so the
 * handover is invisible. */
void scr_splash_show_from(int32_t from_x, int32_t from_y)
{
    show_base();

    dot_style_reset();
    lv_obj_update_layout(s_scr);          /* need the dot's rest coords */
    lv_area_t rest;
    lv_obj_get_coords(s_dot, &rest);
    lv_obj_set_style_opa(s_dot, LV_OPA_TRANSP, 0);

    /* screen is zero-padded at (0,0), so set_pos coords are screen coords */
    lv_obj_set_pos(s_travel, from_x, from_y);
    lv_obj_remove_flag(s_travel, LV_OBJ_FLAG_HIDDEN);

    anim(s_travel, from_x, rest.x1, 350, 850, a_x, lv_anim_path_ease_in_out);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_travel);
    lv_anim_set_values(&a, from_y, rest.y1 + DOT_REST_TY);
    lv_anim_set_delay(&a, 350);
    lv_anim_set_duration(&a, 850);
    lv_anim_set_exec_cb(&a, a_y);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_completed_cb(&a, travel_done);
    lv_anim_start(&a);
}
