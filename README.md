# zmk-soft-off-plus

An enhanced soft-off module for ZMK. It layers two independent, configuration-
and devicetree-gated features on top of ZMK's built-in soft off:

1. **Split simultaneous off** — pressing soft off on *either* half powers off
   *both* halves over BLE.
2. **Hold-to-wake delay** — after the board has gone to soft off, the power/wake
   key must be *held* for a configurable time before the board actually turns
   on. A brief, accidental press goes straight back to deep sleep (System OFF)
   before the radio or display are powered, so it costs almost no battery.

It is the reworked successor to `zmk-soft-off-wake-delay`. The old approach
forced the PM state at init with `pm_state_force()`, which is unreliable; this
version goes back to sleep with `sys_poweroff()` after re-arming the wake GPIO,
the same mechanism ZMK itself uses, and drives both halves over a dedicated GATT
characteristic.

> Renaming note: the folder/module are now `zmk-soft-off-plus`. The git remote
> still points at the old `zmk-soft-off-wake-delay` repository — rename it on the
> host (or create a new repo) and update the remote when convenient.

## Requirements

- `CONFIG_ZMK_PM_SOFT_OFF=y` (this module builds on ZMK soft off).
- nRF (tested on nRF52840). Wake-from-System-OFF detection uses
  `hwinfo_get_reset_cause()` / `RESET_LOW_POWER_WAKE`.

## Kconfig

| Option | Default | Purpose |
| --- | --- | --- |
| `CONFIG_ZMK_SOFT_OFF_PLUS` | auto (`y` if a soft-off-plus node exists) | Master switch. |
| `CONFIG_ZMK_SOFT_OFF_PLUS_WAKE_DELAY` | auto (`y` if a `zmk,soft-off-plus-wake` node exists) | Boot-time hold-to-wake check. |
| `CONFIG_ZMK_SOFT_OFF_PLUS_WAKE_DELAY_INIT_PRIORITY` | `50` | POST_KERNEL priority of the boot check. Must run before the kscan that owns the wake pins (lower it when polling matrix pins — see corne below). |
| `CONFIG_ZMK_SOFT_OFF_PLUS_SPLIT_SYNC` | `y` on split | Cross-half "off" signalling over a dedicated GATT characteristic. |
| `CONFIG_ZMK_SOFT_OFF_PLUS_SPLIT_SYNC_FLUSH_MS` | `60` | Delay after sending the off signal before this half powers off, so the BLE write/notify is transmitted first. |

## Devicetree

### Behavior — `zmk,behavior-soft-off-plus`

```dts
/ {
    behaviors {
        soft_off_plus: soft_off_plus {
            compatible = "zmk,behavior-soft-off-plus";
            #binding-cells = <0>;
            hold-time-ms = <1200>; /* hold this long before release to fire; 0 = on release */
        };
    };
};
```

Bind `&soft_off_plus` from a keymap, or from a dedicated power key via
`zmk,kscan-sideband-behaviors`. The behavior has **central locality**: from a
keymap it always runs on the central, which then powers off every peripheral.
When invoked locally on a peripheral (e.g. a dedicated power key wired through a
sideband kscan) it notifies the central instead. Either way both halves go off.

### Hold-to-wake — `zmk,soft-off-plus-wake`

```dts
/ {
    soft_off_plus_wake: soft_off_plus_wake {
        compatible = "zmk,soft-off-plus-wake";
        wake-gpios = <&gpio0 3 (GPIO_ACTIVE_LOW | GPIO_PULL_UP)>; /* same pin(s) as the wake key */
        /* strobe-gpios = <...>;  // columns to drive for a matrix wake key */
        wake-hold-ms = <3000>;
        bypass-on-usb; /* skip the hold while USB is connected */
    };
};
```

| Property | Required | Description |
| --- | --- | --- |
| `wake-gpios` | yes | Input(s) polled at boot. Use the same active-level flags as the wake key. For a matrix key, the row/sense line(s). |
| `strobe-gpios` | no | Output(s) driven active while polling and left driven in System OFF (matrix column(s)). |
| `wake-hold-ms` | no (1000) | How long the input(s) must stay active to confirm a wake. |
| `bypass-on-usb` | no | Skip the hold requirement while USB is powered. |

The hold-to-wake check runs only when the reset was a wake from System OFF, so a
normal boot, USB plug, or reset button is unaffected. On a non-confirmed wake it
re-arms the wake input sense and calls `sys_poweroff()` immediately.

## Example: dedicated power key per half (e.g. wafer)

Each half already has a dedicated GPIO power key fed through
`zmk,kscan-sideband-behaviors`. Point the sideband binding at `&soft_off_plus`
(set `compatible = "zmk,behavior-soft-off-plus"` on the existing behavior),
add a `zmk,soft-off-plus-wake` node on the same pin, and keep the existing
`zmk,soft-off-wakeup-sources`. Pressing/holding the key on either half now turns
both off; holding it ~3s turns the board back on.

## Example: keymap soft off + any-key wake (e.g. corne)

No dedicated key. Bind `&soft_off_plus` in the keymap, make the matrix kscan the
wake source, and let the hold check poll the whole matrix:

```dts
/ {
    soft_off_wakers {
        compatible = "zmk,soft-off-wakeup-sources";
        wakeup-sources = <&kscan0>; /* any key wakes */
    };
    soft_off_plus_wake: soft_off_plus_wake {
        compatible = "zmk,soft-off-plus-wake";
        wake-gpios   = <&pro_micro 4 ...>, <&pro_micro 5 ...>, ...;   /* all rows  */
        strobe-gpios = <&pro_micro 14 ...>, <&pro_micro 15 ...>, ...; /* all cols  */
        wake-hold-ms = <1200>;
        bypass-on-usb;
    };
};
```

Because it polls matrix pins, set
`CONFIG_ZMK_SOFT_OFF_PLUS_WAKE_DELAY_INIT_PRIORITY=1` so the check runs before
the kscan driver claims them.

## Build wiring

This repo is consumed as an extra Zephyr module. Add its path to
`ZMK_EXTRA_MODULES` for each build, alongside the other local modules, e.g.:

```
-DZMK_EXTRA_MODULES="<ws>/zmk-nice-view-hid;<ws>/zmk-raw-hid;<ws>/zmk-split-peripheral-output-relay;<ws>/zmk-soft-off-plus"
```

(There is no `config/west.yml` entry; modules here are wired via
`ZMK_EXTRA_MODULES`.) Once the module is on the build and a soft-off-plus node is
present in devicetree, the relevant Kconfig options enable themselves.

## How "both halves off" works

The behavior never relies on ZMK's behavior relay (a dedicated/sideband key
bypasses it). Instead a small GATT service carries a one-byte off command:

- **central → peripheral(s):** the central writes the command to each peripheral.
- **peripheral → central:** the peripheral notifies the central.

The receiving side defers the power-off to a work item, so it never runs inside
a Bluetooth callback. Off-together is supported; wake-together is not, since a
half in System OFF cannot receive BLE — wake each half with its own key.
