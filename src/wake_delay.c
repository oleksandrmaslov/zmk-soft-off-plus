/*
 * SPDX-License-Identifier: MIT
 *
 * Boot-time "hold to wake" delay for ZMK soft off.
 *
 * On nRF, waking from soft off (System OFF) is a reset, so this runs on every
 * boot. If the reset was a System OFF wakeup, we poll the wake GPIO(s) and only
 * continue booting when they are held for wake-hold-ms. Otherwise we re-arm the
 * wake source and go straight back to System OFF, before the radio or display
 * are powered, so an accidental tap costs almost no energy.
 */

#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/init.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>
#include <zephyr/logging/log.h>

#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/usb.h>
#endif

LOG_MODULE_DECLARE(zmk_soft_off_plus, CONFIG_ZMK_SOFT_OFF_PLUS_LOG_LEVEL);

#define WAKE_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(zmk_soft_off_plus_wake)

#if DT_NODE_HAS_STATUS(WAKE_NODE, okay)

#define WAKE_HOLD_MS DT_PROP_OR(WAKE_NODE, wake_hold_ms, 1000)

#define SOP_GPIO_ELEM(node_id, prop, idx) GPIO_DT_SPEC_GET_BY_IDX(node_id, prop, idx),

static const struct gpio_dt_spec wake_gpios[] = {
    DT_FOREACH_PROP_ELEM(WAKE_NODE, wake_gpios, SOP_GPIO_ELEM)};

#define NUM_WAKE_GPIOS ARRAY_SIZE(wake_gpios)

#if DT_NODE_HAS_PROP(WAKE_NODE, strobe_gpios)
static const struct gpio_dt_spec strobe_gpios[] = {
    DT_FOREACH_PROP_ELEM(WAKE_NODE, strobe_gpios, SOP_GPIO_ELEM)};
#define NUM_STROBE_GPIOS ARRAY_SIZE(strobe_gpios)
#else
#define NUM_STROBE_GPIOS 0
#endif

static bool wake_usb_bypass(void) {
#if DT_NODE_HAS_PROP(WAKE_NODE, bypass_on_usb) && IS_ENABLED(CONFIG_ZMK_USB)
    return zmk_usb_is_powered();
#else
    return false;
#endif
}

static void wake_drive_strobes(void) {
#if DT_NODE_HAS_PROP(WAKE_NODE, strobe_gpios)
    for (size_t i = 0; i < NUM_STROBE_GPIOS; i++) {
        if (!gpio_is_ready_dt(&strobe_gpios[i])) {
            continue;
        }
        int ret = gpio_pin_configure_dt(&strobe_gpios[i], GPIO_OUTPUT_ACTIVE);
        if (ret < 0) {
            LOG_WRN("soft-off-plus: strobe GPIO config failed (%d)", ret);
        }
    }
#endif
}

static bool wake_any_active(void) {
    for (size_t i = 0; i < NUM_WAKE_GPIOS; i++) {
        if (gpio_pin_get_dt(&wake_gpios[i]) > 0) {
            return true;
        }
    }
    return false;
}

static FUNC_NORETURN void wake_return_to_off(void) {
    LOG_INF("soft-off-plus: wake not confirmed, returning to System OFF");

    /* Re-arm the wake input(s) so a future press wakes the board again. Strobe
     * outputs were already driven active and their state is retained in System
     * OFF, which is required for matrix wake keys. */
    for (size_t i = 0; i < NUM_WAKE_GPIOS; i++) {
        gpio_pin_configure_dt(&wake_gpios[i], GPIO_INPUT);
        gpio_pin_interrupt_configure_dt(&wake_gpios[i], GPIO_INT_LEVEL_ACTIVE);
    }

    sys_poweroff();
    CODE_UNREACHABLE;
}

static int wake_delay_init(void) {
    uint32_t cause = 0;
    int rc = hwinfo_get_reset_cause(&cause);
    if (rc != 0) {
        LOG_WRN("soft-off-plus: cannot read reset cause (%d); continuing boot", rc);
        return 0;
    }
    /* Clear so a later non-wake reset isn't mistaken for a soft-off wake. */
    hwinfo_clear_reset_cause();

    if (!(cause & RESET_LOW_POWER_WAKE)) {
        LOG_DBG("soft-off-plus: not a soft-off wake (cause 0x%08x); continuing", cause);
        return 0;
    }

    if (wake_usb_bypass()) {
        LOG_INF("soft-off-plus: USB present; bypassing wake hold");
        return 0;
    }

    for (size_t i = 0; i < NUM_WAKE_GPIOS; i++) {
        if (!gpio_is_ready_dt(&wake_gpios[i])) {
            LOG_ERR("soft-off-plus: wake GPIO not ready; continuing boot");
            return 0; /* fail open: turn on rather than get stuck off */
        }
        gpio_pin_configure_dt(&wake_gpios[i], GPIO_INPUT);
    }

    wake_drive_strobes();
    k_busy_wait(100); /* let strobe/input levels settle */

    if (!wake_any_active()) {
        LOG_INF("soft-off-plus: wake key not held at boot");
        wake_return_to_off();
    }

    if (WAKE_HOLD_MS == 0) {
        return 0;
    }

    uint32_t elapsed = 0;
    while (elapsed < WAKE_HOLD_MS) {
        uint32_t step = MIN((uint32_t)WAKE_HOLD_MS - elapsed, 10U);
        k_msleep(step);
        elapsed += step;
        if (!wake_any_active()) {
            LOG_INF("soft-off-plus: wake key released after %u ms", elapsed);
            wake_return_to_off();
        }
    }

    LOG_INF("soft-off-plus: wake confirmed after %u ms", (uint32_t)WAKE_HOLD_MS);
    return 0;
}

SYS_INIT(wake_delay_init, POST_KERNEL, CONFIG_ZMK_SOFT_OFF_PLUS_WAKE_DELAY_INIT_PRIORITY);

#else /* !DT_NODE_HAS_STATUS(WAKE_NODE, okay) */

static int wake_delay_init(void) {
    LOG_WRN("soft-off-plus: wake delay enabled but no zmk,soft-off-plus-wake node");
    return 0;
}
SYS_INIT(wake_delay_init, POST_KERNEL, CONFIG_ZMK_SOFT_OFF_PLUS_WAKE_DELAY_INIT_PRIORITY);

#endif
