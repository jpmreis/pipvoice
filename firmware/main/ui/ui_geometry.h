/* Pip UI - per-board geometry.
 *
 * The single source of truth for everything panel-shaped: screen size,
 * safe areas, per-screen anchors and widget sizes. One block per SKU,
 * selected by the PIP_BOARD_* define that CMake derives from
 * -DPIP_BOARD=<key> (see firmware/CMakeLists.txt); the 1.8 block is the
 * identity mapping of the numbers the screens always used, so a default
 * build is pixel-identical to pre-geometry firmware.
 *
 * The 1.75-B is ROUND (466x466, r=233): corners do not exist, so its
 * block moves every corner-anchored element (header back arrow, status
 * gear/wifi, bottom button pairs) onto centred clusters and chord-fitted
 * widths, per the "safe rect" design (canvas: "Pip Home, Three Boards").
 * The 2.16 (480x480) keeps the 1.8 anatomy scaled up ("big tiles").
 *
 * Numbers here were solved against the circle in the design pass; when
 * touching a GEO_ROUND value, re-check the chord: an element spanning
 * half-width w at distance d from the nearest edge needs
 *   w <= sqrt(d * (2*233 - d))   (with a few px of margin).
 */
#ifndef PIP_UI_GEOMETRY_H
#define PIP_UI_GEOMETRY_H

#if defined(PIP_BOARD_AMOLED_1_75B)

/* ---------------- Waveshare AMOLED 1.75-B: 466x466 round --------------- */
#define SCREEN_W              466
#define SCREEN_H              466
#define GEO_ROUND             1

/* header: centred chevron+title cluster (no corners to put a back
 * button in); the bar still spans the width for theming/hit purposes */
#define GEO_HDR_H             64
#define GEO_HDR_TITLE_Y       30    /* cluster top */

/* toasts / centred modals: the top band is narrow on a circle */
#define GEO_TOAST_W           260
#define GEO_TOAST_Y           90
#define GEO_MODAL_W           300   /* offline nag panel */
#define GEO_REACT_W           320   /* react picker panel */

/* home */
#define GEO_HOME_GREETING     1
#define GEO_STATUS_Y          28    /* centred status cluster top */
#define GEO_HOME_PAD_TOP      74
#define GEO_GRID_PAD_ROW      8
#define GEO_CELL_W            150
#define GEO_CELL_H            126
#define GEO_AVATAR_D          96
#define GEO_NAME_Y            100
#define GEO_CHIP_D            36
#define GEO_CHIP_DX           6
#define GEO_CHIP_DY           -4
#define GEO_CHIP_EMOJI_PX     20
#define GEO_PILL_W            220
#define GEO_PILL_H            54
#define GEO_PILL_BOTTOM       34
#define GEO_BADGE_D           30

/* record */
#define GEO_REC_TIMER_Y       76
#define GEO_REC_BTN_D         170
#define GEO_REC_BTN_DY        2
#define GEO_REC_HINT_DY       112
#define GEO_REC_PEER_DY       140
#define GEO_SEND_W            180
#define GEO_SEND_H            54
#define GEO_REDO_W            54
#define GEO_BTNROW_BOTTOM     44
#define GEO_BTNROW_X          111   /* (466 - (180+10+54)) / 2 */

/* playback */
#define GEO_PB_WHEN_Y         64
#define GEO_PB_ARC_D          200
#define GEO_PB_PLAY_D         152
#define GEO_PB_ARC_DY         -7
#define GEO_PB_TIME_DY        113
#define GEO_PB_BTN_W          124
#define GEO_PB_BTN_H          54
#define GEO_PB_BTN_BOTTOM     48
#define GEO_PB_BTN_X          104   /* (466 - (124+10+124)) / 2 */

/* inbox list / settings column: rows live in the wide mid-band */
#define GEO_LIST_TOP          84
#define GEO_LIST_BOTTOM       84
#define GEO_LIST_W            300
#define GEO_ROW_H             76
#define GEO_ROW_AV            44
#define GEO_ROW_AV_X          6
#define GEO_ROW_TEXT_X        62
#define GEO_ROW_PLAY_DX       -10
#define GEO_ROW_REACT_DX      -48
#define GEO_SETTINGS_W        300

/* pinpad */
#define GEO_PIN_MSG_Y         32
#define GEO_PIN_DOTS_Y        68
#define GEO_KEY_D             72
#define GEO_PAD_W             264
#define GEO_PAD_H             312
#define GEO_PAD_BOTTOM        50

/* theme picker: round panel, round 1:1 thumbs (server renders these) */
#define GEO_THUMB_W           100
#define GEO_THUMB_H           100
#define GEO_THUMB_ROUND       1
#define GEO_PICKER_COLS       3
#define GEO_PICKER_TOP        84
#define GEO_PICKER_BOTTOM     62

/* ambient: 2x5 grid inside r~200 - every cell corner stays >=33 px
 * inside the rim (checked in the design pass; the rectangular bounds
 * are still asserted in ui_ambient.c) */
#define GEO_AMB_X0            80
#define GEO_AMB_Y0            120
#define GEO_AMB_COLS          2
#define GEO_AMB_ROWS          5
#define GEO_AMB_STEP_X        166
#define GEO_AMB_STEP_Y        48

/* splash: the wordmark grows into the circle (needs
 * CONFIG_LV_FONT_MONTSERRAT_48, sdkconfig.defaults.amoled-1.75b);
 * letters drop from past the rim, greeting sits on the bottom chord */
#define FONT_SPLASH           (&lv_font_montserrat_48)
#define GEO_SPLASH_HELLO_DY   -120
#define GEO_SPLASH_DROP       280
#define GEO_SPLASH_DOT_DROP   300

/* voice screen: the corner close is off-glass on a circle */
#define GEO_VOICE_CLOSE_BOTTOM 1

/* wifi setup QR screen */
#define GEO_WIFI_HINT_Y       70
#define GEO_WIFI_QR_DY        14
#define GEO_WIFI_SSID_DY      -76
#define GEO_WIFI_PASS_DY      -48

#elif defined(PIP_BOARD_AMOLED_2_16)

/* ---------------- Waveshare AMOLED 2.16: 480x480 square ---------------- */
#define SCREEN_W              480
#define SCREEN_H              480
#define GEO_ROUND             0

#define GEO_HDR_H             56

#define GEO_TOAST_W           (SCREEN_W - 40)
#define GEO_TOAST_Y           12
#define GEO_MODAL_W           (SCREEN_W - 40)
#define GEO_REACT_W           (SCREEN_W - 24)

/* home: "big tiles" - the cube reads across a room; the greeting folds
 * away to buy the 128 px avatars room */
#define GEO_HOME_GREETING     0
#define GEO_HOME_PAD_TOP      72
#define GEO_GRID_PAD_ROW      12
#define GEO_CELL_W            200
#define GEO_CELL_H            158
#define GEO_AVATAR_D          128
#define GEO_NAME_Y            136
#define GEO_CHIP_D            44
#define GEO_CHIP_DX           6
#define GEO_CHIP_DY           -4
#define GEO_CHIP_EMOJI_PX     26
#define GEO_PILL_W            (SCREEN_W - 24)
#define GEO_PILL_H            68
#define GEO_PILL_BOTTOM       8
#define GEO_BADGE_D           34

/* record */
#define GEO_REC_TITLE_Y       62
#define GEO_REC_TIMER_Y       112
#define GEO_REC_BTN_D         190
#define GEO_REC_BTN_DY        27
#define GEO_REC_HINT_DY       149
#define GEO_REC_PEER_DY       185
#define GEO_SEND_W            332
#define GEO_SEND_H            64
#define GEO_REDO_W            112
#define GEO_BTNROW_BOTTOM     12
#define GEO_BTNROW_X          12

/* playback */
#define GEO_PB_WHO_Y          62
#define GEO_PB_WHEN_Y         98
#define GEO_PB_ARC_D          230
#define GEO_PB_PLAY_D         176
#define GEO_PB_ARC_DY         7
#define GEO_PB_TIME_DY        146
#define GEO_PB_BTN_W          216
#define GEO_PB_BTN_H          64
#define GEO_PB_BTN_BOTTOM     10
#define GEO_PB_BTN_X          16

/* inbox / settings */
#define GEO_LIST_TOP          64
#define GEO_LIST_BOTTOM       0
#define GEO_LIST_W            (SCREEN_W - 24)
#define GEO_ROW_H             84
#define GEO_ROW_AV            52
#define GEO_ROW_AV_X          10
#define GEO_ROW_TEXT_X        74
#define GEO_ROW_PLAY_DX       -12
#define GEO_ROW_REACT_DX      -56
#define GEO_SETTINGS_W        (SCREEN_W - 24)

/* pinpad */
#define GEO_PIN_MSG_Y         24
#define GEO_PIN_DOTS_Y        58
#define GEO_KEY_D             84
#define GEO_PAD_W             342
#define GEO_PAD_H             372
#define GEO_PAD_BOTTOM        14

/* theme picker: 4 columns of 1:1 thumbs (panel aspect) */
#define GEO_THUMB_W           106
#define GEO_THUMB_H           106
#define GEO_THUMB_ROUND       0
#define GEO_PICKER_COLS       4
#define GEO_PICKER_TOP        58
#define GEO_PICKER_BOTTOM     6

/* ambient: same discipline, more panel - 2 cols x 9 rows */
#define GEO_AMB_X0            20
#define GEO_AMB_Y0            20
#define GEO_AMB_COLS          2
#define GEO_AMB_ROWS          9
#define GEO_AMB_STEP_X        300
#define GEO_AMB_STEP_Y        48

#define FONT_SPLASH           FONT_BIG
#define GEO_SPLASH_HELLO_DY   -64
#define GEO_SPLASH_DROP       260
#define GEO_SPLASH_DOT_DROP   280

#define GEO_VOICE_CLOSE_BOTTOM 0

#define GEO_WIFI_HINT_Y       58
#define GEO_WIFI_QR_DY        24
#define GEO_WIFI_SSID_DY      -46
#define GEO_WIFI_PASS_DY      -18

#else /* PIP_BOARD_AMOLED_1_8 (default) */

/* ------------- Waveshare AMOLED 1.8: 368x448 rect (original) ----------- */
#define SCREEN_W              368
#define SCREEN_H              448
#define GEO_ROUND             0

#define GEO_HDR_H             56

#define GEO_TOAST_W           (SCREEN_W - 40)
#define GEO_TOAST_Y           12
#define GEO_MODAL_W           (SCREEN_W - 40)
#define GEO_REACT_W           (SCREEN_W - 24)

/* home */
#define GEO_HOME_GREETING     1
#define GEO_HOME_PAD_TOP      48
#define GEO_GRID_PAD_ROW      14
#define GEO_CELL_W            ((SCREEN_W / 2) - 16)
#define GEO_CELL_H            132
#define GEO_AVATAR_D          104
#define GEO_NAME_Y            108
#define GEO_CHIP_D            40
#define GEO_CHIP_DX           8
#define GEO_CHIP_DY           -4
#define GEO_CHIP_EMOJI_PX     28
#define GEO_PILL_W            (SCREEN_W - 24)
#define GEO_PILL_H            68
#define GEO_PILL_BOTTOM       8
#define GEO_BADGE_D           34

/* record: one row: 12 | send 248 | 12 | redo 84 | 12 = 368 */
#define GEO_REC_TITLE_Y       60
#define GEO_REC_TIMER_Y       108
#define GEO_REC_BTN_D         170
#define GEO_REC_BTN_DY        20
#define GEO_REC_HINT_DY       130
#define GEO_REC_PEER_DY       168
#define GEO_SEND_W            248
#define GEO_SEND_H            64
#define GEO_REDO_W            84
#define GEO_BTNROW_BOTTOM     12
#define GEO_BTNROW_X          12

/* playback */
#define GEO_PB_WHO_Y          62
#define GEO_PB_WHEN_Y         98
#define GEO_PB_ARC_D          210
#define GEO_PB_PLAY_D         160
#define GEO_PB_ARC_DY         10
#define GEO_PB_TIME_DY        130
#define GEO_PB_BTN_W          ((SCREEN_W - 36) / 2)
#define GEO_PB_BTN_H          64
#define GEO_PB_BTN_BOTTOM     10
#define GEO_PB_BTN_X          12

/* inbox / settings */
#define GEO_LIST_TOP          64
#define GEO_LIST_BOTTOM       0
#define GEO_LIST_W            (SCREEN_W - 16)
#define GEO_ROW_H             76
#define GEO_ROW_AV            44
#define GEO_ROW_AV_X          2
#define GEO_ROW_TEXT_X        58
#define GEO_ROW_PLAY_DX       -6
#define GEO_ROW_REACT_DX      -46
#define GEO_SETTINGS_W        (SCREEN_W - 24)

/* pinpad */
#define GEO_PIN_MSG_Y         26
#define GEO_PIN_DOTS_Y        62
#define GEO_KEY_D             80
#define GEO_PAD_W             300
#define GEO_PAD_H             348
#define GEO_PAD_BOTTOM        4

/* theme picker: 3 x 108 tiles + 2 x 10 gaps = 344 */
#define GEO_THUMB_W           108
#define GEO_THUMB_H           132
#define GEO_THUMB_ROUND       0
#define GEO_PICKER_COLS       3
#define GEO_PICKER_TOP        58
#define GEO_PICKER_BOTTOM     6

/* ambient: the original 16-cell grid */
#define GEO_AMB_X0            20
#define GEO_AMB_Y0            20
#define GEO_AMB_COLS          2
#define GEO_AMB_ROWS          8
#define GEO_AMB_STEP_X        188
#define GEO_AMB_STEP_Y        48

#define FONT_SPLASH           FONT_BIG
#define GEO_SPLASH_HELLO_DY   -56
#define GEO_SPLASH_DROP       240
#define GEO_SPLASH_DOT_DROP   260

#define GEO_VOICE_CLOSE_BOTTOM 0

#define GEO_WIFI_HINT_Y       58
#define GEO_WIFI_QR_DY        24
#define GEO_WIFI_SSID_DY      -46
#define GEO_WIFI_PASS_DY      -18

#endif

/* derived, shared by all boards */
#define GEO_PICKER_W \
    (GEO_PICKER_COLS * GEO_THUMB_W + (GEO_PICKER_COLS - 1) * 10)
#define GEO_GRID_H    (SCREEN_H - GEO_PILL_H - GEO_PILL_BOTTOM)
#define GEO_LIST_H    (SCREEN_H - GEO_LIST_TOP - GEO_LIST_BOTTOM)
#define GEO_PICKER_H  (SCREEN_H - GEO_PICKER_TOP - GEO_PICKER_BOTTOM)

#endif /* PIP_UI_GEOMETRY_H */
