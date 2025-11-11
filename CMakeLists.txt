# SPDX-License-Identifier: MIT

zephyr_library()

zephyr_library_sources_ifdef(CONFIG_ZMK_LONG_PRESS_WAKE src/long_press_wake.c)
