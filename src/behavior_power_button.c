/*
 * SPDX-License-Identifier: MIT
 *
 * Dedicated sideband power-button gesture.
 *
 * ZMK's general tap-dance behavior is intentionally central-only because it
 * listens to every key-position event to implement interruption semantics.
 * A split peripheral must forward those events untouched. This smaller
 * behavior consumes only the sideband press/release edges delivered directly
 * to it: first press held past the tapping term selects child 0, while a third
 * quick press selects child 1. Short single and double taps do nothing.
 */

#define DT_DRV_COMPAT zmk_behavior_power_button

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/keymap.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT)
#include <zmk/events/position_state_changed.h>
#endif

LOG_MODULE_DECLARE(zmk_soft_off_plus, CONFIG_ZMK_SOFT_OFF_PLUS_LOG_LEVEL);

enum power_button_state {
    POWER_BUTTON_IDLE,
    POWER_BUTTON_COLLECTING,
    POWER_BUTTON_HOLD_ACTIVE,
    POWER_BUTTON_TRIPLE_ACTIVE,
};

struct behavior_power_button_config {
    uint32_t tapping_term_ms;
    struct zmk_behavior_binding bindings[2];
};

struct behavior_power_button_data {
    const struct device *dev;
    struct k_work_delayable decision_work;
    struct zmk_behavior_binding_event event;
    enum power_button_state state;
    uint8_t press_count;
    bool pressed;
};

static void power_button_reset(struct behavior_power_button_data *data) {
    data->state = POWER_BUTTON_IDLE;
    data->press_count = 0;
    data->pressed = false;
}

static int power_button_invoke(const struct behavior_power_button_config *config, size_t index,
                               struct zmk_behavior_binding_event event, bool pressed) {
    /* kscan-sideband-behaviors invokes this wrapper directly on the physical
     * half, bypassing ZMK's locality dispatcher. Keep the selected child on
     * that same half too. Calling zmk_behavior_invoke_binding() here would
     * re-enter locality dispatch; a GLOBAL child such as &soft_off_plus would
     * then create a false held-key state on the other half. */
    struct zmk_behavior_binding child = config->bindings[index];

#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    /* Preserve physical-half source semantics for nested EVENT_SOURCE
     * behaviors, including &if_usb -> &bootloader. */
    event.source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL;
#endif
    return pressed ? behavior_keymap_binding_pressed(&child, event)
                   : behavior_keymap_binding_released(&child, event);
}

static void power_button_decision_work_cb(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct behavior_power_button_data *data =
        CONTAINER_OF(dwork, struct behavior_power_button_data, decision_work);
    const struct behavior_power_button_config *config = data->dev->config;

    if (data->state != POWER_BUTTON_COLLECTING) {
        return;
    }

    if (data->press_count == 1 && data->pressed) {
        /* Match the old tap-dance timing: commit the hold child after the
         * tapping term, then keep it pressed until the physical release. */
        data->state = POWER_BUTTON_HOLD_ACTIVE;
        data->event.timestamp = k_uptime_get();
        int err = power_button_invoke(config, 0, data->event, true);
        if (err < 0) {
            LOG_ERR("power-button: hold binding press failed (%d)", err);
            power_button_reset(data);
        }
        return;
    }

    /* A released single tap or any double tap intentionally does nothing. */
    power_button_reset(data);
}

static int power_button_pressed(struct zmk_behavior_binding *binding,
                                struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct behavior_power_button_data *data = dev->data;
    const struct behavior_power_button_config *config = dev->config;

    if (data->pressed || data->state == POWER_BUTTON_HOLD_ACTIVE ||
        data->state == POWER_BUTTON_TRIPLE_ACTIVE) {
        return ZMK_BEHAVIOR_OPAQUE;
    }

    data->pressed = true;
    data->event = event;

    if (data->state == POWER_BUTTON_IDLE) {
        data->state = POWER_BUTTON_COLLECTING;
        data->press_count = 1;
    } else {
        data->press_count++;
    }

    if (data->press_count >= 3) {
        k_work_cancel_delayable(&data->decision_work);
        data->state = POWER_BUTTON_TRIPLE_ACTIVE;
        int err = power_button_invoke(config, 1, event, true);
        if (err < 0) {
            LOG_ERR("power-button: triple-tap binding press failed (%d)", err);
            power_button_reset(data);
        }
        return ZMK_BEHAVIOR_OPAQUE;
    }

    k_work_reschedule(&data->decision_work, K_MSEC(config->tapping_term_ms));
    return ZMK_BEHAVIOR_OPAQUE;
}

static int power_button_released(struct zmk_behavior_binding *binding,
                                 struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct behavior_power_button_data *data = dev->data;
    const struct behavior_power_button_config *config = dev->config;

    if (!data->pressed) {
        return ZMK_BEHAVIOR_OPAQUE;
    }
    data->pressed = false;

    if (data->state == POWER_BUTTON_HOLD_ACTIVE) {
        power_button_reset(data);
        int err = power_button_invoke(config, 0, event, false);
        if (err < 0) {
            LOG_ERR("power-button: hold binding release failed (%d)", err);
        }
    } else if (data->state == POWER_BUTTON_TRIPLE_ACTIVE) {
        power_button_reset(data);
        int err = power_button_invoke(config, 1, event, false);
        if (err < 0) {
            LOG_ERR("power-button: triple-tap binding release failed (%d)", err);
        }
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int behavior_power_button_init(const struct device *dev) {
    struct behavior_power_button_data *data = dev->data;
    data->dev = dev;
    power_button_reset(data);
    k_work_init_delayable(&data->decision_work, power_button_decision_work_cb);
    return 0;
}

static const struct behavior_driver_api behavior_power_button_driver_api = {
    .binding_pressed = power_button_pressed,
    .binding_released = power_button_released,
    .locality = BEHAVIOR_LOCALITY_CENTRAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

#define POWER_BUTTON_INST(n)                                                                      \
    BUILD_ASSERT(DT_INST_PROP_LEN(n, bindings) == 2,                                              \
                 "zmk,behavior-power-button requires exactly two bindings");                    \
    static const struct behavior_power_button_config power_button_config_##n = {                  \
        .tapping_term_ms = DT_INST_PROP(n, tapping_term_ms),                                      \
        .bindings = {ZMK_KEYMAP_EXTRACT_BINDING(0, DT_DRV_INST(n)),                               \
                     ZMK_KEYMAP_EXTRACT_BINDING(1, DT_DRV_INST(n))},                              \
    };                                                                                            \
    static struct behavior_power_button_data power_button_data_##n;                              \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_power_button_init, NULL, &power_button_data_##n,          \
                            &power_button_config_##n, POST_KERNEL,                                \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                  \
                            &behavior_power_button_driver_api);

DT_INST_FOREACH_STATUS_OKAY(POWER_BUTTON_INST)
