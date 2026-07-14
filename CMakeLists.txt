# SPDX-License-Identifier: MIT

zephyr_library()

zephyr_include_directories(include)

# ZMK app headers (zmk/pm.h, zmk/behavior.h, zmk/ble.h, drivers/behavior.h, ...).
# ZMK marks its app include dir PRIVATE, so modules must add it explicitly.
zephyr_include_directories(${APPLICATION_SOURCE_DIR}/include)

# Shared log module registration (compiled whenever the module is enabled).
zephyr_library_sources_ifdef(CONFIG_ZMK_SOFT_OFF_PLUS src/soft_off_plus.c)

# Enhanced soft-off behavior + cross-half "both halves off" signalling.
# split_sync.c backs the behavior and degrades to a no-op when split sync is
# disabled, so the two are compiled together.
zephyr_library_sources_ifdef(CONFIG_ZMK_SOFT_OFF_PLUS_BEHAVIOR src/behavior_soft_off_plus.c)
zephyr_library_sources_ifdef(CONFIG_ZMK_SOFT_OFF_PLUS_BEHAVIOR src/split_sync.c)

# Boot-time "hold to wake" delay.
zephyr_library_sources_ifdef(CONFIG_ZMK_SOFT_OFF_PLUS_WAKE_DELAY src/wake_delay.c)

# USB-gated wrapper, e.g. to put &bootloader on a dedicated power/soft-off key.
zephyr_library_sources_ifdef(CONFIG_ZMK_SOFT_OFF_PLUS_IF_USB src/behavior_if_usb.c)

# Dedicated sideband power-button gesture: hold for soft off, triple-tap for
# the USB-gated bootloader. Unlike ZMK's keymap tap-dance behavior, this has no
# global position-event listener and is therefore safe on a split peripheral.
zephyr_library_sources_ifdef(CONFIG_ZMK_SOFT_OFF_PLUS_POWER_BUTTON src/behavior_power_button.c)
