/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>

/* Mark a System OFF entered by soft-off-plus, as opposed to ZMK's inactivity
 * deep sleep. The boot-time hold check consumes this marker. */
void zmk_soft_off_plus_off_marker_set(void);
void zmk_soft_off_plus_off_marker_clear(void);
bool zmk_soft_off_plus_off_marker_consume(void);

/* Mark, enter ZMK soft off, and clear the marker if the call fails/returns. */
int zmk_soft_off_plus_pm_soft_off(void);
