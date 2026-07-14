/*
 * SPDX-License-Identifier: MIT
 *
 * Forwards to a child behavior only while USB is connected/powered, otherwise
 * does nothing. Each half evaluates its own USB/VBUS state, so it is handy to
 * gate a destructive action like &bootloader so it can only fire on the half
 * that is actually plugged in.
 */

#define DT_DRV_COMPAT zmk_behavior_if_usb

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>
#include <zmk/keymap.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT)
#include <zmk/events/position_state_changed.h>
#endif

LOG_MODULE_DECLARE(zmk_soft_off_plus, CONFIG_ZMK_SOFT_OFF_PLUS_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/usb.h>
static inline bool if_usb_powered(void) { return zmk_usb_is_powered(); }
#elif IS_ENABLED(CONFIG_SOC_FAMILY_NORDIC_NRF)
/* ZMK_USB is central-only on a split. Read VBUS directly so a Nordic
 * peripheral can still gate its own bootloader while plugged in. */
#include <hal/nrf_power.h>
static inline bool if_usb_powered(void) {
#if NRF_POWER_HAS_USBREG
    return nrf_power_usbregstatus_vbusdet_get(NRF_POWER);
#else
    return false;
#endif
}
#else
static inline bool if_usb_powered(void) { return false; }
#endif

struct behavior_if_usb_config {
    struct zmk_behavior_binding binding;
};

struct behavior_if_usb_data {
    bool forwarded;
};

static int if_usb_invoke_child(const struct behavior_if_usb_config *config,
                               struct zmk_behavior_binding_event event, bool pressed) {
    struct zmk_behavior_binding child = config->binding;

#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    /* Sideband events do not carry a local source in pinned ZMK. Make the
     * selected child (notably &bootloader) execute on the half whose VBUS was
     * just checked instead of being routed to peripheral index zero. */
    event.source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL;
#endif

    return zmk_behavior_invoke_binding(&child, event, pressed);
}

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
    return if_usb_invoke_child(config, event, true);
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
    return if_usb_invoke_child(config, event, false);
}

static const struct behavior_driver_api behavior_if_usb_driver_api = {
    .binding_pressed = if_usb_pressed,
    .binding_released = if_usb_released,
    /* Keep the wrapper on the MCU currently handling the binding. A sideband
     * tap-dance runs on its physical half, including a split peripheral, while
     * a regular keymap binding runs on the central. */
    .locality = BEHAVIOR_LOCALITY_CENTRAL,
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
