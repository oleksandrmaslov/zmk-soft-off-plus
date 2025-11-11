# ZMK Long-Press Wake Module

This extra module adds long-press filtering for SOFT-OFF wake events and safely
gates any external power rails until a wake key has been held for a configured
duration. It ensures accidental or short presses immediately return the board
to SOFT-OFF while keeping optional external peripherals unpowered until a valid
wake is confirmed.

## Integration Steps

1. Add this repository to `ZMK_EXTRA_MODULES` in your board's west manifest or
   `build.yaml`.
2. Enable the feature in your configuration (if not already defaulted):
   `CONFIG_ZMK_LONG_PRESS_WAKE=y`.
3. Define a `long_press_wake` node in your board devicetree overlay with the
   `compatible = "zmk,long-press-wake"` property. Configure:
   - `wake-gpios` for one or more dedicated wake inputs.
   - Optional `required-hold-ms`, `bypass-on-usb`, `active-low`,
     `ext-power-gpios`, and `ext-power-on-delay-ms`.
4. Ensure the wake GPIOs are also marked as hardware wakeup sources in the
   SoC/board devicetree so they can bring the MCU out of SOFT-OFF.

## Devicetree Example

```dts
/ {
    long_press_wake: long_press_wake {
        compatible = "zmk,long-press-wake";

        /* Dedicated power / wake key */
        wake-gpios = <&gpio0 11 (GPIO_PULL_UP | GPIO_ACTIVE_LOW)>;

        /* Require 3s hold */
        required-hold-ms = <3000>;

        /* Allow skipping long-press when on USB power */
        bypass-on-usb;

        /* Wake key is active low */
        active-low;

        /* Optional: external power enable (e.g. PMIC EN, display power, etc.) */
        ext-power-gpios = <&gpio0 13 GPIO_OUTPUT_ACTIVE>;
        ext-power-on-delay-ms = <50>;
    };
};

/* Example: ensure wake-gpios pin is also marked as a hardware wakeup source
 * in the SoC/board configuration (simplified, illustrative):
 *
 * &gpio0 {
 *     wakeup-source;
 * };
 */
```

## Behavior Summary

| Scenario | Result |
| --- | --- |
| Short press `< required-hold-ms` | MCU immediately returns to a low-power interrupt-enabled state (falls back to SOFT-OFF if required), external power stays OFF. |
| Long press `>= required-hold-ms` | Wake accepted, optional delay applied, external power turns ON. |
| `bypass-on-usb` + USB present | Long-press check skipped, external power turns ON after optional delay. |

