/*
 * SPDX-License-Identifier: MIT
 *
 * Shared, always-compiled translation unit for soft-off-plus: it owns the
 * LOG_MODULE_REGISTER that the other files LOG_MODULE_DECLARE, plus the
 * one-shot power-off claim shared between the behavior and the cross-half
 * off-signal, so any subset of features can be enabled.
 */

#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <zmk/soft_off_plus/split_sync.h>

LOG_MODULE_REGISTER(zmk_soft_off_plus, CONFIG_ZMK_SOFT_OFF_PLUS_LOG_LEVEL);

/* One-shot guard so each half powers off only once, even when both the
 * keymap-relayed behavior and a cross-half off-signal fire on it. Implicitly
 * reset on the next boot (RAM is lost in System OFF). */
static atomic_t sop_off_claimed;

bool zmk_soft_off_plus_claim_off(void) { return atomic_cas(&sop_off_claimed, 0, 1); }
