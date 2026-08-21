#include "ui.h"

#include "graph_screen.h"
#include "log_manager_screen.h"
#include "measurement_screen.h"
#include "screen_nav.h"
#include "sd_info_screen.h"
#include "set_time_screen.h"
#include "settings_screen.h"
#include "tiles_screen.h"
#include "time_settings_screen.h"
#include "timezone_screen.h"
#include "wifi_settings_screen.h"

void ui_init(void)
{
    screen_register("tiles", tiles_screen_create);
    screen_register("settings", settings_screen_create);
    screen_register("measurement", measurement_screen_create);
    screen_register("set_time", set_time_screen_create);
    screen_register("sd_info", sd_info_screen_create);
    screen_register("log_manager", log_manager_screen_create);
    screen_register("graph", graph_screen_create);
    screen_register("graph_config", graph_config_screen_create);
    screen_register("timezone", timezone_screen_create);
    screen_register("time_settings", time_settings_screen_create);
    screen_register("wifi_settings", wifi_settings_screen_create);
    screen_activate(SCREEN_HOME);
}
