/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

/* Opcode exchanged between halves to request a synchronized soft off. */
#define ZMK_SOFT_OFF_PLUS_CMD_OFF 0x01

/* Signal the other half/halves to power off together with this one.
 *
 * On a split central this writes an off command to every connected peripheral;
 * on a split peripheral it notifies the central. When split sync is disabled
 * (or this is not a split build) it is a no-op that returns 0.
 *
 * Returns 0 on success or a negative errno on failure.
 */
int zmk_soft_off_plus_signal_peers(void);
