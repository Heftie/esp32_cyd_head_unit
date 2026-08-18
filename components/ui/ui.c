#include "ui.h"

#include "measurement_screen.h"
#include "screen_nav.h"
#include "sd_info_screen.h"
#include "set_time_screen.h"
#include "settings_screen.h"
#include "tiles_screen.h"

void ui_init(void)
{
    screen_register("tiles", tiles_screen_create);
    screen_register("settings", settings_screen_create);
    screen_register("measurement", measurement_screen_create);
    screen_register("set_time", set_time_screen_create);
    screen_register("sd_info", sd_info_screen_create);
    screen_activate(SCREEN_HOME);
}
