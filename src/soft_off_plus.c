/*
 * SPDX-License-Identifier: MIT
 *
 * Shared, always-compiled translation unit for soft-off-plus: it owns the
 * LOG_MODULE_REGISTER that the other files LOG_MODULE_DECLARE, plus the
 * one-shot power-off claim shared between the behavior and the cross-half
 * off-signal, so any subset of features can be enabled.
 */

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <zmk/soft_off_plus/split_sync.h>

#if IS_ENABLED(CONFIG_ZMK_DISPLAY) && DT_HAS_CHOSEN(zephyr_display)
#include <zephyr/drivers/display.h>
#endif

LOG_MODULE_REGISTER(zmk_soft_off_plus, CONFIG_ZMK_SOFT_OFF_PLUS_LOG_LEVEL);

/* One-shot guard so each half powers off only once, even when both the
 * keymap-relayed behavior and a cross-half off-signal fire on it. Implicitly
 * reset on the next boot (RAM is lost in System OFF). */
static atomic_t sop_off_claimed;

bool zmk_soft_off_plus_claim_off(void) { return atomic_cas(&sop_off_claimed, 0, 1); }

void zmk_soft_off_plus_release_off_claim(void) { atomic_clear(&sop_off_claimed); }

/* Number of soft-off-plus keys currently held on THIS half via its own behavior.
 * Used to decide whether an incoming cross-half DROP may power this half off
 * (only when nothing is held here -- otherwise the held wake key would re-wake
 * us). begin/end are balanced by the behavior's press/release. */
static atomic_t sop_hold_count;

void zmk_soft_off_plus_hold_begin(void) { atomic_inc(&sop_hold_count); }

void zmk_soft_off_plus_hold_end(void) {
    /* guard against underflow if begin/end ever get unbalanced */
    if (atomic_get(&sop_hold_count) > 0) {
        atomic_dec(&sop_hold_count);
    }
}

bool zmk_soft_off_plus_hold_active(void) { return atomic_get(&sop_hold_count) > 0; }

static void sop_blank_display(void) {
#if IS_ENABLED(CONFIG_ZMK_DISPLAY) && DT_HAS_CHOSEN(zephyr_display)
    const struct device *disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    if (device_is_ready(disp)) {
        /* The same call ZMK uses for blank-on-idle: a clean panel-level blank for
         * displays that support it (an OLED's display-off, or an LS0xx with
         * disp-en-gpios). It safely returns -ENOTSUP on a bare nice!view. */
        display_blanking_on(disp);
    }
#endif
}

void zmk_soft_off_plus_drop_components(void) {
    /* Phase 1 is only a visual confirmation. Do not suspend the device graph or
     * cut an external display rail while input/BLE remain alive: EXT_POWER PM
     * suspend can persist OFF into settings, and an LS0xx VCOM thread would keep
     * driving an unpowered panel. Displays with a real blanking control blank
     * here; a bare nice!view stays powered until final System OFF on release. */
    sop_blank_display();
}
