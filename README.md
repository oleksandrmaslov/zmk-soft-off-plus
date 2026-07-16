# zmk-soft-off-plus

An enhanced soft-off module for ZMK. Five independent, config- and
devicetree-gated features that build on ZMK's built-in soft off:

| Feature | What it does | Kconfig | Devicetree |
| --- | --- | --- | --- |
| **Enhanced soft off** | `&soft_off_plus` behavior; powers off **both** halves even from a **dedicated/sideband power key** — the case ZMK's `&soft_off` can't cover (see below) | `ZMK_SOFT_OFF_PLUS_BEHAVIOR`, `…_SPLIT_SYNC` | `zmk,behavior-soft-off-plus` |
| **Hold-to-wake** | After soft off, the wake key must be **held** for a set time or the board drops straight back to deep sleep (saves battery on accidental presses) | `ZMK_SOFT_OFF_PLUS_WAKE_DELAY` | `zmk,soft-off-plus-wake` |
| **USB-gated wrapper** | `zmk,behavior-if-usb` runs a child behavior only while USB is connected — e.g. put `&bootloader` on the power key so DFU only works on the plugged-in half | `ZMK_SOFT_OFF_PLUS_IF_USB` | `zmk,behavior-if-usb` |
| **Sideband power gesture** | Split-safe hold + triple-tap handling for a dedicated power button, without ZMK tap-dance's central-only global key listener | `ZMK_SOFT_OFF_PLUS_POWER_BUTTON` | `zmk,behavior-power-button` |
| **Deep-sleep wake guard** | Keeps wake-capable matrix scanners and sideband wrappers armed during inactivity deep sleep | `ZMK_SOFT_OFF_PLUS_DEEP_SLEEP_WAKE` | `wakeup-source` on the scanner and sideband wrapper |

Every feature is opt-in: each Kconfig is `default y` only when its matching
devicetree node or scanner is present. The wake guard only acts on devices that
also carry `wakeup-source`, so unrelated scanners remain unchanged.

## What this fixes that ZMK's `&soft_off` doesn't

ZMK's built-in `&soft_off` **already powers off both halves** when you bind it on
a normal **keymap** key: a peripheral has no keymap, so its keypress is sent to
the central, the central runs the behavior (`BEHAVIOR_LOCALITY_GLOBAL`) and
relays it to every half, and each half powers itself off. If a keymap key is all
you need, you do **not** need this module's behavior — use `&soft_off` and take
only the hold-to-wake / if-usb features here.

The gap is the **dedicated power key on its own pin, wired through
`kscan-sideband-behaviors`** (Recipe A). A sideband key runs the behavior
*locally* on the one half it's wired to and **bypasses the keymap relay**, so
ZMK's `&soft_off` there powers off only that half — the other half stays on.
`&soft_off_plus` closes this gap: when it runs on one half it sends a one-byte
"off" command to the other half over a small dedicated GATT service (central →
peripheral by write, peripheral → central by notify), so a sideband press on
**either** half drops **both**. A one-shot guard keeps the keymap-relay path and
this GATT path from ever powering a half off twice.

So: keymap soft-off → ZMK already handles it (this behavior just rides the same
relay, with the GATT as a harmless backup); **sideband/dedicated-power-key
soft-off → this module is what makes both halves drop.** The hold-to-wake delay
and the if-usb wrapper have no ZMK equivalent at all.

> Successor to `zmk-soft-off-wake-delay`. The old version forced the PM state at
> init (`pm_state_force()`), which is unreliable; this one re-arms the wake GPIO
> and uses `sys_poweroff()`, the same path ZMK uses. Keymap presses ride ZMK's
> behavior relay; a sideband press signals the other half over a dedicated GATT
> characteristic. On nRF52840, a marker in the second GPREGRET byte distinguishes
> a deliberate soft off from ZMK's inactivity System OFF: hold-to-wake applies
> only to deliberate soft off, while a matrix or sideband inactivity wake boots
> normally.

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
| `ZMK_SOFT_OFF_PLUS_POWER_BUTTON` | auto | Split-safe dedicated-button hold/triple-tap gesture. |
| `ZMK_SOFT_OFF_PLUS_DEEP_SLEEP_WAKE` | auto | Preserve wake-enabled matrix and sideband input paths across inactivity deep sleep. |
| `ZMK_SOFT_OFF_PLUS_DEEP_SLEEP_WAKE_INIT_PRIORITY` | `99` | Late APPLICATION priority; must run after ZMK initializes the scanners. |

"auto" = `default y` when the matching devicetree node exists.

## 4. Recipe A — dedicated power/soft-off switch (e.g. wafer)

A physical power button per half, on its own pin, fed through a sideband kscan.
Hold = power off both halves; **triple-tap = bootloader, only on the half that
is plugged into USB**; hold to wake.

> This is the case `&soft_off_plus` exists for (see
> [What this fixes](#what-this-fixes-that-zmks-soft_off-doesnt)). The sideband
> key runs the behavior locally on one half, so ZMK's `&soft_off` would leave the
> other half on; the GATT off-signal here is what drops both.

```dts
/ {
    keys {
        compatible = "gpio-keys";
        soft_off_gpio_key: soft_off_gpio_key {
            gpios = <&gpio0 3 (GPIO_ACTIVE_LOW | GPIO_PULL_UP)>; /* your button pin */
        };
    };

    behaviors {
        /* Keep the node name <= 8 characters: ZMK's split behavior relay has
         * an 8-character device-name payload. The phandle label may be longer. */
        hw_soft_off: sop {
            compatible = "zmk,behavior-soft-off-plus";
            #binding-cells = <0>;
            hold-time-ms = <2700>;        /* + 300 ms gesture decision = ~3 s physical hold */
            trigger-on-hold;              /* request blank at hold; System OFF on release */
        };

        /* &bootloader, but only while THIS half is on USB */
        if_usb_dfu: if_usb_dfu {
            compatible = "zmk,behavior-if-usb";
            #binding-cells = <0>;
            bindings = <&bootloader>;
        };

        /* hold -> soft off (both halves) | 2 taps -> nothing | 3 taps -> DFU (USB only).
         * This dedicated gesture has no global position listener, so it is safe
         * to compile and run on a split peripheral. */
        power_btn_combo: pwr_cmb {
            compatible = "zmk,behavior-power-button";
            #binding-cells = <0>;
            tapping-term-ms = <300>;
            bindings = <&hw_soft_off>, <&if_usb_dfu>;
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

> The `if-usb` wrapper is only "active" where you bind it — here, as the
> dedicated power gesture's triple-tap child. It is not compiled at all unless
> a `zmk,behavior-if-usb` node exists. The gesture executes on the physical half
> that owns the button, so that half checks **its own** USB/VBUS and runs
> `&bootloader` locally. Nordic split peripherals read VBUS directly because
> ZMK's USB stack is central-only.

## 5. Recipe B — keymap soft off + any-key wake (e.g. corne)

No extra hardware: bind `&soft_off_plus` on a key, wake on any key, and confirm
the wake by holding any key.

> For a **keymap** key like this, ZMK's built-in `&soft_off` already powers off
> both halves — so the real reason to run this recipe is the **hold-to-wake**
> feature. Using `&soft_off_plus` (instead of `&soft_off`) on the key is fine:
> it rides the same relay, plus it keeps the off-signal working if you later add
> a sideband power key. Swap in `&soft_off` if you prefer the built-in.

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

**`zmk,behavior-soft-off-plus`** — `hold-time-ms` (default 0), `trigger-on-hold`
(boolean). By default soft-off fires **on release** once `hold-time-ms` has
elapsed. `trigger-on-hold` makes it **two-phase, phone-style**: the moment
`hold-time-ms` passes while you're still holding, the keyboard requests display
blanking for visual confirmation and tells the other half to drop too. The half
**you are holding** defers its real System OFF until
you **release** (its key is also the wake source — powering off while it's down
would re-wake the board). Any half with **nothing held** — the non-wired half of
a sideband press — has no such constraint, so it just **powers off right away**
when told; its power-off never depends on a release being relayed back across the
link. That sidesteps the nRF *"System OFF while DETECT is high → instant
re-wake"* trap, so it's **safe on a key that is also the wake source**. Releasing
before `hold-time-ms` cancels with nothing dropped.
GLOBAL locality (like ZMK's `&soft_off`): from a keymap, ZMK runs it on the
central **and** relays it to every peripheral, so each half powers *itself* off —
no cross-half radio handshake needed. Via a `kscan-sideband-behaviors` key it
bypasses locality and runs on only that half, which then signals the other over
BLE.

> **Why two-phase?** Powering off *while the key is held* can't work for a key
> that also wakes the board: on nRF *"setting the system to System OFF while
> DETECT is high causes a wakeup from System OFF reset,"* so it would re-wake
> instantly. Splitting it — request display blanking on hold, `sys_poweroff()` on
> release (pin low, sleeps cleanly) — gives the phone-style feel while keeping
> the wake correct **for the half whose key you're holding**.
>
> The cross-half signal carries this distinction. On hold, each half sends the
> other a **DROP**. A half that is *itself* holding a soft-off-plus key (either
> half on a matrix press, or the wired half of a sideband press) treats a DROP as
> *blank-only* and waits for its own key-release — so a matrix central is never
> powered off mid-hold and stays alive to relay your release. A half with
> **nothing held** treats a DROP as *power off now*: it has no wake key down to
> re-wake it, and holding past `hold-time-ms` already committed you to off, so it
> doesn't wait for any further release to be synchronized across the link. (An
> **OFF** is still sent on release as a redundant backup; the one-shot claim keeps
> a half from powering off twice.)
>
> Phase 1 calls `display_blanking_on()` (the same call ZMK's blank-on-idle uses)
> for a clean panel-level blank on displays that support it (an OLED, or an LS0xx
> wired with `disp-en-gpios`). A **bare nice!view** has no DISP pin, so the module
> falls back to an opaque LVGL overlay and refreshes it on ZMK's display
> queue. The overlay is LVGL-white; on pipelines that render LVGL colors inverted
> on the panel (Zephyr's LVGL 9 mono glue does on MONO01 panels — a nice!view on
> ZMK's Zephyr 4.1 stack shows LVGL white as *black*), set
> `CONFIG_ZMK_SOFT_OFF_PLUS_BLANK_INVERTED=y` so the visible blank is white
> there too. To show a picture instead of a plain blank, override the weak hook
> `zmk_soft_off_plus_blank_overlay_populate()` (declared in
> `zmk/soft_off_plus/display_overlay.h`) in any compiled source and add e.g. a
> centered `lv_img` to the overlay — it shows during the hold stage and stays
> until the panel loses power at System OFF. Phase 1 deliberately does not
> suspend the SPI/display graph or cut
> `ext_power`: doing so while the release path and LS0xx VCOM thread are still
> running can strand the input path, drive an unpowered panel, and persist the
> external rail's OFF state into settings. Final System OFF performs the real
> device suspend and rail removal immediately before sleep. If that PM sequence
> returns with an error, the module resumes the device graph and restores
> external power instead of leaving the display rail off.

**`zmk,soft-off-plus-wake`** — `wake-gpios` (required, the input(s) to poll),
`strobe-gpios` (optional outputs to drive for a matrix key), `wake-hold-ms`
(default 1000), `bypass-on-usb` (after confirming the wake input is active, skip
the remaining hold while on USB). For a matrix wake
key, `wake-gpios` is the key's **row** (sense) line and `strobe-gpios` is its
**column** (drive) line — they must match the row/column GPIOs of that exact
key in your kscan, or the key can't be sensed at wake. While the hold check is
running on battery, the module forces any `zmk,ext-power` rail off so an
unconfirmed (too-short) wake never lights the display or other external
peripherals; the rail is re-enabled normally once the wake is confirmed.

**`zmk,behavior-if-usb`** — `bindings` (the child behavior to run while USB is
powered). `#binding-cells = <0>`.

## 7. How "both halves off" works

From a keymap, the behavior uses **GLOBAL locality** (like ZMK's built-in
`&soft_off`): ZMK runs it on the central and relays it to every peripheral, so
each half runs the behavior and powers *itself* off. This is the reliable path
and needs no cross-half radio handshake.

For a dedicated power key wired through `kscan-sideband-behaviors`, the behavior
runs on only the half it is wired to, so a small GATT service carries a one-byte
off command to the other half (the central writes it to each peripheral; a
peripheral notifies the central) and the receiver powers off from a work item. A
one-shot guard keeps the relayed run and this signal from powering a half off
twice.

Off-together is supported; wake-together is not (a half in System OFF can't
receive BLE) — wake each half with its own key.
