/*
 * SPDX-License-Identifier: MIT
 *
 * Shared, always-compiled translation unit for soft-off-plus: it owns the
 * LOG_MODULE_REGISTER that the other files LOG_MODULE_DECLARE, plus the
 * one-shot power-off claim shared between the behavior and the cross-half
 * off-signal, so any subset of features can be enabled.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/atomic.h>

#include <zmk/soft_off_plus/split_sync.h>

#define SOP_EXT_POWER_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(zmk_ext_power_generic)
#if DT_NODE_HAS_STATUS(SOP_EXT_POWER_NODE, okay)
#include <drivers/ext_power.h>
#include <zephyr/drivers/gpio.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_DISPLAY) && IS_ENABLED(CONFIG_LVGL) &&                                \
    DT_HAS_CHOSEN(zephyr_display)
#include <zephyr/drivers/display.h>
#include <lvgl.h>
#include <zmk/display.h>
#endif

LOG_MODULE_REGISTER(zmk_soft_off_plus, CONFIG_ZMK_SOFT_OFF_PLUS_LOG_LEVEL);

/* One-shot guard so each half powers off only once, even when both the
 * keymap-relayed behavior and a cross-half off-signal fire on it. Implicitly
 * reset on the next boot (RAM is lost in System OFF). */
static atomic_t sop_off_claimed;

bool zmk_soft_off_plus_claim_off(void) { return atomic_cas(&sop_off_claimed, 0, 1); }

void zmk_soft_off_plus_release_off_claim(void) { atomic_clear(&sop_off_claimed); }

#if DT_NODE_HAS_STATUS(SOP_EXT_POWER_NODE, okay) &&                                             \
    DT_NODE_HAS_PROP(SOP_EXT_POWER_NODE, control_gpios)
static const struct gpio_dt_spec sop_rail_gpio =
    GPIO_DT_SPEC_GET_BY_IDX(SOP_EXT_POWER_NODE, control_gpios, 0);
#endif

void zmk_soft_off_plus_cut_power_rail(void) {
#if DT_NODE_HAS_STATUS(SOP_EXT_POWER_NODE, okay) &&                                             \
    DT_NODE_HAS_PROP(SOP_EXT_POWER_NODE, control_gpios)
    /* Raw GPIO write instead of ext_power_disable(): the driver persists OFF
     * to settings, which would leave the rail dead on the next boot. GPIO
     * output state is retained in System OFF, so the rail stays cut until the
     * next boot reinitializes the ext_power driver. This keeps the memory LCD
     * from sitting powered with VCOM stopped, which drifts the panel to black
     * instead of the clean unpowered (white) state. */
    if (gpio_is_ready_dt(&sop_rail_gpio)) {
        gpio_pin_set_dt(&sop_rail_gpio, 0);
    }
#endif
}

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

/* A bare nice!view has no DISP pin, so the LS0xx driver's blanking API returns
 * -ENOTSUP. Keep the panel powered (and therefore keep VCOM valid), then cover
 * the active LVGL screen with an opaque white object and force one refresh.
 * Running this on ZMK's display queue avoids racing LVGL's regular updates. */
#if IS_ENABLED(CONFIG_ZMK_DISPLAY) && IS_ENABLED(CONFIG_LVGL) &&                                \
    DT_HAS_CHOSEN(zephyr_display)
static lv_obj_t *sop_blank_overlay;

static void sop_restore_display_work_cb(struct k_work *work) {
    ARG_UNUSED(work);

    const struct device *disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    if (device_is_ready(disp)) {
        display_blanking_off(disp);
    }

    if (sop_blank_overlay == NULL) {
        return;
    }

    lv_obj_add_flag(sop_blank_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);
}
K_WORK_DEFINE(sop_restore_display_work, sop_restore_display_work_cb);

static void sop_blank_display_work_cb(struct k_work *work) {
    ARG_UNUSED(work);

    const struct device *disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    if (!device_is_ready(disp)) {
        LOG_WRN("soft-off-plus: display is not ready for phase-1 blank");
        return;
    }

    int err = display_blanking_on(disp);
    if (err == 0) {
        return;
    }
    if (err != -ENOTSUP) {
        LOG_WRN("soft-off-plus: display blanking failed (%d); using LVGL fallback", err);
    }

    if (!zmk_display_is_initialized()) {
        LOG_WRN("soft-off-plus: display UI is not initialized for phase-1 blank");
        return;
    }

    lv_obj_t *screen = lv_scr_act();
    if (screen == NULL) {
        LOG_WRN("soft-off-plus: no active LVGL screen for phase-1 blank");
        return;
    }

    if (sop_blank_overlay == NULL) {
        sop_blank_overlay = lv_obj_create(screen);
        lv_obj_remove_style_all(sop_blank_overlay);
        lv_obj_set_pos(sop_blank_overlay, 0, 0);
        lv_obj_set_size(sop_blank_overlay, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(sop_blank_overlay, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(sop_blank_overlay, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_clear_flag(sop_blank_overlay, LV_OBJ_FLAG_SCROLLABLE);
    } else {
        lv_obj_clear_flag(sop_blank_overlay, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_move_foreground(sop_blank_overlay);
    lv_obj_invalidate(sop_blank_overlay);
    lv_refr_now(NULL);
}
K_WORK_DEFINE(sop_blank_display_work, sop_blank_display_work_cb);
#endif

static void sop_blank_display(void) {
#if IS_ENABLED(CONFIG_ZMK_DISPLAY) && IS_ENABLED(CONFIG_LVGL) &&                                \
    DT_HAS_CHOSEN(zephyr_display)
    int err = k_work_submit_to_queue(zmk_display_work_q(), &sop_blank_display_work);
    if (err < 0) {
        LOG_WRN("soft-off-plus: phase-1 display work submit failed (%d)", err);
    }
#endif
}

static void sop_restore_display(void) {
#if IS_ENABLED(CONFIG_ZMK_DISPLAY) && IS_ENABLED(CONFIG_LVGL) &&                                \
    DT_HAS_CHOSEN(zephyr_display)
    int err = k_work_submit_to_queue(zmk_display_work_q(), &sop_restore_display_work);
    if (err < 0) {
        LOG_WRN("soft-off-plus: display restore work submit failed (%d)", err);
    }
#endif
}

void zmk_soft_off_plus_drop_components(void) {
    /* Phase 1 is only a visual confirmation. Do not suspend the device graph or
     * cut an external display rail while input/BLE remain alive: EXT_POWER PM
     * suspend can persist OFF into settings, and an LS0xx VCOM thread would keep
     * driving an unpowered panel. */
    sop_blank_display();
}

void zmk_soft_off_plus_recover_from_failed_off(void) {
#if IS_ENABLED(CONFIG_PM_DEVICE)
    const struct device *devs;
    size_t devc = z_device_get_all_static(&devs);

    /* zmk_pm_soft_off() disables existing wake flags and suspends all devices
     * before its final checked suspend pass. If that pass fails, its built-in
     * rollback only knows about devices from the checked pass. Resume the whole
     * graph in dependency order and restore all capable wake sources. */
    for (const struct device *dev = devs; dev < devs + devc; dev++) {
        if (!device_is_ready(dev)) {
            continue;
        }

        if (pm_device_wakeup_is_capable(dev)) {
            pm_device_wakeup_enable(dev, true);
        }

        int err = pm_device_action_run(dev, PM_DEVICE_ACTION_RESUME);
        if (err < 0 && err != -EALREADY && err != -ENOSYS && err != -ENOTSUP) {
            LOG_WRN("soft-off-plus: failed to resume %s after aborted off (%d)", dev->name, err);
        }
    }
#endif

#if DT_NODE_HAS_STATUS(SOP_EXT_POWER_NODE, okay)
    /* EXT_POWER's suspend callback schedules its false state for persistence.
     * Enabling it immediately replaces that delayed save with true as well as
     * restoring the display rail. */
    const struct device *ext_power = DEVICE_DT_GET(SOP_EXT_POWER_NODE);
    if (device_is_ready(ext_power)) {
        int err = ext_power_enable(ext_power);
        if (err < 0) {
            LOG_WRN("soft-off-plus: failed to restore external power (%d)", err);
        }

        uint32_t settle_ms = DT_PROP_OR(SOP_EXT_POWER_NODE, init_delay_ms, 0);
        if (settle_ms > 0) {
            k_msleep(settle_ms);
        }
    }
#endif

    sop_restore_display();
}
