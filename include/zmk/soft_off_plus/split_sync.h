/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Opcodes exchanged between halves.
 *  OFF  - power off now (claim + zmk_pm_soft_off).
 *  DROP - drop components for visual confirmation only (suspend devices / blank),
 *         WITHOUT powering off. Used by trigger-on-hold phase 1 so a sideband
 *         press blanks both halves while held; safe for a matrix central because
 *         it does not power off, so the central can still relay the key-release. */
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

/* Signal the other half/halves to DROP components for visual confirmation
 * (CMD_DROP): suspend devices / blank the display WITHOUT powering off. Same
 * transport as zmk_soft_off_plus_signal_peers(); no-op returning 0 when split
 * sync is disabled or this is not a split build. */
int zmk_soft_off_plus_signal_peers_drop(void);

/* The universal "looks off" used by trigger-on-hold phase 1 and by an incoming
 * cross-half DROP: blank the display and run every device's PM suspend
 * (ext-power -> display VCC cut, radio, RGB, ...) WITHOUT powering off. Wakeup
 * devices (kscan) and the BLE link stay up. Always available. */
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
