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

#if IS_ENABLED(CONFIG_ZMK_DISPLAY) && DT_HAS_CHOSEN(zephyr_display)
#include <zephyr/drivers/display.h>
#endif

LOG_MODULE_DECLARE(zmk_soft_off_plus, CONFIG_ZMK_SOFT_OFF_PLUS_LOG_LEVEL);

struct behavior_soft_off_plus_config {
    uint32_t hold_time_ms;
    bool trigger_on_hold;
};

struct behavior_soft_off_plus_data {
    uint32_t press_start;
    struct k_work_delayable hold_work;
    bool dropped; /* trigger-on-hold phase 1 done: components suspended, waiting for release */
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

static void soft_off_plus_blank_display(void) {
#if IS_ENABLED(CONFIG_ZMK_DISPLAY) && DT_HAS_CHOSEN(zephyr_display)
    const struct device *disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    if (device_is_ready(disp)) {
        /* Same call ZMK uses for blank-on-idle: a clean panel-level blank for
         * displays that support it (an OLED's display-off, or an LS0xx with
         * disp-en-gpios). On a bare nice_view it's a no-op -- but that's fine:
         * suspending ext-power below cuts the panel's VCC, and a Sharp memory
         * LCD holds its image only *while powered*, so it blanks anyway. */
        display_blanking_on(disp);
    }
#endif
}

/* trigger-on-hold, phase 1: once hold-time-ms elapses while the key is still
 * held, drop the keyboard's components for visual confirmation -- but DO NOT
 * power off yet. The real System OFF happens on release (phase 2), so the wake
 * key's GPIO is no longer active when we enter System OFF (otherwise the nRF
 * re-wakes instantly: "System OFF while DETECT is high causes a wakeup reset").
 *
 * Phase 1 is *visual confirmation only*. It must NOT signal the other half to
 * power off and must NOT power off itself:
 *
 *  - On the matrix path the GLOBAL keymap relay already runs this same phase 1
 *    on the other half, so both screens blank without any cross-half signal.
 *  - If phase 1 *did* signal, the peripheral's notify would make the central run
 *    a full zmk_pm_soft_off() mid-hold. A central that has powered off can no
 *    longer relay the key-release that drives the peripheral's phase 2, leaving
 *    the peripheral stuck "screen off but never in System OFF" -- and therefore
 *    unable to wake from its own key, because its wake source was never armed.
 *
 * So both the real System OFF and the cross-half off-signal wait for release
 * (phase 2 -> soft_off_plus_trigger()).
 */
static void soft_off_plus_hold_work_cb(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct behavior_soft_off_plus_data *data =
        CONTAINER_OF(dwork, struct behavior_soft_off_plus_data, hold_work);

    LOG_INF("soft-off-plus: hold reached; dropping components (release to power off)");

    /* Blank the screen first (while it still has power), the same call ZMK's
     * blank-on-idle uses. */
    soft_off_plus_blank_display();

    /* Universal "looks off": run every device's PM suspend (ext-power -> display
     * loses power, radio down, RGB off, any future device). Wakeup-enabled
     * devices such as the kscan are skipped, so the key release still arrives
     * through the normal behavior path, and the BLE link stays up so the central
     * can still relay our release. */
    zmk_pm_suspend_devices();
    data->dropped = true;
}

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct behavior_soft_off_plus_data *data = dev->data;
    const struct behavior_soft_off_plus_config *config = dev->config;

    if (config->trigger_on_hold) {
        /* Phase 1 is armed for hold-time-ms from now; releasing earlier cancels
         * it (nothing dropped). hold-time-ms == 0 drops essentially on press. */
        data->dropped = false;
        k_work_schedule(&data->hold_work, K_MSEC(config->hold_time_ms));
    } else {
        data->press_start = k_uptime_get();
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct behavior_soft_off_plus_data *data = dev->data;
    const struct behavior_soft_off_plus_config *config = dev->config;

    if (config->trigger_on_hold) {
        k_work_cancel_delayable(&data->hold_work);
        if (data->dropped) {
            /* Phase 2: components were already dropped at the hold (phase 1).
             * The key is now released (GPIO inactive), so it is safe to enter
             * real System OFF -- DETECT is low, so the board won't re-wake. This
             * is the same claim + cross-half signal + power-off path as the
             * trigger-on-release case below; signalling here (not in phase 1)
             * keeps the central alive long enough to relay this release to the
             * peripheral. */
            data->dropped = false;
            LOG_INF("soft-off-plus: key released; entering System OFF");
            soft_off_plus_trigger();
        }
        /* Released before hold-time: hold_work never fired, nothing dropped. */
        return ZMK_BEHAVIOR_OPAQUE;
    }

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

static int behavior_soft_off_plus_init(const struct device *dev) {
    struct behavior_soft_off_plus_data *data = dev->data;
    k_work_init_delayable(&data->hold_work, soft_off_plus_hold_work_cb);
    return 0;
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
        .trigger_on_hold = DT_INST_PROP(n, trigger_on_hold),                                       \
    };                                                                                             \
    static struct behavior_soft_off_plus_data bsop_data_##n = {};                                  \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_soft_off_plus_init, NULL, &bsop_data_##n, &bsop_config_##n,\
                            POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                       \
                            &behavior_soft_off_plus_driver_api);

DT_INST_FOREACH_STATUS_OKAY(SOP_INST)
