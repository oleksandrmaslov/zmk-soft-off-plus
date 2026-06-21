# SPDX-License-Identifier: MIT

zephyr_library()

zephyr_include_directories(include)

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
