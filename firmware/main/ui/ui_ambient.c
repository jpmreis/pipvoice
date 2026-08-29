/* Pip UI - ambient sleep screen: a wandering bedside clock.
 *
 * When the box has been idle long enough to sleep, the panel shows the
 * time in dim amber on true black - "02:47" set exactly like the splash
 * wordmark, and with unheard mail the wordmark's full stop docks after
 * it ("02:47."), out keeping the clock company for the night. Two moods,
 * decided by ui_ambient_mail():
 *
 *   quiet night  - clock only, no animation of any kind: it relocates
 *                  every AMBIENT_SHIFT_MS (a straight jump, nothing
 *                  redraws in between except the minute flip) and the
 *                  panel holds the old AMBIENT_LEVEL (power.c).
 *   mail waiting - the dot joins the clock and greets each new spot
 *                  with the diminishing-hops bounce (s_hops), and
 *                  power.c raises the panel to AMBIENT_MAIL_LEVEL so
 *                  the invitation carries across a room.
 *
 * This replaces the dot-only "presence only, no clock" screen: the owner
 * decided a clock face is worth the extra lit pixels. Burn-in still
 * shaped everything: the clock is amber only (0xFFB300 leaves the blue
 * channel dark and blue emitters age fastest), it relocates around a
 * 16-cell grid whose steps are at least the unit's own size (consecutive
 * cells never share a pixel, and each cell holds the glyphs for ~1/16 of
 * the ambient hours), and a quiet night never redraws between moves. A
 * box that does not know the time yet (no SNTP answer) keeps the old
 * behavior via ui_ambient_wanted(): dot alone with mail, dark panel
 * without.
 *
 * Touch-wake continuity lives in ui_ambient_wake_to_splash() (ui_core.c):
 * unlike every other way out of ambient the panel is NOT blanked - the
 * splash paints with its dot starting at this screen's last dot position
 * (scr_ambient_dot_pos) and gliding home into the wordmark
 * (scr_splash_show_from).
 *
 * The screen deliberately ignores the background theme - there is no
 * scr_ambient_apply_theme() for ui_set_background() to call. A full-panel
 * photo held at medium brightness for hours is the exact thing this
 * design exists to avoid.
 */
#include "ui_internal.h"
#include <string.h>

#define AMBIENT_SHIFT_MS  (10 * 1000)   /* full grid sweep in ~2.7 minutes */
#define CLOCK_TICK_MS     1000          /* minute-flip check; redraw only
                                           when the text actually changes */

#define DOT_SIZE          14    /* same as the splash wordmark's full stop */
#define DOT_REST_TY       12    /* same baseline drop as the splash's dot  */
#define HOP_RISE          16    /* tallest hop of the greeting bounce      */

/* The moving unit is clock + gap + dot; the grid is sized for its worst
 * case so a cell never overlaps its successor. UNIT_H is FONT_BIG's line
 * height; UNIT_W_MAX budgets "88:88" in FONT_BIG plus the wordmark's
 * 3 px letter gap plus the dot, with slack. */
#define UNIT_W_MAX        140
#define UNIT_H            48

#define AMBIENT_MARGIN    20
#define AMBIENT_COLS      2
#define AMBIENT_ROWS      8
#define AMBIENT_STEP_X    188
#define AMBIENT_STEP_Y    48
#define AMBIENT_CELLS     (AMBIENT_COLS * AMBIENT_ROWS)

/* Margin and both steps are even on purpose: rounder_cb (board.c) aligns
 * QSPI flush windows to 2 px for this panel, so an odd offset buys nothing
 * and costs an extra column of redraw. */
_Static_assert(AMBIENT_MARGIN % 2 == 0 && AMBIENT_STEP_X % 2 == 0 &&
               AMBIENT_STEP_Y % 2 == 0, "ambient grid offsets must be even");
_Static_assert(AMBIENT_STEP_X >= UNIT_W_MAX && AMBIENT_STEP_Y >= UNIT_H,
               "ambient cells overlap: consecutive positions share pixels");
_Static_assert(AMBIENT_MARGIN >= HOP_RISE,
               "top-row hops would leave the panel");
_Static_assert(AMBIENT_MARGIN + (AMBIENT_COLS - 1) * AMBIENT_STEP_X +
               UNIT_W_MAX <= SCREEN_W, "ambient grid overflows panel width");
_Static_assert(AMBIENT_MARGIN + (AMBIENT_ROWS - 1) * AMBIENT_STEP_Y +
               UNIT_H <= SCREEN_H, "ambient grid overflows panel height");

static lv_obj_t   *s_unit;      /* flex row: clock label + dot */
static lv_obj_t   *s_clock;
static lv_obj_t   *s_dot;
static lv_timer_t *s_shift;
static lv_timer_t *s_tick;
static uint8_t     s_cursor;    /* index into s_order; wraps */
static char        s_shown[8];  /* clock text on the panel right now */

/* Fixed shuffle of the 16 cells (cell = row * 2 + col): columns strictly
 * alternate and consecutive rows differ by at least 3 - the wrap from the
 * last entry back to the first included - so every move crosses most of
 * the panel. A table, so there is no RNG, no float and nothing to persist
 * across a reboot. */
static const uint8_t s_order[AMBIENT_CELLS] = {
     0,  7, 12,  3,  8, 15,  4, 11,
     2, 13,  6,  1, 10,  5, 14,  9,
};

/* The greeting bounce, macOS-dock style: a crouch, then three diminishing
 * hops, stretched tall in the air and squashed flat on each landing.
 * Driven by one lv_anim whose value is milliseconds into this table;
 * hop_exec() interpolates linearly between rows. Scales are percent
 * (pivot is the dot's bottom center, so squash sticks to the "floor").
 * ty values are relative to the dot's baseline rest (DOT_REST_TY). */
typedef struct { uint16_t t; int8_t ty; uint8_t sx, sy; } hop_kf_t;
static const hop_kf_t s_hops[] = {
    {    0,         0, 100, 100 },
    {   75,         0, 115,  85 },   /* anticipation crouch  */
    {  180, -HOP_RISE,  82, 118 },   /* first hop, in the air */
    {  330,         0, 125,  78 },   /* landing squash        */
    {  420,         0, 100, 100 },
    {  510,       -10,  88, 112 },   /* second, smaller       */
    {  645,         0, 118,  85 },
    {  735,         0, 100, 100 },
    {  825,        -5,  94, 106 },   /* last little one       */
    {  945,         0, 110,  92 },
    { 1050,         0, 100, 100 },
};
#define HOP_TOTAL_MS  1050

static void hop_exec(void *var, int32_t v)
{
    lv_obj_t *dot = var;
    const size_t n = sizeof(s_hops) / sizeof(s_hops[0]);
    const hop_kf_t *a = &s_hops[n - 1], *b = a;
    for (size_t i = 1; i < n; i++) {
        if (v <= s_hops[i].t) { a = &s_hops[i - 1]; b = &s_hops[i]; break; }
    }
    int32_t span = b->t - a->t;
    int32_t f = span ? ((v - a->t) * 256) / span : 256;
    lv_obj_set_style_translate_y(
        dot, DOT_REST_TY + a->ty + ((b->ty - a->ty) * f) / 256, 0);
    lv_obj_set_style_transform_scale_x(
        dot, ((a->sx + ((b->sx - a->sx) * f) / 256) * 256) / 100, 0);
    lv_obj_set_style_transform_scale_y(
        dot, ((a->sy + ((b->sy - a->sy) * f) / 256) * 256) / 100, 0);
}

static void bounce(void)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_dot);
    lv_anim_set_values(&a, 0, HOP_TOTAL_MS);
    lv_anim_set_duration(&a, HOP_TOTAL_MS);
    lv_anim_set_exec_cb(&a, hop_exec);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_start(&a);
}

/* kill an in-flight bounce and put the dot back in its rest pose */
static void dot_reset(void)
{
    lv_anim_delete(s_dot, NULL);
    lv_obj_set_style_translate_y(s_dot, DOT_REST_TY, 0);
    lv_obj_set_style_transform_scale_x(s_dot, LV_SCALE_NONE, 0);
    lv_obj_set_style_transform_scale_y(s_dot, LV_SCALE_NONE, 0);
}

/* rest position of the current cell, in screen coordinates (the unit is a
 * direct child of a zero-padded screen, so set_pos coords are absolute) */
static void cell_pos(int32_t *x, int32_t *y)
{
    uint8_t cell = s_order[s_cursor];
    *x = AMBIENT_MARGIN + (cell % AMBIENT_COLS) * AMBIENT_STEP_X;
    *y = AMBIENT_MARGIN + (cell / AMBIENT_COLS) * AMBIENT_STEP_Y;
}

static void place(void)
{
    int32_t x, y;
    cell_pos(&x, &y);
    lv_obj_set_pos(s_unit, x, y);
}

/* re-render the time; the label redraws only when the minute flips. An
 * empty string (clock not sane yet) collapses the label out of the flex
 * row, leaving the dot alone - the pre-clock screen, exactly. */
static void clock_update(void)
{
    char txt[sizeof(s_shown)];
    ui_clock_text(txt, sizeof(txt));
    if (strcmp(txt, s_shown) == 0) return;
    strlcpy(s_shown, txt, sizeof(s_shown));
    lv_label_set_text(s_clock, txt);
    if (txt[0]) lv_obj_remove_flag(s_clock, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_add_flag(s_clock, LV_OBJ_FLAG_HIDDEN);
}

/* dot visibility follows the mail state; no animation here (advance() and
 * scr_ambient_refresh() decide when a greeting bounce is due) */
static void sync_dot(void)
{
    if (ui_ambient_mail()) {
        lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
    } else {
        dot_reset();
        lv_obj_add_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
    }
}

static void advance(void)
{
    s_cursor = (uint8_t)((s_cursor + 1) % AMBIENT_CELLS);
    dot_reset();
    place();
    clock_update();
    sync_dot();
    /* a quiet night moves in silence; the dot greets each new spot */
    if (!lv_obj_has_flag(s_dot, LV_OBJ_FLAG_HIDDEN)) bounce();
}

static void shift_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    advance();
}

static void tick_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    clock_update();
}

lv_obj_t *scr_ambient_create(void)
{
    lv_obj_t *scr = mk_screen();

    /* the unit mirrors the splash wordmark row: label + 3 px gap + dot,
     * dot dropped DOT_REST_TY to the baseline. OVERFLOW_VISIBLE because
     * the hops and landing squashes poke past the tight content box. */
    s_unit = lv_obj_create(scr);
    lv_obj_remove_style_all(s_unit);
    lv_obj_set_size(s_unit, LV_SIZE_CONTENT, UNIT_H);
    lv_obj_set_flex_flow(s_unit, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_unit, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_unit, 3, 0);
    lv_obj_add_flag(s_unit, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_clear_flag(s_unit, LV_OBJ_FLAG_SCROLLABLE);   /* the dot's hops
                                          must never scroll their own box */

    /* amber, never COL_TEXT white: 0xFFB300 leaves the blue channel dark
     * and blue emitters age fastest */
    s_clock = lv_label_create(s_unit);
    lv_obj_set_style_text_font(s_clock, FONT_BIG, 0);
    lv_obj_set_style_text_color(s_clock, COL_ACCENT, 0);
    lv_label_set_text(s_clock, "");
    lv_obj_add_flag(s_clock, LV_OBJ_FLAG_HIDDEN);

    s_dot = lv_obj_create(s_unit);
    lv_obj_remove_style_all(s_dot);
    lv_obj_set_size(s_dot, DOT_SIZE, DOT_SIZE);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_dot, COL_ACCENT, 0);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_transform_pivot_x(s_dot, DOT_SIZE / 2, 0);
    lv_obj_set_style_transform_pivot_y(s_dot, DOT_SIZE, 0);
    lv_obj_set_style_translate_y(s_dot, DOT_REST_TY, 0);
    lv_obj_add_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
    place();

    return scr;
}

void scr_ambient_show(void)
{
    /* move on entry too, so a box that wakes and re-sleeps repeatedly does
     * not keep restarting on the same pixels */
    s_shown[0] = '\0';           /* force the first clock_update to render */
    advance();
    if (!s_shift) s_shift = lv_timer_create(shift_cb, AMBIENT_SHIFT_MS, NULL);
    if (!s_tick)  s_tick  = lv_timer_create(tick_cb, CLOCK_TICK_MS, NULL);
}

void scr_ambient_hide(void)
{
    if (s_shift) {
        lv_timer_delete(s_shift);
        s_shift = NULL;
    }
    if (s_tick) {
        lv_timer_delete(s_tick);
        s_tick = NULL;
    }
    dot_reset();
}

/* Mail arrived or the last unheard was heard while the screen is up: flip
 * the dot without waiting for the next shift (power.c retunes brightness
 * on its own 1 Hz ladder). A dot that just appeared gets its greeting. */
void scr_ambient_refresh(void)
{
    bool was = !lv_obj_has_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
    sync_dot();
    bool now = !lv_obj_has_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
    if (now && !was) bounce();
}

/* Where the dot rests right now, in screen coordinates - the touch-wake
 * handoff starts the splash dot here. Freezes any bounce first so both
 * screens agree on the pose, then resolves the layout: clearing the hop's
 * translate only marks it dirty, so the cached coords would otherwise
 * still hold the mid-hop pose. On a clock-only night there is no dot on
 * the panel; the glide then starts where the full stop would have sat,
 * just past the clock's right edge. */
void scr_ambient_dot_pos(int32_t *x, int32_t *y)
{
    dot_reset();
    lv_obj_update_layout(s_unit);
    lv_area_t a;
    if (lv_obj_has_flag(s_dot, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_get_coords(s_unit, &a);   /* hidden dot is out of the flex */
        *x = a.x2 + 3;
        *y = a.y1 + (UNIT_H - DOT_SIZE) / 2 + DOT_REST_TY;
        return;
    }
    lv_obj_get_coords(s_dot, &a);
    *x = a.x1;
    *y = a.y1;
}
