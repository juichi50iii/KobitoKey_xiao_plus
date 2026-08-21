#include <stdbool.h>
#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>

#include <hal/nrf_gpio.h>

#if DT_HAS_COMPAT_STATUS_OKAY(kobitokey_haptic_local_input)
#include <zephyr/input/input.h>
#endif

#include "kobitokey_haptic.h"

#if defined(CONFIG_KOBITOKEY_VBUS_SENSE)
#include "kobitokey_vbus.h"
#endif


#define HAPTIC_PIN NRF_GPIO_PIN_MAP(1, 11)

/*
 * Kconfigから値を読む。
 * 左右で違う値にしたい場合は、
 * KobitoKey_left.conf / KobitoKey_right.confで上書きする。
 */
#define HAPTIC_PULSE_MS CONFIG_KOBITOKEY_HAPTIC_PULSE_MS
#define HAPTIC_COOLDOWN_MS CONFIG_KOBITOKEY_HAPTIC_COOLDOWN_MS

#define HAPTIC_BOOT_DELAY_MS CONFIG_KOBITOKEY_HAPTIC_BOOT_DELAY_MS
#define HAPTIC_BOOT_LONG_MS CONFIG_KOBITOKEY_HAPTIC_BOOT_LONG_MS
#define HAPTIC_BOOT_GAP1_MS CONFIG_KOBITOKEY_HAPTIC_BOOT_GAP1_MS
#define HAPTIC_BOOT_SHORT_MS CONFIG_KOBITOKEY_HAPTIC_BOOT_SHORT_MS
#define HAPTIC_BOOT_GAP2_MS CONFIG_KOBITOKEY_HAPTIC_BOOT_GAP2_MS

#if defined(CONFIG_KOBITOKEY_HAPTIC_USB_PULSE_MS)
#define HAPTIC_USB_PULSE_MS CONFIG_KOBITOKEY_HAPTIC_USB_PULSE_MS
#else
#define HAPTIC_USB_PULSE_MS 25
#endif


static int64_t last_haptic_time;

/*
 * True from boot until the startup pattern has finished.
 *
 * The pattern runs inline with sleeps between its buzzes, which means the
 * workqueue is held for as long as it takes -- including the work that
 * switches this motor back off. A movement-driven pulse started in that
 * window turns the motor on and cannot turn it off again until the pattern
 * ends, so touching the ball just after boot produced one very long buzz
 * instead of a short one.
 *
 * Rather than let a pulse start and then get stuck, none is started at all
 * while the pattern owns the motor.
 */
static bool haptic_boot_running;

/*
 * A timer, not a work item.
 *
 * Switching the motor off is a single register write and needs nothing a
 * workqueue provides -- and putting it on one made the pulse length depend
 * on what else that queue was doing. The sensor driver samples from the
 * system queue and can wait up to 20ms per report for room in the input
 * queue, twice per report, so a 7ms pulse queued behind it stayed on for
 * closer to fifty. That is what the occasional hard buzz was: not a
 * different pulse, the same one held down.
 *
 * A timer expires from the clock interrupt, so the pulse is the length it
 * was asked for no matter how busy the queue is.
 */
static struct k_timer haptic_off_timer;
static struct k_work_delayable haptic_boot_work;
static struct k_work_delayable haptic_usb_boot_work;
static bool haptic_initialized;


static void haptic_on(void)
{
    nrf_gpio_pin_set(HAPTIC_PIN);
}


static void haptic_off(void)
{
    nrf_gpio_pin_clear(HAPTIC_PIN);
}


static void haptic_off_timer_handler(struct k_timer *timer)
{
    ARG_UNUSED(timer);

    /* Runs in interrupt context; a direct register write is all it does. */
    haptic_off();
}


/*
 * クールダウンを無視して振動させる内部関数。
 *
 * USB接続通知やブート通知など、
 * 必ず1回鳴らしたいシステム通知に使う。
 */
static void haptic_force_pulse_ms(uint32_t duration_ms)
{
    last_haptic_time = k_uptime_get();

    haptic_on();

    k_timer_start(&haptic_off_timer, K_MSEC(duration_ms), K_NO_WAIT);
}


void kobitokey_haptic_pulse_ms(uint32_t duration_ms)
{
    const int64_t now = k_uptime_get();

    if (haptic_boot_running) {
        return;
    }

    if (now - last_haptic_time < HAPTIC_COOLDOWN_MS) {
        return;
    }

    last_haptic_time = now;

    haptic_on();

    k_timer_start(&haptic_off_timer, K_MSEC(duration_ms), K_NO_WAIT);
}


void kobitokey_haptic_pulse(void)
{
    kobitokey_haptic_pulse_ms(HAPTIC_PULSE_MS);
}


bool kobitokey_haptic_quiet_for_ms(uint32_t quiet_ms)
{
    if (last_haptic_time == 0) {
        /* Never pulsed since boot. */
        return true;
    }

    return (k_uptime_get() - last_haptic_time) >= (int64_t)quiet_ms;
}


void kobitokey_haptic_usb_acknowledge(void)
{
    /* Used before normal application threads start on a closed USB boot. */
    nrf_gpio_cfg_output(HAPTIC_PIN);
    haptic_on();
    k_busy_wait(HAPTIC_USB_PULSE_MS * 1000U);
    haptic_off();
    last_haptic_time = k_uptime_get();
}


void kobitokey_haptic_shutdown(void)
{
    haptic_boot_running = false;

    if (haptic_initialized) {
        k_work_cancel_delayable(&haptic_boot_work);
        k_work_cancel_delayable(&haptic_usb_boot_work);
        k_timer_stop(&haptic_off_timer);
    }

    /* Configure explicitly so this is also safe before haptic_init(). */
    nrf_gpio_cfg_output(HAPTIC_PIN);
    haptic_off();
}


/*
 * バッテリー起動時の通常ブート振動。
 *
 * 長い1回目
 * → GAP1
 * → 短い2回目
 * → GAP2
 * → 短い3回目
 */
static void haptic_boot_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

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

    last_haptic_time = k_uptime_get();
    haptic_boot_running = false;
}


/*
 * USB接続状態で起動したときの短い通知。
 *
 * 通常の3連ブート振動の代わりに、
 * 充電開始を示す短い振動を1回だけ行う。
 */
static void haptic_usb_boot_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    haptic_force_pulse_ms(HAPTIC_USB_PULSE_MS);
}


#if defined(CONFIG_KOBITOKEY_VBUS_SENSE)

/*
 * 動作中のVBUS状態変化通知。
 *
 * connected == true:
 *   USBが挿されたので25ms振動
 *
 * connected == false:
 *   USBが抜かれたが、現在は何もしない
 */
static void kobitokey_haptic_vbus_changed(bool connected)
{
    if (!connected) {
        return;
    }

    haptic_force_pulse_ms(HAPTIC_USB_PULSE_MS);
}

#endif


#if DT_HAS_COMPAT_STATUS_OKAY(kobitokey_haptic_local_input)

/*
 * 左手peripheral用。
 *
 * input processorとして挟まず、
 * input callbackでRELイベントを横から見る。
 *
 * これによりtb_left_splitの送信経路を邪魔しない。
 */
static void kobitokey_haptic_local_input_cb(struct input_event *event)
{
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


static int haptic_init(void)
{
    nrf_gpio_cfg_output(HAPTIC_PIN);
    haptic_off();

    k_timer_init(&haptic_off_timer, haptic_off_timer_handler, NULL);

    k_work_init_delayable(
        &haptic_boot_work,
        haptic_boot_work_handler);

    k_work_init_delayable(
        &haptic_usb_boot_work,
        haptic_usb_boot_work_handler);

    haptic_initialized = true;

#if defined(CONFIG_KOBITOKEY_VBUS_SENSE)
    kobitokey_vbus_set_callback(
        kobitokey_haptic_vbus_changed);
#endif

    /* A -> C and B -> D are both opening events: always use three pulses. */
    haptic_boot_running = true;

    k_work_reschedule(
        &haptic_boot_work,
        K_MSEC(HAPTIC_BOOT_DELAY_MS));

    return 0;
}

SYS_INIT(
    haptic_init,
    APPLICATION,
    CONFIG_APPLICATION_INIT_PRIORITY);
