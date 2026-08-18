#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Diagnostics screen — raw touch coordinates, useful after any hardware
// change. Reachable from the settings screen; Back returns to whichever
// screen pushed it here.
void touch_test_screen_create(void);

#ifdef __cplusplus
}
#endif
