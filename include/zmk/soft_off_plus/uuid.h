/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/bluetooth/uuid.h>

/* Custom 128-bit UUIDs for the soft-off-plus cross-half sync service.
 * Distinct from ZMK's own split service UUIDs. */
#define ZMK_SOFT_OFF_PLUS_SVC_UUID                                                                  \
    BT_UUID_128_ENCODE(0x6f2a1b00, 0x5c3d, 0x4e8a, 0x9f10, 0x2b7c4d6e8a01)

#define ZMK_SOFT_OFF_PLUS_CHRC_UUID                                                                 \
    BT_UUID_128_ENCODE(0x6f2a1b01, 0x5c3d, 0x4e8a, 0x9f10, 0x2b7c4d6e8a01)
