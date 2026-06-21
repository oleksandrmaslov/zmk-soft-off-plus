/*
 * SPDX-License-Identifier: MIT
 *
 * Forwards to a child behavior only while USB is connected/powered, otherwise
 * does nothing. Each half evaluates its own USB state, so it is handy to gate a
 * destructive action like &bootloader so it can only fire on the half that is
 * actually plugged in.
 */

#define DT_DRV_COMPAT zmk_behavior_if_usb

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>
#include <zmk/keymap.h>

LOG_MODULE_DECLARE(zmk_soft_off_plus, CONFIG_ZMK_SOFT_OFF_PLUS_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/usb.h>
static inline bool if_usb_powered(void) { return zmk_usb_is_powered(); }
#else
static inline bool if_usb_powered(void) { return false; }
#endif

struct behavior_if_usb_config {
    struct zmk_behavior_binding binding;
};

struct behavior_if_usb_data {
    bool forwarded;
};

static int if_usb_pressed(struct zmk_behavior_binding *binding,
                          struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct behavior_if_usb_config *config = dev->config;
    struct behavior_if_usb_data *data = dev->data;

    data->forwarded = false;
    if (!if_usb_powered()) {
        LOG_INF("if-usb: USB not connected, ignoring");
        return ZMK_BEHAVIOR_OPAQUE;
    }

    data->forwarded = true;
    struct zmk_behavior_binding child = config->binding;
    return zmk_behavior_invoke_binding(&child, event, true);
}

static int if_usb_released(struct zmk_behavior_binding *binding,
                           struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct behavior_if_usb_config *config = dev->config;
    struct behavior_if_usb_data *data = dev->data;

    if (!data->forwarded) {
        return ZMK_BEHAVIOR_OPAQUE;
    }

    data->forwarded = false;
    struct zmk_behavior_binding child = config->binding;
    return zmk_behavior_invoke_binding(&child, event, false);
}

static const struct behavior_driver_api behavior_if_usb_driver_api = {
    .binding_pressed = if_usb_pressed,
    .binding_released = if_usb_released,
    .locality = BEHAVIOR_LOCALITY_EVENT_SOURCE,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

#define IF_USB_INST(n)                                                                              \
    static const struct behavior_if_usb_config behavior_if_usb_config_##n = {                       \
        .binding = ZMK_KEYMAP_EXTRACT_BINDING(0, DT_DRV_INST(n)),                                   \
    };                                                                                              \
    static struct behavior_if_usb_data behavior_if_usb_data_##n = {};                               \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, &behavior_if_usb_data_##n, &behavior_if_usb_config_##n,  \
                            POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                       \
                            &behavior_if_usb_driver_api);

DT_INST_FOREACH_STATUS_OKAY(IF_USB_INST)
