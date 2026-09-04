idf_component_register(
    SRCS
        "app_main.c" "config.c" "board.c" "storage.c" "lv_mem_psram.c"
        "audio.c" "opus_file.c"
        "voice.c" "voice_infer.cc"
        "voice_model_wake.c" "voice_model_confirm.c"
        "net_wifi.c" "net_http.c" "net_mqtt.c" "sync.c" "net_check.c"
        "provisioning.c" "power.c" "ota.c" "theme.c"
        "ui/ui_core.c" "ui/ui_theme.c" "ui/ui_home.c" "ui/ui_record.c"
        "ui/ui_inbox.c" "ui/ui_playback.c" "ui/ui_pinpad.c"
        "ui/ui_settings.c" "ui/ui_theme_picker.c"
        "ui/ui_wifi_setup.c" "ui/ui_splash.c" "ui/ui_offline.c"
        "ui/ui_ambient.c" "ui/ui_voice.c"
        "ui/ui_react.c"
        "ui/assets/emoji_heart.c" "ui/assets/emoji_up.c"
        "ui/assets/emoji_down.c" "ui/assets/emoji_joy.c"
    INCLUDE_DIRS "." "ui"
    EMBED_TXTFILES "portal.html"
    # managed components from idf_component.yml (BSP, lvgl, littlefs) are
    # added to this component's requirements automatically
    PRIV_REQUIRES
        nvs_flash esp_wifi esp_netif esp_event esp_http_client
        esp_http_server esp_https_ota mqtt json esp-tls app_update
        esp_driver_i2c opus mbedtls esp_timer
        espressif__esp_lcd_touch)

# lv_mem_psram.c defines LVGL's custom-malloc hooks; nothing in this
# component references them, so force the linker to pull the object in
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_malloc_core")

# --- board selection (PIP_BOARD set in the project CMakeLists) ---
# The define picks the ui_geometry.h block; PIP_BOARD_NAME is the server
# boards.py key printed in the PIP-HW line and sent on theme downloads.
if(PIP_BOARD STREQUAL "amoled-1.8")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PIP_BOARD_AMOLED_1_8)
elseif(PIP_BOARD STREQUAL "amoled-1.75b")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PIP_BOARD_AMOLED_1_75B)
    message(STATUS "pip: amoled-1.75b build (BSP 3.x path; NOT yet "
                   "bench-tested on hardware)")
elseif(PIP_BOARD STREQUAL "amoled-2.16")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PIP_BOARD_AMOLED_2_16)
    message(STATUS "pip: amoled-2.16 build (BSP 3.x path; NOT yet "
                   "bench-tested on hardware)")
else()
    message(FATAL_ERROR "unknown PIP_BOARD '${PIP_BOARD}' "
                        "(amoled-1.8 | amoled-1.75b | amoled-2.16)")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE
                           PIP_BOARD_NAME="${PIP_BOARD}")
