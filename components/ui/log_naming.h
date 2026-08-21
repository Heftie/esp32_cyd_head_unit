#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Fills out with a fresh "YYYYMMDD_HHMMSS_log.csv" name from the current
// UTC wall clock (web_server_get_wall_clock()) — the "just start logging"
// naming tiles' Start button has always used, factored out here so
// log_manager's "Default name" button (and the web dashboard's
// equivalent, via /api/time) generate the exact same pattern instead of
// a second hand-rolled copy of the same snprintf. Returns false, leaving
// out untouched, if wall clock isn't available yet.
bool log_naming_default_filename(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
