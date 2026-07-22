#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/devicetree.h>
#include <hal/nrf_gpio.h>

#if DT_HAS_COMPAT_STATUS_OKAY(kobitokey_haptic_local_input)
#include <zephyr/input/input.h>
#endif

#include "kobitokey_haptic.h"

/*
 * XIAO BLE / nRF52840
 * P1.10 = port 1, pin 10
 */
#define HAPTIC_PIN NRF_GPIO_PIN_MAP(1, 11)

/*
 * Kconfig から値を読む。
 * 左右で違う値にしたい場合は、
 * KobitoKey_left.conf / KobitoKey_right.conf で上書きする。
 */
#define HAPTIC_PULSE_MS CONFIG_KOBITOKEY_HAPTIC_PULSE_MS
#define HAPTIC_COOLDOWN_MS CONFIG_KOBITOKEY_HAPTIC_COOLDOWN_MS

#define HAPTIC_BOOT_DELAY_MS CONFIG_KOBITOKEY_HAPTIC_BOOT_DELAY_MS
#define HAPTIC_BOOT_LONG_MS CONFIG_KOBITOKEY_HAPTIC_BOOT_LONG_MS
#define HAPTIC_BOOT_GAP1_MS CONFIG_KOBITOKEY_HAPTIC_BOOT_GAP1_MS
#define HAPTIC_BOOT_SHORT_MS CONFIG_KOBITOKEY_HAPTIC_BOOT_SHORT_MS
#define HAPTIC_BOOT_GAP2_MS CONFIG_KOBITOKEY_HAPTIC_BOOT_GAP2_MS

static int64_t last_haptic_time = 0;

static struct k_work_delayable haptic_off_work;
static struct k_work_delayable haptic_boot_work;

static void haptic_on(void) {
    nrf_gpio_pin_set(HAPTIC_PIN);
}

static void haptic_off(void) {
    nrf_gpio_pin_clear(HAPTIC_PIN);
}

static void haptic_off_work_handler(struct k_work *work) {
    haptic_off();
}

void kobitokey_haptic_pulse_ms(uint32_t duration_ms) {
    int64_t now = k_uptime_get();

    if (now - last_haptic_time < HAPTIC_COOLDOWN_MS) {
        return;
    }

    last_haptic_time = now;

    haptic_on();
    k_work_reschedule(&haptic_off_work, K_MSEC(duration_ms));
}

void kobitokey_haptic_pulse(void) {
    kobitokey_haptic_pulse_ms(HAPTIC_PULSE_MS);
}

static void haptic_boot_work_handler(struct k_work *work) {
    haptic_on();
    k_sleep(K_MSEC(HAPTIC_BOOT_LONG_MS));
    haptic_off();

    k_sleep(K_MSEC(HAPTIC_BOOT_GAP1_MS));

    haptic_on();
    k_sleep(K_MSEC(HAPTIC_BOOT_SHORT_MS));
    haptic_off();

    k_sleep(K_MSEC(HAPTIC_BOOT_GAP2_MS));

    haptic_on();
    k_sleep(K_MSEC(HAPTIC_BOOT_SHORT_MS));
    haptic_off();
}

#if DT_HAS_COMPAT_STATUS_OKAY(kobitokey_haptic_local_input)

/*
 * 左手 peripheral 用。
 *
 * input processor として挟まず、
 * input callback でRELイベントを横から見る。
 *
 * これにより tb_left_split の送信経路を邪魔しない。
 */
static void kobitokey_haptic_local_input_cb(struct input_event *event) {
    if (event->type != INPUT_EV_REL || event->value == 0) {
        return;
    }

    if (event->code == INPUT_REL_X ||
        event->code == INPUT_REL_Y ||
        event->code == INPUT_REL_WHEEL ||
        event->code == INPUT_REL_HWHEEL) {
        kobitokey_haptic_pulse();
    }
}

INPUT_CALLBACK_DEFINE(NULL, kobitokey_haptic_local_input_cb);

#endif

static int haptic_init(void) {
    nrf_gpio_cfg_output(HAPTIC_PIN);
    haptic_off();

    k_work_init_delayable(&haptic_off_work, haptic_off_work_handler);

    k_work_init_delayable(&haptic_boot_work, haptic_boot_work_handler);
    k_work_reschedule(&haptic_boot_work, K_MSEC(HAPTIC_BOOT_DELAY_MS));

    return 0;
}

SYS_INIT(haptic_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);