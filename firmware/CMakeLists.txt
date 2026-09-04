cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "1.3.8")

# Target board (server boards.py key): amoled-1.8 | amoled-1.75b | amoled-2.16.
# Selects the UI geometry block (main/ui/ui_geometry.h) and PIP_BOARD_NAME;
# validated in main/CMakeLists.txt. sdkconfig is generated once per build
# dir, so build each board in its own dir: idf.py -B build-<board>
# -DPIP_BOARD=<board> build (CI does exactly that).
if(NOT DEFINED PIP_BOARD)
    set(PIP_BOARD "amoled-1.8")
endif()
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.${PIP_BOARD}")
    set(SDKCONFIG_DEFAULTS
        "sdkconfig.defaults;sdkconfig.defaults.${PIP_BOARD}")
endif()
# every board keeps its own generated sdkconfig (gitignored): the shared
# default-path file belongs to the 1.8 and must not be reused - or
# clobbered - by another board's configure
if(NOT PIP_BOARD STREQUAL "amoled-1.8")
    set(SDKCONFIG "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.${PIP_BOARD}")
endif()

# The board's component manifest: the three Waveshare BSPs export
# identically named headers (bsp/esp-bsp.h), so exactly one may be a
# dependency - main/idf_component.yml is gitignored and generated here
# from the tracked per-board manifest. NOTE: dependencies.lock is
# rewritten by the component manager per manifest; the committed lock
# belongs to the 1.8 - after a local cross-board build, restore it
# (git restore dependencies.lock). CI builds each board from a clean
# checkout.
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/main/idf_component.${PIP_BOARD}.yml")
    message(FATAL_ERROR "no manifest for PIP_BOARD '${PIP_BOARD}'")
endif()
configure_file("${CMAKE_CURRENT_LIST_DIR}/main/idf_component.${PIP_BOARD}.yml"
               "${CMAKE_CURRENT_LIST_DIR}/main/idf_component.yml" COPYONLY)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pip)

# Waveshare BSPs have log lines that IDF >= 5.4 flags under
# -Werror=format; don't fail the build on third-party warnings. Only the
# selected board's BSP exists in the build (resolve hard-fails on absent
# components, so check the component list first).
idf_build_get_property(_pip_components BUILD_COMPONENTS)
foreach(ws_bsp waveshare__esp32_s3_touch_amoled_1_8
               waveshare__esp32_s3_touch_amoled_1_75
               waveshare__esp32_s3_touch_amoled_2_16)
    if(ws_bsp IN_LIST _pip_components)
        idf_component_get_property(ws_lib ${ws_bsp} COMPONENT_LIB)
        if(ws_lib)
            target_compile_options(${ws_lib} PRIVATE -Wno-error=format)
        endif()
        unset(ws_lib)
    endif()
endforeach()
