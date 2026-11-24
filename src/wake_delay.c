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

#include <zmk/soft_off_plus/off_marker.h>

#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/usb.h>
#elif IS_ENABLED(CONFIG_SOC_FAMILY_NORDIC_NRF)
#include <hal/nrf_power.h>
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

/* External-power rail control, if the board exposes a zmk,ext-power node. We
 * force it inactive for the duration of the boot-time hold check so an
 * unconfirmed wake never lights the display or other external peripherals.
 * The off level is retained when we drop back to System OFF; on a confirmed
 * wake ZMK's ext-power driver re-enables the rail at its normal init priority. */
#define EXT_POWER_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(zmk_ext_power_generic)
#if DT_NODE_HAS_STATUS(EXT_POWER_NODE, okay)
static const struct gpio_dt_spec ext_power_gpios[] = {
    DT_FOREACH_PROP_ELEM(EXT_POWER_NODE, control_gpios, SOP_GPIO_ELEM)};

static void wake_force_ext_power_off(void) {
    for (size_t i = 0; i < ARRAY_SIZE(ext_power_gpios); i++) {
        if (!gpio_is_ready_dt(&ext_power_gpios[i])) {
            continue;
        }
        int ret = gpio_pin_configure_dt(&ext_power_gpios[i], GPIO_OUTPUT_INACTIVE);
        if (ret < 0) {
            LOG_WRN("soft-off-plus: ext-power off failed (%d)", ret);
        }
    }
}
#else
static void wake_force_ext_power_off(void) {}
#endif

static bool wake_usb_bypass(void) {
#if !DT_NODE_HAS_PROP(WAKE_NODE, bypass_on_usb)
    return false;
#elif IS_ENABLED(CONFIG_ZMK_USB)
    return zmk_usb_is_powered();
#elif IS_ENABLED(CONFIG_SOC_FAMILY_NORDIC_NRF) && NRF_POWER_HAS_USBREG
    /* ZMK_USB is central-only on a split. Read VBUS directly so a Nordic
     * peripheral can still honor bypass-on-usb after a System OFF wake. */
    return nrf_power_usbregstatus_vbusdet_get(NRF_POWER);
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

    /* Keep classifying subsequent wake attempts as manual soft-off wakes until
     * the configured hold is actually confirmed. */
    zmk_soft_off_plus_off_marker_set();
    sys_poweroff();
    CODE_UNREACHABLE;
}

static int wake_delay_init(void) {
    /* Consume this on every reset so a failed/aborted System OFF cannot poison
     * a later pin reset or DFU reboot. Rejected wake attempts set it again just
     * before returning to System OFF. */
    bool manual_soft_off = zmk_soft_off_plus_off_marker_consume();
    uint32_t cause = 0;
    int rc = hwinfo_get_reset_cause(&cause);
    if (rc != 0) {
        LOG_WRN("soft-off-plus: cannot read reset cause (%d); continuing boot", rc);
        return 0;
    }
    /* Clear so a later non-wake reset isn't mistaken for a soft-off wake. */
    hwinfo_clear_reset_cause();
    LOG_DBG("soft-off-plus: reset cause 0x%08x", cause);

    if (!(cause & RESET_LOW_POWER_WAKE)) {
        LOG_DBG("soft-off-plus: not a soft-off wake (cause 0x%08x); continuing", cause);
        return 0;
    }

    if (!manual_soft_off) {
        LOG_DBG("soft-off-plus: inactivity deep-sleep wake; continuing boot");
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
        wake_force_ext_power_off();
        wake_return_to_off();
    }

    /* USB bypass means that an actually held wake key need not remain down for
     * the full wake-hold-ms. It must not bypass the key check itself: otherwise
     * any spurious low-power reset on a USB-powered passive split half becomes
     * a complete reboot immediately after the peer asks it to power off. */
    if (wake_usb_bypass()) {
        k_msleep(20); /* reject a release edge or switch bounce around reset */
        if (!wake_any_active()) {
            LOG_INF("soft-off-plus: USB wake key released during debounce");
            wake_force_ext_power_off();
            wake_return_to_off();
        }
        LOG_INF("soft-off-plus: USB present and wake key active; bypassing wake hold");
        return 0;
    }

    /* Committed to a hold check on battery: keep external peripherals (e.g. the
     * display) dark until the wake is confirmed, so a too-short press cannot
     * power them. */
    wake_force_ext_power_off();

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
