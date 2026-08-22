cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "0.1.34")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pip)

# Waveshare BSP 2.0.3 has a %02X/uint32_t log line that IDF >= 5.4 flags
# under -Werror=format; don't fail the build on third-party warnings.
idf_component_get_property(ws_lib waveshare__esp32_s3_touch_amoled_1_8 COMPONENT_LIB)
if(ws_lib)
    target_compile_options(${ws_lib} PRIVATE -Wno-error=format)
endif()
