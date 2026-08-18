cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(esp32-2432S028R)

# Use the following to define CYD_ILI9341 for the 'old' boards with only micro usb, using the ili9341 driver.
#
# The newer boards, with both micro-usb and an usb-c connector use a st7789 display driver.
#
#idf_build_set_property(COMPILE_OPTIONS "-DCYD_ILI9341=1" APPEND)

# Use the following to define SIM_DATA to run without a companion MCU: a
# task in main.c publishes two fake data_hub channels (VOLT:DC, TEMP) with
# random values every 500ms, so the tile UI, logger, and web dashboard all
# have something to show. Never on by default — this is a dev/bench aid,
# not something that should ship enabled.
idf_build_set_property(COMPILE_OPTIONS "-DSIM_DATA=1" APPEND)
