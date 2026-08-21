/* LVGL allocator backed by PSRAM (LV_USE_CUSTOM_MALLOC).
 *
 * The widget tree is ~100 KB of small allocations; with the stock CLIB
 * malloc they all land in internal RAM (below SPIRAM_MALLOC_ALWAYSINTERNAL
 * they never even try PSRAM) and starve the WiFi driver's DMA buffers and
 * the audio task stack. UI objects have no DMA/flash-cache constraints, so
 * put them in PSRAM; fall back to the default heap if PSRAM is exhausted.
 * Draw buffers are unaffected: esp_lvgl_port allocates those itself with
 * explicit DMA caps. */
#include "lvgl.h"
#include "esp_heap_caps.h"
#include <stdlib.h>

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    LV_UNUSED(mem); LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool) { LV_UNUSED(pool); }

void *lv_malloc_core(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(size);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    void *n = heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM);
    return n ? n : realloc(p, new_size);
}

void lv_free_core(void *p) { free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) { LV_UNUSED(mon_p); }

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }
