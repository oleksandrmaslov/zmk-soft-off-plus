/*
 * SPDX-License-Identifier: MIT
 *
 * Optional hook for customizing the hold-stage shutdown blank.
 */

#pragma once

#include <lvgl.h>

/**
 * Called once on ZMK's display work queue, right after the module creates the
 * blank overlay and before it is first shown. `overlay` is a screen-sized
 * opaque fill (LVGL white, or LVGL black with
 * CONFIG_ZMK_SOFT_OFF_PLUS_BLANK_INVERTED). Override this to add custom
 * content on top of the fill, e.g. a centered lv_image "goodbye" picture:
 *
 *     void zmk_soft_off_plus_blank_overlay_populate(lv_obj_t *overlay) {
 *         lv_obj_t *img = lv_img_create(overlay);
 *         lv_img_set_src(img, &my_goodbye_art);
 *         lv_obj_center(img);
 *     }
 *
 * The default (weak) implementation does nothing, leaving a solid blank. Note
 * that on inverted mono pipelines (see the Kconfig help) image pixels render
 * inverted on the panel as well, so author the asset accordingly. The overlay
 * is created once and re-shown on later blanks; it stays visible only while
 * the panel is powered (hold stage until the power rail drops at System OFF).
 */
void zmk_soft_off_plus_blank_overlay_populate(lv_obj_t *overlay);
