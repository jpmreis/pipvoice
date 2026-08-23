/* Pip UI - ambient "you have mail" screen.
 *
 * When the box has been idle long enough to sleep and there is unheard
 * mail on it, the panel does not go dark: it shows the amber dot from the
 * splash wordmark - the full stop of "Pip." out wandering for the night -
 * on true black at about a third of full brightness (AMBIENT_LEVEL,
 * power.c). No count, no clock: presence only. With nothing unheard the
 * panel still goes fully off, exactly as before.
 *
 * The dot relocates on a 60 s timer around a 48-cell grid (fixed shuffle,
 * s_order below) and greets each new spot with a short burst of
 * diminishing hops, squash-and-stretch and all (s_hops) - alive to a
 * passing eye, still for the rest of the minute. Burn-in shaped
 * everything: a 14 px dot lights ~0.09 % of the panel (the envelope glyph
 * it replaced lit seven times more), no pixel holds it for more than
 * ~1/48 of the ambient hours, and between bursts nothing redraws.
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

#define AMBIENT_SHIFT_MS  (60 * 1000)   /* one full sweep every 48 minutes */

#define DOT_SIZE          14    /* same as the splash wordmark's full stop */
#define HOP_RISE          16    /* tallest hop of the greeting bounce      */

#define AMBIENT_MARGIN    20
#define AMBIENT_COLS      6
#define AMBIENT_ROWS      8
#define AMBIENT_STEP_X    54
#define AMBIENT_STEP_Y    50
#define AMBIENT_CELLS     (AMBIENT_COLS * AMBIENT_ROWS)

/* Margin and both steps are even on purpose: rounder_cb (board.c) aligns
 * QSPI flush windows to 2 px for this panel, so an odd offset buys nothing
 * and costs an extra column of redraw. */
_Static_assert(AMBIENT_MARGIN % 2 == 0 && AMBIENT_STEP_X % 2 == 0 &&
               AMBIENT_STEP_Y % 2 == 0, "ambient grid offsets must be even");
_Static_assert(AMBIENT_STEP_X >= DOT_SIZE && AMBIENT_STEP_Y >= DOT_SIZE,
               "ambient cells overlap: consecutive positions share pixels");
_Static_assert(AMBIENT_MARGIN >= HOP_RISE,
               "top-row hops would leave the panel");
_Static_assert(AMBIENT_MARGIN + (AMBIENT_COLS - 1) * AMBIENT_STEP_X +
               DOT_SIZE <= SCREEN_W, "ambient grid overflows panel width");
_Static_assert(AMBIENT_MARGIN + (AMBIENT_ROWS - 1) * AMBIENT_STEP_Y +
               DOT_SIZE <= SCREEN_H, "ambient grid overflows panel height");

static lv_obj_t   *s_dot;
static lv_timer_t *s_shift;
static uint8_t     s_cursor;    /* index into s_order; wraps */

/* Fixed shuffle of the 48 cells: no two consecutive cells share a row or a
 * column, and every hop - the wrap from the last entry back to the first
 * included - moves at least 5 cells. A table, so there is no RNG, no float
 * and nothing to persist across a reboot. */
static const uint8_t s_order[AMBIENT_CELLS] = {
    24, 34, 18, 41,  6, 33, 11, 39,
     5, 27,  7, 40, 30,  9, 29, 45,
     8, 43, 12, 22, 36, 28,  1, 42,
    23, 38,  4, 20, 47, 16, 31, 10,
    19, 44, 13, 46, 26, 17, 32,  3,
    25,  2, 35, 14, 37, 21,  0, 15,
};

/* The greeting bounce, macOS-dock style: a crouch, then three diminishing
 * hops, stretched tall in the air and squashed flat on each landing.
 * Driven by one lv_anim whose value is milliseconds into this table;
 * hop_exec() interpolates linearly between rows. Scales are percent
 * (pivot is the dot's bottom center, so squash sticks to the "floor"). */
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
    lv_obj_set_style_translate_y(dot, a->ty + ((b->ty - a->ty) * f) / 256, 0);
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
    lv_obj_set_style_translate_y(s_dot, 0, 0);
    lv_obj_set_style_transform_scale_x(s_dot, LV_SCALE_NONE, 0);
    lv_obj_set_style_transform_scale_y(s_dot, LV_SCALE_NONE, 0);
}

static void place(void)
{
    uint8_t cell = s_order[s_cursor];
    lv_obj_set_pos(s_dot,
                   AMBIENT_MARGIN + (cell % AMBIENT_COLS) * AMBIENT_STEP_X,
                   AMBIENT_MARGIN + (cell / AMBIENT_COLS) * AMBIENT_STEP_Y);
}

static void advance(void)
{
    s_cursor = (uint8_t)((s_cursor + 1) % AMBIENT_CELLS);
    place();
    bounce();
}

static void shift_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    advance();
}

lv_obj_t *scr_ambient_create(void)
{
    lv_obj_t *scr = mk_screen();

    s_dot = lv_obj_create(scr);
    lv_obj_remove_style_all(s_dot);
    lv_obj_set_size(s_dot, DOT_SIZE, DOT_SIZE);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    /* amber, never COL_TEXT white: 0xFFB300 leaves the blue channel dark
     * and blue emitters age fastest */
    lv_obj_set_style_bg_color(s_dot, COL_ACCENT, 0);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_transform_pivot_x(s_dot, DOT_SIZE / 2, 0);
    lv_obj_set_style_transform_pivot_y(s_dot, DOT_SIZE, 0);
    place();

    return scr;
}

void scr_ambient_show(void)
{
    /* move on entry too, so a box that wakes and re-sleeps repeatedly does
     * not keep restarting on the same pixels */
    dot_reset();
    advance();
    if (!s_shift) s_shift = lv_timer_create(shift_cb, AMBIENT_SHIFT_MS, NULL);
}

void scr_ambient_hide(void)
{
    if (s_shift) {
        lv_timer_delete(s_shift);
        s_shift = NULL;
    }
    dot_reset();
}

/* Where the dot rests right now, in screen coordinates - the touch-wake
 * handoff starts the splash dot here. Freezes any bounce first so both
 * screens agree on the pose. */
void scr_ambient_dot_pos(int32_t *x, int32_t *y)
{
    dot_reset();
    lv_area_t a;
    lv_obj_get_coords(s_dot, &a);
    *x = a.x1;
    *y = a.y1;
}
