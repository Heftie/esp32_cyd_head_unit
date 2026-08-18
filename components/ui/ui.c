#include "ui.h"

#include "measurement_screen.h"
#include "screen_nav.h"
#include "settings_screen.h"
#include "tiles_screen.h"
#include "touch_test_screen.h"

void ui_init(void)
{
    screen_register("tiles", tiles_screen_create);
    screen_register("touch_test", touch_test_screen_create);
    screen_register("settings", settings_screen_create);
    screen_register("measurement", measurement_screen_create);
    screen_activate(SCREEN_HOME);
}
