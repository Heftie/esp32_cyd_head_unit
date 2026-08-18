#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Default screen: one tile per data_hub channel, created on first sight
// and refreshed on a timer. Nothing here assumes a fixed set of channels
// — the same screen works whether the companion MCU exposes one reading
// or ten. Tapping a tile opens the measurement screen for that channel.
void tiles_screen_create(void);

#ifdef __cplusplus
}
#endif
