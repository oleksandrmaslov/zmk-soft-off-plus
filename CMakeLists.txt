# SPDX-License-Identifier: MIT

zephyr_library()

zephyr_include_directories(include)

# Boot-time "hold to wake" delay.
zephyr_library_sources_ifdef(CONFIG_ZMK_SOFT_OFF_PLUS_WAKE_DELAY src/wake_delay.c)

# Enhanced soft-off behavior + cross-half signalling. split_sync.c always
# compiles under the master switch and degrades to a no-op when split sync is
# disabled, so the behavior can call it unconditionally.
zephyr_library_sources_ifdef(CONFIG_ZMK_SOFT_OFF_PLUS src/behavior_soft_off_plus.c)
zephyr_library_sources_ifdef(CONFIG_ZMK_SOFT_OFF_PLUS src/split_sync.c)
