/*
 * SPDX-License-Identifier: MIT
 *
 * Enhanced soft-off behavior.
 *
 * Declared with central locality so that, when bound in a keymap, it always
 * runs on the central where it can drive both halves. When invoked locally on a
 * peripheral (e.g. via a dedicated power key wired through
 * kscan-sideband-behaviors) it still works: the hold is timed on whichever half
 * runs the behavior, and zmk_soft_off_plus_signal_peers() takes care of telling
 * the other half to power off too.
 */

#define DT_DRV_COMPAT zmk_behavior_soft_off_plus

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>
#include <zmk/pm.h>
#include <zmk/soft_off_plus/split_sync.h>

LOG_MODULE_DECLARE(zmk_soft_off_plus, CONFIG_ZMK_SOFT_OFF_PLUS_LOG_LEVEL);

struct behavior_soft_off_plus_config {
    uint32_t hold_time_ms;
};

struct behavior_soft_off_plus_data {
    uint32_t press_start;
};

static void soft_off_plus_trigger(void) {
    LOG_INF("soft-off-plus: triggering soft off");

    /* Ask the other half/halves to power off as well (no-op on a non-split or
     * split-sync-disabled build). */
    zmk_soft_off_plus_signal_peers();

#if CONFIG_ZMK_SOFT_OFF_PLUS_SPLIT_SYNC_FLUSH_MS > 0
    /* Give the BLE write/notification time to be transmitted before we cut
     * power and drop the connection. */
    k_msleep(CONFIG_ZMK_SOFT_OFF_PLUS_SPLIT_SYNC_FLUSH_MS);
#endif

    zmk_pm_soft_off();
}

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct behavior_soft_off_plus_data *data = dev->data;

    data->press_start = k_uptime_get();

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct behavior_soft_off_plus_data *data = dev->data;
    const struct behavior_soft_off_plus_config *config = dev->config;

    if (config->hold_time_ms == 0) {
        soft_off_plus_trigger();
        return ZMK_BEHAVIOR_OPAQUE;
    }

    uint32_t hold = k_uptime_get() - data->press_start;
    if (hold >= config->hold_time_ms) {
        soft_off_plus_trigger();
    } else {
        LOG_INF("soft-off-plus: held %u ms < %u ms; not triggering", hold, config->hold_time_ms);
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_soft_off_plus_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
    .locality = BEHAVIOR_LOCALITY_CENTRAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

#define SOP_INST(n)                                                                                \
    static const struct behavior_soft_off_plus_config bsop_config_##n = {                          \
        .hold_time_ms = DT_INST_PROP_OR(n, hold_time_ms, 0),                                       \
    };                                                                                             \
    static struct behavior_soft_off_plus_data bsop_data_##n = {};                                  \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, &bsop_data_##n, &bsop_config_##n, POST_KERNEL,          \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_soft_off_plus_driver_api);

DT_INST_FOREACH_STATUS_OKAY(SOP_INST)
