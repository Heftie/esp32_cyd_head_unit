#include "log_naming.h"

#include <stdio.h>
#include <time.h>

#include "web_server.h"

bool log_naming_default_filename(char *out, size_t out_len)
{
    time_t now;
    if (!web_server_get_wall_clock(&now)) {
        return false;
    }
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    snprintf(out, out_len, "%04d%02d%02d_%02d%02d%02d_log.csv",
             tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
             tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
    return true;
}
