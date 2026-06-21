/*
 * SPDX-License-Identifier: MIT
 *
 * Shared log module registration for soft-off-plus. Kept in its own
 * always-compiled translation unit so any subset of features can be enabled
 * without losing the LOG_MODULE_REGISTER the other files LOG_MODULE_DECLARE.
 */

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(zmk_soft_off_plus, CONFIG_ZMK_SOFT_OFF_PLUS_LOG_LEVEL);
