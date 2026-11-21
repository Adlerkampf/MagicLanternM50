/* test68: the old full-sensor bin recorder (raw_bin.c) is removed for code room; CRX recording replaces it.
 * These stubs keep the menu and the vsync hook linking. */
#include <stdint.h>

int m50_bin_enabled = 0, m50_bin_cached_read = 0, m50_bin_fast = 0, m50_bin_core = 0;
uint32_t m50_bin_hook(void) { return 0; }
int m50_bin_take_redirect(void *ptr) { (void) ptr; return 0; }
int m50_bin_core_alive(void) { return 0; }
uint32_t m50_bin_cache_mismatches(void) { return 0; }
void m50_bin_stats(uint32_t *frames, uint32_t *dropped, int *last_us, int *max_us, int *fps_x10, int *state)
{
    *frames = *dropped = 0; *last_us = *max_us = *fps_x10 = *state = 0;
}
int m50_bin_geometry(void **buf, int *w, int *h, int *skip_l, int *skip_t) { (void) buf; (void) w; (void) h; (void) skip_l; (void) skip_t; return 0; }
