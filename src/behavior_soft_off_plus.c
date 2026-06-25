/*
 * SPDX-License-Identifier: MIT
 *
 * Enhanced soft-off behavior.
 *
 * Declared with GLOBAL locality, exactly like ZMK's built-in &soft_off: when
 * bound in a keymap, ZMK runs it on the central AND relays it to every
 * peripheral, so each half runs the behavior and powers *itself* off. This is
 * the reliable "both halves off" path and does not depend on the cross-half
 * GATT signal.
 *
 * A dedicated power key wired through kscan-sideband-behaviors bypasses locality
 * and runs the behavior on only the half it is wired to; there
 * zmk_soft_off_plus_signal_peers() is what powers the other half off. A
 * one-shot claim guard keeps the relayed run and the cross-half signal from
 * powering a half off twice.
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
    /* Both the keymap-relayed run (GLOBAL locality) and an incoming cross-half
     * off-signal can land on the same half; claim the one-shot so we power off
     * only once. */
    if (!zmk_soft_off_plus_claim_off()) {
        LOG_DBG("soft-off-plus: power-off already in progress");
        return;
    }

    LOG_INF("soft-off-plus: triggering soft off");

    /* Ask the other half/halves to power off as well. Redundant on the keymap
     * path (GLOBAL locality already relays the behavior to each half) but
     * required when a sideband key runs this on only one half. No-op on a
     * non-split or split-sync-disabled build. */
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
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
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
