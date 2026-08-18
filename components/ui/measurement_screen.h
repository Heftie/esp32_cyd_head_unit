#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Multimeter-style detail view for one data_hub channel: current value at
// large scale, running MIN/MAX/AVG since a Start press, and Reset.
void measurement_screen_create(void);

// Sets which channel the measurement screen will show the next time it's
// entered. Call this before screen_push("measurement") — e.g. from a
// tile's click handler in tiles_screen.c.
void measurement_screen_set_channel(const char *name);

#ifdef __cplusplus
}
#endif
