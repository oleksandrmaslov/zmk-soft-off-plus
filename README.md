# zmk-soft-off-plus

An enhanced soft-off module for ZMK. Three independent, config- and
devicetree-gated features that build on ZMK's built-in soft off:

| Feature | What it does | Kconfig | Devicetree |
| --- | --- | --- | --- |
| **Enhanced soft off** | `&soft_off_plus` behavior; on a split, pressing it on **either** half powers off **both** over BLE | `ZMK_SOFT_OFF_PLUS_BEHAVIOR`, `…_SPLIT_SYNC` | `zmk,behavior-soft-off-plus` |
| **Hold-to-wake** | After soft off, the wake key must be **held** for a set time or the board drops straight back to deep sleep (saves battery on accidental presses) | `ZMK_SOFT_OFF_PLUS_WAKE_DELAY` | `zmk,soft-off-plus-wake` |
| **USB-gated wrapper** | `zmk,behavior-if-usb` runs a child behavior only while USB is connected — e.g. put `&bootloader` on the power key so DFU only works on the plugged-in half | `ZMK_SOFT_OFF_PLUS_IF_USB` | `zmk,behavior-if-usb` |

Every feature is opt-in: each Kconfig is `default y` **only when its devicetree
node is present**, so you just add the nodes you want and nothing else turns on.

> Successor to `zmk-soft-off-wake-delay`. The old version forced the PM state at
> init (`pm_state_force()`), which is unreliable; this one re-arms the wake GPIO
> and uses `sys_poweroff()`, the same path ZMK uses, and drives both halves over
> a dedicated GATT characteristic.

## 1. Requirements

- `CONFIG_ZMK_PM_SOFT_OFF=y` (this module builds on ZMK soft off).
- nRF target (tested on nRF52840). Hold-to-wake uses `hwinfo` reset-cause
  (`RESET_LOW_POWER_WAKE`).

## 2. Add the module to your build

**Option A — west manifest** (recommended for GitHub Actions). In
`config/west.yml`:

```yaml
manifest:
  remotes:
    - name: oleksandrmaslov
      url-base: https://github.com/oleksandrmaslov
  projects:
    - name: zmk-soft-off-plus
      remote: oleksandrmaslov
      revision: main          # or a branch/tag
```

**Option B — local module.** Pass the path in your build command:

```
-DZMK_EXTRA_MODULES="<workspace>/zmk-soft-off-plus"
```

## 3. Config reference

| Option | Default | Notes |
| --- | --- | --- |
| `ZMK_SOFT_OFF_PLUS` | auto | Master switch (depends on `ZMK_PM_SOFT_OFF`). |
| `ZMK_SOFT_OFF_PLUS_BEHAVIOR` | auto | The `&soft_off_plus` behavior. |
| `ZMK_SOFT_OFF_PLUS_SPLIT_SYNC` | `y` on split | Power both halves off together (needs `ZMK_SPLIT` + `BT`). |
| `ZMK_SOFT_OFF_PLUS_SPLIT_SYNC_FLUSH_MS` | `60` | Delay so the BLE off-signal flushes before powering off. |
| `ZMK_SOFT_OFF_PLUS_WAKE_DELAY` | auto | Boot-time hold-to-wake check. |
| `ZMK_SOFT_OFF_PLUS_WAKE_DELAY_INIT_PRIORITY` | `50` | POST_KERNEL priority; lower (e.g. `1`) when polling matrix pins. |
| `ZMK_SOFT_OFF_PLUS_IF_USB` | auto | The `zmk,behavior-if-usb` wrapper. |

"auto" = `default y` when the matching devicetree node exists.

## 4. Recipe A — dedicated power/soft-off switch (e.g. wafer)

A physical power button per half, on its own pin, fed through a sideband kscan.
Hold = power off both halves; **triple-tap = bootloader, only on the half that
is plugged into USB**; hold to wake.

```dts
/ {
    keys {
        compatible = "gpio-keys";
        soft_off_gpio_key: soft_off_gpio_key {
            gpios = <&gpio0 3 (GPIO_ACTIVE_LOW | GPIO_PULL_UP)>; /* your button pin */
        };
    };

    behaviors {
        hw_soft_off: hw_soft_off {
            compatible = "zmk,behavior-soft-off-plus";
            #binding-cells = <0>;
            hold-time-ms = <3000>;        /* hold this long to power off */
        };

        /* &bootloader, but only while THIS half is on USB */
        if_usb_bootloader: if_usb_bootloader {
            compatible = "zmk,behavior-if-usb";
            #binding-cells = <0>;
            bindings = <&bootloader>;
        };

        /* hold -> soft off (both halves) | 2 taps -> nothing | 3 taps -> DFU (USB only) */
        power_btn_combo: power_btn_combo {
            compatible = "zmk,behavior-tap-dance";
            #binding-cells = <0>;
            tapping-term-ms = <300>;
            bindings = <&hw_soft_off>, <&none>, <&if_usb_bootloader>;
        };
    };

    wakeup_scan: wakeup_scan {
        compatible = "zmk,kscan-gpio-direct";
        input-keys = <&soft_off_gpio_key>;
        wakeup-source;
    };

    side_band_behavior_triggers: side_band_behavior_triggers {
        compatible = "zmk,kscan-sideband-behaviors";
        kscan = <&wakeup_scan>;
        auto-enable;
        wakeup-source;
        soft_off {
            column = <0>;
            row = <0>;
            bindings = <&power_btn_combo>;
        };
    };

    /* arm the button as a wake source while in soft off */
    soft_off_wakers {
        compatible = "zmk,soft-off-wakeup-sources";
        wakeup-sources = <&wakeup_scan>;
    };

    /* require a ~3s hold of the same button to actually turn on */
    soft_off_plus_wake: soft_off_plus_wake {
        compatible = "zmk,soft-off-plus-wake";
        wake-gpios = <&gpio0 3 (GPIO_ACTIVE_LOW | GPIO_PULL_UP)>; /* same pin/flags */
        wake-hold-ms = <3000>;
        bypass-on-usb;
    };
};
```

`&bootloader` / `&none` come from ZMK's `behaviors.dtsi` (already included by
keymaps). No `.conf` changes are needed — every feature auto-enables from the
nodes above (assuming `CONFIG_ZMK_PM_SOFT_OFF=y`).

> The `if-usb` wrapper is only "active" where you bind it — here, on the
> dedicated power key's tap-dance. It is not compiled at all unless a
> `zmk,behavior-if-usb` node exists, and each half checks **its own** USB.

## 5. Recipe B — keymap soft off + any-key wake (e.g. corne)

No extra hardware: bind `&soft_off_plus` on a key, wake on any key, and confirm
the wake by holding any key.

`config` / shield `.conf`:

```conf
CONFIG_ZMK_PM_SOFT_OFF=y
# the wake check polls matrix pins, so it must run before the kscan driver
CONFIG_ZMK_SOFT_OFF_PLUS_WAKE_DELAY_INIT_PRIORITY=1
```

Overlay / shield `.dtsi`:

```dts
/ {
    behaviors {
        soft_off_plus: soft_off_plus {
            compatible = "zmk,behavior-soft-off-plus";
            #binding-cells = <0>;
            hold-time-ms = <1200>;
        };
    };

    soft_off_wakers {
        compatible = "zmk,soft-off-wakeup-sources";
        wakeup-sources = <&kscan0>;   /* any key wakes */
    };

    soft_off_plus_wake: soft_off_plus_wake {
        compatible = "zmk,soft-off-plus-wake";
        wake-gpios   = <&pro_micro 4 (GPIO_ACTIVE_HIGH | GPIO_PULL_DOWN)>,  /* all rows */
                       <&pro_micro 5 (GPIO_ACTIVE_HIGH | GPIO_PULL_DOWN)>,
                       <&pro_micro 6 (GPIO_ACTIVE_HIGH | GPIO_PULL_DOWN)>,
                       <&pro_micro 7 (GPIO_ACTIVE_HIGH | GPIO_PULL_DOWN)>;
        strobe-gpios = <&pro_micro 14 GPIO_ACTIVE_HIGH>,                    /* all cols */
                       <&pro_micro 15 GPIO_ACTIVE_HIGH>,
                       <&pro_micro 18 GPIO_ACTIVE_HIGH>,
                       <&pro_micro 19 GPIO_ACTIVE_HIGH>,
                       <&pro_micro 20 GPIO_ACTIVE_HIGH>,
                       <&pro_micro 21 GPIO_ACTIVE_HIGH>;
        wake-hold-ms = <1200>;
        bypass-on-usb;
    };
};
```

Then drop `&soft_off_plus` onto a key in your keymap. To require a specific wake
key instead of "any key", narrow `wakeup-sources` to a `zmk,gpio-key-wakeup-trigger`
and `wake-gpios`/`strobe-gpios` to that one row/column.

## 6. Node/behavior reference

**`zmk,behavior-soft-off-plus`** — `hold-time-ms` (default 0 = trigger on
release). Central locality: from a keymap it runs on the central; via a sideband
key it runs on whichever half, which then signals the other.

**`zmk,soft-off-plus-wake`** — `wake-gpios` (required, the input(s) to poll),
`strobe-gpios` (optional outputs to drive for a matrix key), `wake-hold-ms`
(default 1000), `bypass-on-usb` (skip the hold while on USB).

**`zmk,behavior-if-usb`** — `bindings` (the child behavior to run while USB is
powered). `#binding-cells = <0>`.

## 7. How "both halves off" works

A dedicated GATT service carries a one-byte off command between halves: the
central writes it to each peripheral, a peripheral notifies the central; the
receiver powers off from a work item. Off-together is supported; wake-together
is not (a half in System OFF can't receive BLE) — wake each half with its own key.
