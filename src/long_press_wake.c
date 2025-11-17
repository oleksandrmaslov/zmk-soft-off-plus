// SPDX-License-Identifier: MIT
#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/pm/pm.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/pm/state.h>

#ifndef CONFIG_ZMK_LONG_PRESS_WAKE_LOG_LEVEL
#define CONFIG_ZMK_LONG_PRESS_WAKE_LOG_LEVEL LOG_LEVEL_INF
#endif

LOG_MODULE_REGISTER(zmk_long_press_wake, CONFIG_ZMK_LONG_PRESS_WAKE_LOG_LEVEL);

#define LPW_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(zmk_long_press_wake)

#if DT_NODE_HAS_STATUS(LPW_NODE, okay)

#define LPW_REQUIRED_HOLD_MS DT_PROP_OR(LPW_NODE, required_hold_ms, 3000)
#define LPW_EXT_POWER_ON_DELAY_MS DT_PROP_OR(LPW_NODE, ext_power_on_delay_ms, 0)

#if DT_NODE_HAS_PROP(LPW_NODE, bypass_on_usb)
#define LPW_BYPASS_ON_USB 1
#else
#define LPW_BYPASS_ON_USB 0
#endif

#if DT_NODE_HAS_PROP(LPW_NODE, active_low)
#define LPW_WAKE_ACTIVE_LOW 1
#else
#define LPW_WAKE_ACTIVE_LOW 0
#endif

#define LPW_WAKE_GPIO_INIT(node_id, prop, idx) \
    GPIO_DT_SPEC_GET_BY_IDX(node_id, prop, idx),

static const struct gpio_dt_spec wake_gpios[] = {
    DT_FOREACH_PROP_ELEM(LPW_NODE, wake_gpios, LPW_WAKE_GPIO_INIT)
};

#define LPW_NUM_WAKE_GPIOS ARRAY_SIZE(wake_gpios)

#if DT_NODE_HAS_PROP(LPW_NODE, ext_power_gpios)
#define LPW_HAS_EXT_POWER 1
#define LPW_EXT_POWER_GPIO_INIT(node_id, prop, idx) \
    GPIO_DT_SPEC_GET_BY_IDX(node_id, prop, idx),

static const struct gpio_dt_spec ext_power_gpios[] = {
    DT_FOREACH_PROP_ELEM(LPW_NODE, ext_power_gpios, LPW_EXT_POWER_GPIO_INIT)
};

#define LPW_NUM_EXT_POWER_GPIOS ARRAY_SIZE(ext_power_gpios)
#else
#define LPW_HAS_EXT_POWER 0
#define LPW_NUM_EXT_POWER_GPIOS 0
#endif

#else /* DT_NODE_HAS_STATUS(LPW_NODE, okay) */

#define LPW_REQUIRED_HOLD_MS 3000
#define LPW_EXT_POWER_ON_DELAY_MS 0
#define LPW_BYPASS_ON_USB 0
#define LPW_WAKE_ACTIVE_LOW 0
#define LPW_NUM_WAKE_GPIOS 0
#define LPW_HAS_EXT_POWER 0
#define LPW_NUM_EXT_POWER_GPIOS 0

#endif /* DT_NODE_HAS_STATUS(LPW_NODE, okay) */

static bool zmk_long_press_wake_usb_present(void)
{
#if LPW_BYPASS_ON_USB && defined(CONFIG_USB_DEVICE_STACK)
    return true;
#else
    return false;
#endif
}

#if LPW_HAS_EXT_POWER
static void zmk_long_press_wake_set_ext_power(bool enable)
{
    for (size_t i = 0; i < LPW_NUM_EXT_POWER_GPIOS; i++) {
        if (!gpio_is_ready_dt(&ext_power_gpios[i])) {
            continue;
        }

        int ret = gpio_pin_set_dt(&ext_power_gpios[i], enable);
        if (ret < 0) {
            LOG_ERR("Failed to set ext-power GPIO %s:%u (%d)",
                    ext_power_gpios[i].port->name, ext_power_gpios[i].pin, ret);
        }
    }
}

static void zmk_long_press_wake_configure_ext_power(void)
{
    for (size_t i = 0; i < LPW_NUM_EXT_POWER_GPIOS; i++) {
        if (!gpio_is_ready_dt(&ext_power_gpios[i])) {
            LOG_ERR("Ext-power GPIO %s not ready", ext_power_gpios[i].port->name);
            continue;
        }

        int ret = gpio_pin_configure_dt(&ext_power_gpios[i], GPIO_OUTPUT_INACTIVE);
        if (ret < 0) {
            LOG_ERR("Failed to configure ext-power GPIO %s:%u (%d)",
                    ext_power_gpios[i].port->name, ext_power_gpios[i].pin, ret);
        }
    }
}

static void zmk_long_press_wake_enable_ext_power(void)
{
    if (LPW_EXT_POWER_ON_DELAY_MS > 0) {
        k_msleep(LPW_EXT_POWER_ON_DELAY_MS);
    }

    zmk_long_press_wake_set_ext_power(true);
}
#else
static void zmk_long_press_wake_configure_ext_power(void)
{
}

static void zmk_long_press_wake_enable_ext_power(void)
{
}
#endif /* LPW_HAS_EXT_POWER */

static bool zmk_long_press_wake_gpio_active(const struct gpio_dt_spec *spec)
{
    int val = gpio_pin_get_dt(spec);

    if (val < 0) {
        LOG_ERR("Failed to read wake GPIO %s:%u (%d)", spec->port->name, spec->pin, val);
        return false;
    }

#if LPW_WAKE_ACTIVE_LOW
    return val == 0;
#else
    return val > 0;
#endif
}

#if defined(CONFIG_PM)
static void zmk_long_press_wake_request_low_power(void)
{
    static const struct {
        enum pm_state state;
        const char *name;
    } candidates[] = {
        { PM_STATE_SUSPEND_TO_IDLE, "SUSPEND_TO_IDLE" },
        { PM_STATE_SOFT_OFF, "SOFT_OFF" },
    };

    for (size_t i = 0; i < ARRAY_SIZE(candidates); i++) {
        struct pm_state_info info = {
            .state = candidates[i].state,
            .substate_id = 0,
            .min_residency_us = 0,
            .exit_latency_us = 0,
        };

        int ret = pm_state_force(0, info);

        if (ret == 0) {
            LOG_INF("Returning to %s after invalid wake", candidates[i].name);
            return;
        }

        LOG_WRN("Failed to enter %s (%d)", candidates[i].name, ret);
    }

    LOG_WRN("Unable to request any low-power state after invalid wake");
}
#else
static void zmk_long_press_wake_request_low_power(void)
{
    LOG_WRN("Power management not enabled; cannot request low-power state");
}
#endif

static int zmk_long_press_wake_init(void)
{

#if !DT_NODE_HAS_STATUS(LPW_NODE, okay)
    LOG_DBG("No long-press wake configuration found");
    return 0;
#else
    bool wake_valid[MAX(LPW_NUM_WAKE_GPIOS, 1)] = { false };
    bool still_active[MAX(LPW_NUM_WAKE_GPIOS, 1)] = { false };
    bool initial_active_found = false;

    zmk_long_press_wake_configure_ext_power();

#if LPW_BYPASS_ON_USB
    if (zmk_long_press_wake_usb_present()) {
        LOG_INF("USB power detected, bypassing long-press requirement");
        zmk_long_press_wake_enable_ext_power();
        return 0;
    }
#endif

    if (LPW_NUM_WAKE_GPIOS == 0) {
        LOG_ERR("No wake GPIOs defined; skipping long-press enforcement");
        zmk_long_press_wake_enable_ext_power();
        return 0;
    }

    for (size_t i = 0; i < LPW_NUM_WAKE_GPIOS; i++) {
        if (!gpio_is_ready_dt(&wake_gpios[i])) {
            LOG_ERR("Wake GPIO %s not ready", wake_gpios[i].port->name);
            continue;
        }

        int ret = gpio_pin_configure_dt(&wake_gpios[i], GPIO_INPUT);
        if (ret < 0) {
            LOG_ERR("Failed to configure wake GPIO %s:%u (%d)",
                    wake_gpios[i].port->name, wake_gpios[i].pin, ret);
            continue;
        }

        wake_valid[i] = true;
    }

    bool any_valid = false;
    for (size_t i = 0; i < LPW_NUM_WAKE_GPIOS; i++) {
        if (wake_valid[i]) {
            any_valid = true;
            break;
        }
    }

    if (!any_valid) {
        LOG_ERR("No usable wake GPIOs; skipping enforcement");
        zmk_long_press_wake_enable_ext_power();
        return 0;
    }

    for (size_t i = 0; i < LPW_NUM_WAKE_GPIOS; i++) {
        if (!wake_valid[i]) {
            continue;
        }

        bool active = zmk_long_press_wake_gpio_active(&wake_gpios[i]);
        still_active[i] = active;
        initial_active_found |= active;
    }

    if (!initial_active_found) {
        LOG_INF("No wake inputs active, returning to low-power state");
        zmk_long_press_wake_request_low_power();
        return 0;
    }

    if (LPW_REQUIRED_HOLD_MS == 0) {
        LOG_INF("Required hold time is 0 ms; accepting wake");
        zmk_long_press_wake_enable_ext_power();
        return 0;
    }

    uint32_t elapsed = 0U;

    while (elapsed < LPW_REQUIRED_HOLD_MS) {
        uint32_t remaining = LPW_REQUIRED_HOLD_MS - elapsed;
        uint32_t wait_ms = MIN(remaining, 10U);

        k_msleep(wait_ms);
        elapsed += wait_ms;

        bool any_still_active = false;

        for (size_t i = 0; i < LPW_NUM_WAKE_GPIOS; i++) {
            if (!wake_valid[i] || !still_active[i]) {
                continue;
            }

            bool active = zmk_long_press_wake_gpio_active(&wake_gpios[i]);
            if (!active) {
                still_active[i] = false;
            } else {
                any_still_active = true;
            }
        }

        if (!any_still_active) {
            LOG_INF("Wake input released before hold duration");
            zmk_long_press_wake_request_low_power();
            return 0;
        }
    }

    LOG_INF("Valid long-press detected");
    zmk_long_press_wake_enable_ext_power();

    return 0;
#endif /* DT_NODE_HAS_STATUS(LPW_NODE, okay) */
}

SYS_INIT(zmk_long_press_wake_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
