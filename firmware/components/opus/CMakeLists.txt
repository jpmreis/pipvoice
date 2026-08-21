# libopus 1.4 as an ESP-IDF component (fixed point, no float API)
file(GLOB OPUS_SRC
    opus/src/*.c opus/celt/*.c opus/silk/*.c opus/silk/fixed/*.c)
list(FILTER OPUS_SRC EXCLUDE REGEX ".*(demo|compare).*")
idf_component_register(
    SRCS ${OPUS_SRC}
    INCLUDE_DIRS opus/include
    PRIV_INCLUDE_DIRS opus opus/src opus/celt opus/silk opus/silk/fixed)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    OPUS_BUILD FIXED_POINT DISABLE_FLOAT_API USE_ALLOCA)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-maybe-uninitialized -Wno-stringop-overread -Wno-stringop-overflow)
