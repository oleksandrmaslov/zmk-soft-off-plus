/*
 * Copyright (c) 2026 Oleksandr Maslov
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>

#include <zephyr/sys/util.h>

#include <zmk/pm.h>
#include <zmk/soft_off_plus/off_marker.h>

#if IS_ENABLED(CONFIG_ZMK_SOFT_OFF_PLUS_WAKE_DELAY) &&                                      \
    IS_ENABLED(CONFIG_SOC_FAMILY_NORDIC_NRF)
#include <hal/nrf_power.h>
#if NRF_POWER_HAS_GPREGRET &&                                                               \
    (NRF_POWER_HAS_GPREGRET_ARRAY || defined(POWER_GPREGRET2_GPREGRET_Msk))
#define SOP_HAS_OFF_MARKER 1
#else
#define SOP_HAS_OFF_MARKER 0
#endif
#else
#define SOP_HAS_OFF_MARKER 0
#endif

/* GPREGRET[0] is reserved for Zephyr/ZMK's reboot type (including UF2 DFU).
 * nRF52840 provides a second retained byte, which survives System OFF and is
 * not consumed by the Adafruit bootloader. */
#define SOP_OFF_MARKER 0xA5U
#define SOP_OFF_MARKER_REG 1U

void zmk_soft_off_plus_off_marker_set(void) {
#if SOP_HAS_OFF_MARKER
    nrf_power_gpregret_set(NRF_POWER, SOP_OFF_MARKER_REG, SOP_OFF_MARKER);
#endif
}

void zmk_soft_off_plus_off_marker_clear(void) {
#if SOP_HAS_OFF_MARKER
    nrf_power_gpregret_set(NRF_POWER, SOP_OFF_MARKER_REG, 0U);
#endif
}

bool zmk_soft_off_plus_off_marker_consume(void) {
#if SOP_HAS_OFF_MARKER
    bool marked =
        nrf_power_gpregret_get(NRF_POWER, SOP_OFF_MARKER_REG) == SOP_OFF_MARKER;
    zmk_soft_off_plus_off_marker_clear();
    return marked;
#else
    /* Preserve the original cause-only behavior on targets without a second
     * retained register. The module's documented/tested target is nRF52840. */
    return true;
#endif
}

int zmk_soft_off_plus_pm_soft_off(void) {
    if (IS_ENABLED(CONFIG_ZMK_SOFT_OFF_PLUS_WAKE_DELAY)) {
        zmk_soft_off_plus_off_marker_set();
    }

    int err = zmk_pm_soft_off();

    /* A successful System OFF never returns. Do not leave a stale marker when
     * device suspension fails and ZMK returns an error. */
    zmk_soft_off_plus_off_marker_clear();
    return err;
}
