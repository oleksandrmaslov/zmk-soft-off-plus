/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Opcodes exchanged between halves.
 *  OFF  - power off now (claim + marked ZMK soft off).
 *  DROP - request phase-1 visual confirmation WITHOUT powering off the half
 *         whose own wake key is held. Used by trigger-on-hold so that half can
 *         still observe release; a passive peer enters System OFF immediately. */
#define ZMK_SOFT_OFF_PLUS_CMD_OFF 0x01
#define ZMK_SOFT_OFF_PLUS_CMD_DROP 0x02

/* Signal the other half/halves to power off together with this one (CMD_OFF).
 *
 * On a split central this writes the command to every connected peripheral; on a
 * split peripheral it notifies the central. When split sync is disabled (or this
 * is not a split build) it is a no-op that returns 0.
 *
 * Returns 0 on success or a negative errno on failure.
 */
int zmk_soft_off_plus_signal_peers(void);

/* Signal the other half/halves to perform trigger-on-hold phase 1 (CMD_DROP).
 * Same transport as zmk_soft_off_plus_signal_peers(); no-op returning 0 when
 * split sync is disabled or this is not a split build. */
int zmk_soft_off_plus_signal_peers_drop(void);

/* Visual confirmation used by trigger-on-hold phase 1 and an incoming DROP.
 * It only requests panel blanking; device PM and external rails stay intact
 * until final System OFF so input/release and display drivers remain valid. */
void zmk_soft_off_plus_drop_components(void);

/* Track whether THIS half currently has a soft-off-plus key held via its own
 * behavior (matrix relay, or the sideband half the key is wired to). Such a half
 * must power off on its own key-release -- the key is also the wake source, so
 * powering off while it is held would re-wake the board. A half with no key held
 * (hold not active) is a passive receiver and can power off as soon as a peer
 * tells it to. begin/end are called by the behavior around a trigger-on-hold
 * press; the cross-half DROP handler consults hold_active(). */
void zmk_soft_off_plus_hold_begin(void);
void zmk_soft_off_plus_hold_end(void);
bool zmk_soft_off_plus_hold_active(void);

/* Claim the one-shot power-off for this half. Returns true for the first caller
 * (which should proceed to power off) and false for any later caller, so the
 * keymap-relayed behavior and the cross-half off-signal never power a half off
 * twice. */
bool zmk_soft_off_plus_claim_off(void);

/* Release a claimed attempt if the marked soft-off call unexpectedly returns, allowing
 * a later gesture or peer command to retry instead of wedging until reset. */
void zmk_soft_off_plus_release_off_claim(void);

/* ZMK soft off normally never returns. If device suspension fails, restore
 * the device graph, wake flags, external-power state, and phase-1 display so the
 * keyboard remains usable and EXT_POWER=off is not persisted in settings. */
void zmk_soft_off_plus_recover_from_failed_off(void);
