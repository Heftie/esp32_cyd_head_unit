#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Live line graph for one data_hub channel — value at large scale isn't
// the point here (that's measurement_screen), watching it move over time
// is. Reached by long-pressing a tile on the dashboard.
void graph_screen_create(void);

// Sets which channel the graph screen shows the next time it's entered.
// Call before screen_push("graph") — same pattern as
// measurement_screen_set_channel().
void graph_screen_set_channel(const char *name);

// Channel/tick-rate/time-span picker for the graph — a separate screen
// rather than controls crammed onto the chart view, since a 320x240
// display doesn't have room for both a readable chart and three rollers
// at once. Reached from the graph screen's "Cfg" button.
void graph_config_screen_create(void);

#ifdef __cplusplus
}
#endif
