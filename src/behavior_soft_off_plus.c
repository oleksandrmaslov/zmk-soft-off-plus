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
#include <zephyr/sys/atomic.h>

#include <zmk/behavior.h>
#include <zmk/pm.h>
#include <zmk/soft_off_plus/split_sync.h>

LOG_MODULE_DECLARE(zmk_soft_off_plus, CONFIG_ZMK_SOFT_OFF_PLUS_LOG_LEVEL);

struct behavior_soft_off_plus_config {
    uint32_t hold_time_ms;
    bool trigger_on_hold;
};

enum soft_off_plus_hold_phase {
    SOP_HOLD_IDLE,
    SOP_HOLD_ARMED,
    SOP_HOLD_DROPPING,
    SOP_HOLD_DROPPED,
    SOP_HOLD_RELEASED_DURING_DROP,
};

struct behavior_soft_off_plus_data {
    uint32_t press_start;
    struct k_work_delayable hold_work;
    atomic_t hold_phase;
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

    zmk_soft_off_plus_cut_power_rail();
    int err = zmk_pm_soft_off();
    LOG_ERR("soft-off-plus: System OFF returned unexpectedly (%d)", err);
    zmk_soft_off_plus_recover_from_failed_off();
    zmk_soft_off_plus_release_off_claim();
}

/* trigger-on-hold, phase 1: once hold-time-ms elapses while the key is still
 * held, request display blanking for visual confirmation -- but DO NOT power
 * off yet. The real System OFF happens on release (phase 2), so the wake
 * key's GPIO is no longer active when we enter System OFF (otherwise the nRF
 * re-wakes instantly: "System OFF while DETECT is high causes a wakeup reset").
 *
 * This half requests a local blank (no power off -- our wake key is still held)
 * and sends the other half a DROP. What the other half does with the
 * DROP depends on whether *it* has a key held (see sop_drop_work_cb):
 *
 *  - A half holding its own soft-off-plus key (either half on a matrix press, or
 *    the sideband half the key is wired to) waits for its own
 *    release. That keeps a matrix central alive to relay our key-release -- an
 *    immediate off there would strand the peripheral "screen off but never in
 *    System OFF," unable to wake from its own key.
 *  - A half with nothing held (the non-wired half of a sideband press) just
 *    powers off now: no held wake key to re-wake it, and holding past hold-time
 *    already committed to off, so it needn't wait for a relayed release.
 *
 * Our own real System OFF waits for release (phase 2 -> soft_off_plus_trigger(),
 * which also re-sends OFF as a redundant backup).
 */
static void soft_off_plus_hold_work_cb(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct behavior_soft_off_plus_data *data =
        CONTAINER_OF(dwork, struct behavior_soft_off_plus_data, hold_work);

    /* A release can race the delayed work exactly as phase 1 starts.
     * Claim phase 1 before touching any devices so release either cancels an
     * armed hold or records that System OFF must run after this work finishes. */
    if (!atomic_cas(&data->hold_phase, SOP_HOLD_ARMED, SOP_HOLD_DROPPING)) {
        return;
    }

    LOG_INF("soft-off-plus: hold reached; release to power off");

    /* Tell the other half that the hold committed (no-op on a non-split build),
     * then request safe local display blanking. */
    zmk_soft_off_plus_signal_peers_drop();
    zmk_soft_off_plus_drop_components();

    if (atomic_cas(&data->hold_phase, SOP_HOLD_DROPPING, SOP_HOLD_DROPPED)) {
        return; /* still held; normal release will complete System OFF */
    }

    if (atomic_cas(&data->hold_phase, SOP_HOLD_RELEASED_DURING_DROP, SOP_HOLD_IDLE)) {
        LOG_INF("soft-off-plus: key released during phase 1; entering System OFF");
        soft_off_plus_trigger();
    }
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
        if (!atomic_cas(&data->hold_phase, SOP_HOLD_IDLE, SOP_HOLD_ARMED)) {
            LOG_WRN("soft-off-plus: ignored overlapping hold press");
            return ZMK_BEHAVIOR_OPAQUE;
        }
        /* Mark our wake key as held so a cross-half DROP keeps this half alive
         * until our own release, instead of powering us off while the key --
         * our wake source -- is still down. */
        zmk_soft_off_plus_hold_begin();
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
        /* Our wake key is up now; clear the held marker before we (maybe) power
         * off. Must run before soft_off_plus_trigger(), which does not return. */
        zmk_soft_off_plus_hold_end();
        if (atomic_cas(&data->hold_phase, SOP_HOLD_ARMED, SOP_HOLD_IDLE)) {
            /* Released before hold-time: hold_work was cancelled or will see
             * IDLE and exit without dropping anything. */
            return ZMK_BEHAVIOR_OPAQUE;
        }

        if (atomic_cas(&data->hold_phase, SOP_HOLD_DROPPING,
                       SOP_HOLD_RELEASED_DURING_DROP)) {
            /* Phase 1 is executing on another context. It will enter System
             * OFF after the component drop completes, with the wake key up. */
            return ZMK_BEHAVIOR_OPAQUE;
        }

        if (atomic_cas(&data->hold_phase, SOP_HOLD_DROPPED, SOP_HOLD_IDLE)) {
            /* Phase 2: the hold already committed in phase 1.
             * The key is now released (GPIO inactive), so it is safe to enter
             * real System OFF -- DETECT is low, so the board won't re-wake. Same
             * claim + cross-half OFF + power-off path as the trigger-on-release
             * case below. (Phase 1 only ever sent a DROP, never an OFF, so the
             * central stayed alive to relay this release to the peripheral.) */
            LOG_INF("soft-off-plus: key released; entering System OFF");
            soft_off_plus_trigger();
        }
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
    atomic_set(&data->hold_phase, SOP_HOLD_IDLE);
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
