/*
 * Copyright (c) 2026 Oleksandr Maslov
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/toolchain.h>

LOG_MODULE_DECLARE(zmk_soft_off_plus, CONFIG_ZMK_SOFT_OFF_PLUS_LOG_LEVEL);

static void __maybe_unused sop_enable_deep_sleep_wakeup(const struct device *dev) {
    if (!device_is_ready(dev)) {
        LOG_WRN("Deep-sleep wake device %s is not ready", dev->name);
        return;
    }

    if (pm_device_wakeup_is_capable(dev) && !pm_device_wakeup_enable(dev, true)) {
        LOG_WRN("Failed to wake-enable %s", dev->name);
    }
}

#define SOP_ENABLE_WAKE_DEVICE(node_id) sop_enable_deep_sleep_wakeup(DEVICE_DT_GET(node_id));

#define SOP_ENABLE_SIDEBAND(node_id)                                                        \
    SOP_ENABLE_WAKE_DEVICE(node_id)                                                         \
    SOP_ENABLE_WAKE_DEVICE(DT_PHANDLE(node_id, kscan))

static int sop_deep_sleep_wake_init(void) {
#if DT_HAS_COMPAT_STATUS_OKAY(zmk_kscan_sideband_behaviors) &&                              \
    IS_ENABLED(CONFIG_ZMK_KSCAN_SIDEBAND_BEHAVIORS)
    DT_FOREACH_STATUS_OKAY(zmk_kscan_sideband_behaviors, SOP_ENABLE_SIDEBAND)
#endif

#if DT_HAS_COMPAT_STATUS_OKAY(zmk_kscan_gpio_matrix) &&                                    \
    IS_ENABLED(CONFIG_ZMK_KSCAN_GPIO_MATRIX)
    DT_FOREACH_STATUS_OKAY(zmk_kscan_gpio_matrix, SOP_ENABLE_WAKE_DEVICE)
#endif

    return 0;
}

SYS_INIT(sop_deep_sleep_wake_init, APPLICATION,
         CONFIG_ZMK_SOFT_OFF_PLUS_DEEP_SLEEP_WAKE_INIT_PRIORITY);
