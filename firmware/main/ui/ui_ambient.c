/* Pip UI - ambient "you have mail" screen.
 *
 * When the box has been idle long enough to sleep and there is unheard mail
 * on it, the panel does not go dark: it shows a single small amber icon on
 * true black at about a third of full brightness (AMBIENT_LEVEL, power.c).
 * No count, no clock, no animation - presence only, and as few lit pixels
 * as saying "there is mail" allows. With nothing unheard the panel still
 * goes fully off, exactly as before.
 *
 * Everything here is shaped by OLED burn-in. The icon lights well under 1%
 * of the panel, and one 60 s timer walks it around a 48-cell grid so no
 * pixel carries more than ~1/48 of the ambient hours. The visit order is a
 * fixed shuffle rather than a raster scan: a box that sits ambient over the
 * same stretch every night would otherwise wear a row-shaped band.
 *
 * The screen deliberately ignores the background theme - there is no
 * scr_ambient_apply_theme() for ui_set_background() to call. A full-panel
 * photo held at medium brightness for hours is the exact thing this design
 * exists to avoid.
 */
#include "ui_internal.h"

#define AMBIENT_SHIFT_MS  (60 * 1000)   /* one full sweep every 48 minutes */

/* Box reserved for the icon at every cell (the montserrat-40 envelope plus
 * slack): the grid keeps this clear of the panel edge. AMBIENT_GLYPH_MAX is
 * the tighter bound - what the glyph itself can actually cover - and is what
 * the cell spacing has to beat for two consecutive positions to be disjoint. */
#define AMBIENT_ICON_W    56
#define AMBIENT_ICON_H    56
#define AMBIENT_GLYPH_MAX 48    /* >= any montserrat-40 glyph extent */

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
_Static_assert(AMBIENT_STEP_X >= AMBIENT_GLYPH_MAX &&
               AMBIENT_STEP_Y >= AMBIENT_GLYPH_MAX,
               "ambient cells overlap: consecutive positions would share pixels");
_Static_assert(AMBIENT_MARGIN + (AMBIENT_COLS - 1) * AMBIENT_STEP_X +
               AMBIENT_ICON_W <= SCREEN_W, "ambient grid overflows panel width");
_Static_assert(AMBIENT_MARGIN + (AMBIENT_ROWS - 1) * AMBIENT_STEP_Y +
               AMBIENT_ICON_H <= SCREEN_H, "ambient grid overflows panel height");

static lv_obj_t   *s_icon;
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

static void place(void)
{
    uint8_t cell = s_order[s_cursor];
    int32_t x = AMBIENT_MARGIN + (cell % AMBIENT_COLS) * AMBIENT_STEP_X;
    int32_t y = AMBIENT_MARGIN + (cell / AMBIENT_COLS) * AMBIENT_STEP_Y;
    lv_obj_align(s_icon, LV_ALIGN_TOP_LEFT, x, y);
}

static void advance(void)
{
    s_cursor = (uint8_t)((s_cursor + 1) % AMBIENT_CELLS);
    place();
}

static void shift_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    advance();
}

lv_obj_t *scr_ambient_create(void)
{
    lv_obj_t *scr = mk_screen();

    s_icon = lv_label_create(scr);
    lv_label_set_text(s_icon, LV_SYMBOL_ENVELOPE);
    lv_obj_set_style_text_font(s_icon, FONT_BIG, 0);
    /* amber, never COL_TEXT white: 0xFFB300 leaves the blue channel dark
     * and blue emitters age fastest */
    lv_obj_set_style_text_color(s_icon, COL_ACCENT, 0);
    lv_obj_set_style_text_opa(s_icon, LV_OPA_COVER, 0);
    place();

    return scr;
}

void scr_ambient_show(void)
{
    /* move on entry too, so a box that wakes and re-sleeps repeatedly does
     * not keep restarting on the same pixels */
    advance();
    if (!s_shift) s_shift = lv_timer_create(shift_cb, AMBIENT_SHIFT_MS, NULL);
}

void scr_ambient_hide(void)
{
    if (s_shift) {
        lv_timer_delete(s_shift);
        s_shift = NULL;
    }
}
