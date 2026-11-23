"""Run the module's real wake code with simulated GPIO, clock and retention I/O."""

from pathlib import Path
import subprocess
import tempfile

MODULE = Path(__file__).resolve().parents[1]
ENV = r'''
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <stdio.h>

#define CONFIG_ZMK_USB TEST_CENTRAL
#define CONFIG_SOC_FAMILY_NORDIC_NRF 1
#define CONFIG_ZMK_SOFT_OFF_PLUS_WAKE_DELAY 1
#define CONFIG_ZMK_SOFT_OFF_PLUS_WAKE_DELAY_INIT_PRIORITY 1
#define CONFIG_ZMK_SOFT_OFF_PLUS_LOG_LEVEL 0
#define IS_ENABLED(x) x
#define LOG_MODULE_DECLARE(...)
#define LOG_INF(...)
#define LOG_WRN(...)
#define LOG_ERR(...)
#define LOG_DBG(...)
#define SYS_INIT(...)
#define FUNC_NORETURN __attribute__((noreturn))
#define CODE_UNREACHABLE __builtin_unreachable()
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define RESET_LOW_POWER_WAKE 1U
#define RESET_SOFTWARE 2U
#define GPIO_INPUT 1
#define GPIO_OUTPUT_INACTIVE 2
#define GPIO_INT_LEVEL_ACTIVE 3

#define NODE_zmk_soft_off_plus_wake 1
#define NODE_zmk_ext_power_generic 2
#define DT_COMPAT_GET_ANY_STATUS_OKAY(compat) NODE_##compat
#define DT_NODE_HAS_STATUS(node, status) 1
#define HAS_strobe_gpios 0
#define HAS_bypass_on_usb TEST_BYPASS
#define DT_NODE_HAS_PROP(node, prop) HAS_##prop
#define DT_PROP_OR(node, prop, fallback) 1500
#define DT_FOREACH_PROP_ELEM(node, prop, fn) fn(node, prop, 0)
#define GPIO_DT_SPEC_GET_BY_IDX(node, prop, index) { .pin = ((node) == 1 ? 3 : 29) }

struct gpio_dt_spec { unsigned pin; };
static unsigned now_ms, reset_cause, window_start[2], window_end[2];
static unsigned gpregret[2];
static bool usb_present, ext_power_off;
static jmp_buf off_return;

static bool gpio_is_ready_dt(const struct gpio_dt_spec *gpio) { return true; }
static int gpio_pin_get_dt(const struct gpio_dt_spec *gpio) {
    return (now_ms >= window_start[0] && now_ms < window_end[0]) ||
           (now_ms >= window_start[1] && now_ms < window_end[1]);
}
static int gpio_pin_configure_dt(const struct gpio_dt_spec *gpio, int flags) {
    if (gpio->pin == 29 && flags == GPIO_OUTPUT_INACTIVE) ext_power_off = true;
    return 0;
}
static int gpio_pin_interrupt_configure_dt(const struct gpio_dt_spec *gpio, int flags) {
    return 0;
}
static void k_busy_wait(unsigned us) {}
static int k_msleep(unsigned ms) { assert(ext_power_off); now_ms += ms; return 0; }
static int hwinfo_get_reset_cause(uint32_t *cause) { *cause = reset_cause; return 0; }
static int hwinfo_clear_reset_cause(void) { reset_cause = 0; return 0; }
static __attribute__((noreturn)) void sys_poweroff(void) { longjmp(off_return, 1); }
static int zmk_pm_soft_off(void) { sys_poweroff(); }
static bool zmk_usb_is_powered(void) { return TEST_USB_READY && usb_present; }

#define NRF_POWER gpregret
#define NRF_POWER_HAS_USBREG 1
#define NRF_POWER_HAS_GPREGRET 1
#define NRF_POWER_HAS_GPREGRET_ARRAY 1
static void nrf_power_gpregret_set(unsigned *power, unsigned index, unsigned value) {
    power[index] = value;
}
static unsigned nrf_power_gpregret_get(unsigned *power, unsigned index) {
    return power[index];
}
static bool nrf_power_usbregstatus_vbusdet_get(unsigned *power) { return usb_present; }
'''

TEST = r'''
#include "harness_env.h"
#include "off_marker.c"
#include "wake_delay.c"

static void prepare(bool manual, bool usb, unsigned first_release,
                    unsigned second_start, unsigned second_release) {
    now_ms = 0;
    usb_present = usb;
    ext_power_off = false;
    gpregret[0] = 0;
    gpregret[1] = 0;
    if (manual) zmk_soft_off_plus_off_marker_set();
    window_start[0] = 0;
    window_end[0] = first_release;
    window_start[1] = second_start;
    window_end[1] = second_release;
}

static bool boot(unsigned cause) {
    reset_cause = cause;
    if (setjmp(off_return)) return false;
    assert(wake_delay_init() == 0);
    return true;
}

int main(void) {
    const char *role = TEST_CENTRAL ? "central" : "peripheral";
    prepare(true, false, 50, 100, 150);
    assert(!boot(RESET_LOW_POWER_WAKE));
    assert(now_ms == 50 && ext_power_off && gpregret[1] == 0xA5);
    now_ms = 100;
    assert(!boot(RESET_LOW_POWER_WAKE));
    assert(now_ms == 150 && gpregret[1] == 0xA5);
    printf("%s: battery single/double short taps rejected; off marker retained\n", role);

    prepare(true, false, 1600, 0, 0);
    assert(boot(RESET_LOW_POWER_WAKE));
    assert(now_ms == 1500 && gpregret[1] == 0);
    printf("%s: battery continuous hold accepted at 1500 ms\n", role);

    prepare(true, true, 50, 0, 0);
    bool accepted = boot(RESET_LOW_POWER_WAKE);
    assert(accepted == (bool)TEST_BYPASS);
    assert(now_ms == (TEST_BYPASS ? 20 : 50));
    assert(ext_power_off);
    printf("%s: USB short tap %s (bypass=%d, %u ms)\n", role,
           accepted ? "accepted" : "rejected", TEST_BYPASS, now_ms);

    prepare(true, true, 5, 0, 0);
    assert(!boot(RESET_LOW_POWER_WAKE));

    prepare(false, false, 50, 0, 0);
    assert(boot(RESET_LOW_POWER_WAKE));
    assert(now_ms == 0);
    printf("%s: missing manual-off marker bypasses hold as inactivity wake\n", role);

    prepare(true, false, 50, 0, 0);
    assert(boot(0));
    assert(now_ms == 0);
    printf("%s: missing low-power reset cause also bypasses hold\n", role);

    prepare(true, false, 0, 0, 0);
    assert(boot(RESET_SOFTWARE));
    assert(gpregret[1] == 0);
    return 0;
}
'''

headers = ('zephyr/kernel.h', 'zephyr/device.h', 'zephyr/devicetree.h',
           'zephyr/drivers/gpio.h', 'zephyr/drivers/hwinfo.h', 'zephyr/init.h',
           'zephyr/sys/poweroff.h', 'zephyr/sys/util.h', 'zephyr/toolchain.h',
           'zephyr/logging/log.h', 'zmk/usb.h', 'zmk/pm.h', 'hal/nrf_power.h')
with tempfile.TemporaryDirectory(prefix='wafer-wake-check-') as temporary:
    root = Path(temporary)
    (root/'harness_env.h').write_text(ENV)
    (root/'test.c').write_text(TEST)
    for header in headers:
        target = root/header
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text('#include "harness_env.h"\n')
    for central in (1, 0):
        for bypass in (1, 0):
            executable = root/f'wake-{central}-{bypass}'
            subprocess.run(['gcc', '-std=c11', '-Werror=implicit-function-declaration',
                            f'-DTEST_CENTRAL={central}', f'-DTEST_BYPASS={bypass}', '-DTEST_USB_READY=0',
                            '-I', str(root), '-I', str(MODULE/'include'),
                            '-I', str(MODULE/'src'), str(root/'test.c'),
                            '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True)
