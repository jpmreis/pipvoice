/* Voice-control screen: what Pip is asking right now, in letters big
 * enough to read from across a room. The flow itself lives in voice.c;
 * this screen is pure display - no touch targets beyond a corner close,
 * because the whole point is not needing hands. */
#include "ui_internal.h"

static lv_obj_t *s_line1;     /* the question                    */
static lv_obj_t *s_line2;     /* contact name / detail           */
static lv_obj_t *s_hint;      /* standing "say yes" instruction  */

static void close_clicked(lv_event_t *e)
{
    LV_UNUSED(e);
    ui_voice_close();
}

lv_obj_t *scr_voice_create(void)
{
    lv_obj_t *scr = mk_screen();

    lv_obj_t *col = lv_obj_create(scr);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, SCREEN_W, SCREEN_H);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 18, 0);

    s_line1 = lv_label_create(col);
    lv_obj_set_style_text_font(s_line1, FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_line1, COL_TEXT, 0);
    lv_obj_set_style_text_align(s_line1, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_line1, SCREEN_W - 40);
    lv_label_set_long_mode(s_line1, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_line1, "");

    s_line2 = lv_label_create(col);
    lv_obj_set_style_text_font(s_line2, FONT_BIG, 0);
    lv_obj_set_style_text_color(s_line2, COL_ACCENT, 0);
    lv_obj_set_style_text_align(s_line2, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_line2, SCREEN_W - 40);
    lv_label_set_long_mode(s_line2, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_line2, "");

    s_hint = lv_label_create(col);
    lv_obj_set_style_text_font(s_hint, FONT_BODY, 0);
    lv_obj_set_style_text_color(s_hint, COL_TEXT_DIM, 0);
    lv_label_set_text(s_hint, "say \"yes\"");

    /* an escape hatch for a helper standing nearby */
    lv_obj_t *close = lv_label_create(scr);
    lv_obj_set_style_text_font(close, FONT_TITLE, 0);
    lv_obj_set_style_text_color(close, COL_TEXT_DIM, 0);
    lv_label_set_text(close, LV_SYMBOL_CLOSE);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -20, 16);
    lv_obj_add_flag(close, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(close, 24);
    lv_obj_add_event_cb(close, close_clicked, LV_EVENT_CLICKED, NULL);

    return scr;
}

void ui_voice_show(const char *line1, const char *line2)
{
    lv_label_set_text(s_line1, line1 ? line1 : "");
    lv_label_set_text(s_line2, line2 ? line2 : "");
    if (!ui_screen_is(SCR_VOICE) && !nav_locked_out())
        nav_to(SCR_VOICE, LV_SCR_LOAD_ANIM_FADE_IN);
}

void ui_voice_close(void)
{
    if (ui_screen_is(SCR_VOICE))
        nav_to(SCR_HOME, LV_SCR_LOAD_ANIM_FADE_IN);
}
