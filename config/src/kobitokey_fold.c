#include <errno.h>
#include <stdbool.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/util.h>

#include <hal/nrf_power.h>
#include <hal/nrf_gpio.h>

#include "kobitokey_fold.h"
#include "kobitokey_haptic.h"
#include "kobitokey_vbus.h"

LOG_MODULE_REGISTER(kobitokey_fold, LOG_LEVEL_INF);

#define FOLD_NODE DT_NODELABEL(fold_sense)
/*
 * Found by what it is rather than by name. This was DT_NODELABEL(tb_right),
 * which exists on one half only -- so everything guarded by it, including
 * powering the sensor down before System OFF, was quietly compiled out of
 * the other half.
 */
#define FOLD_HAS_TRACKBALL \
    (DT_HAS_COMPAT_STATUS_OKAY(pixart_paw3222) && IS_ENABLED(CONFIG_PM_DEVICE))

/* Use GPREGRET2 so Zephyr/bootloader use of GPREGRET1 remains untouched. */
#define FOLD_USB_STATE_REG 1U
#define FOLD_USB_FLAG_RUNTIME_CLOSE BIT(0)
#define FOLD_USB_FLAG_SESSION_NOTIFIED BIT(1)

/*
 * Spare bits of the same retained register cache the battery colour last
 * shown, so a closed-lid USB insertion can light the LED immediately
 * instead of waiting for a real reading. GPREGRET survives System OFF,
 * which is exactly the boundary the reading is needed across.
 *
 * The colour is the widget's 3-bit RGB mask (bit0 red, bit1 green, bit2
 * blue), so it fits in three bits with one more marking it as populated.
 */
#define FOLD_USB_COLOR_SHIFT 2
#define FOLD_USB_COLOR_MASK (0x7U << FOLD_USB_COLOR_SHIFT)
#define FOLD_USB_FLAG_COLOR_VALID BIT(5)


/* Adafruit/Seeed UF2 bootloader: skip DFU checks on a System OFF wake. */
#define FOLD_BOOTLOADER_GPREGRET_REG 0U
#define FOLD_BOOTLOADER_DFU_MAGIC_SKIP 0x6DU

static const struct gpio_dt_spec fold_gpio =
    GPIO_DT_SPEC_GET(FOLD_NODE, gpios);

static struct gpio_callback fold_gpio_callback;
static struct k_work_delayable fold_change_work;

/* XIAO right-side wiring used for the System OFF SENSE configuration. */
#define FOLD_FAST_PIN NRF_GPIO_PIN_MAP(0, 19)
#define VBUS_FAST_PIN NRF_GPIO_PIN_MAP(0, 15)
/* XIAO nRF52840 の充電IC(BQ25101)の CHG#。充電中だけLow(赤い基板上LEDと同じ線)。 */
#define CHG_FAST_PIN NRF_GPIO_PIN_MAP(0, 17)

BUILD_ASSERT(DT_GPIO_PIN(FOLD_NODE, gpios) == 19,
             "Update FOLD_FAST_PIN when the fold GPIO changes");
BUILD_ASSERT(DT_GPIO_PIN(DT_NODELABEL(vbus_sense), gpios) == 15,
             "Update VBUS_FAST_PIN when the VBUS GPIO changes");

static uint8_t fold_usb_state_get(void)
{
    return (uint8_t)nrf_power_gpregret_get(NRF_POWER, FOLD_USB_STATE_REG);
}

/* Replace the session flags, leaving the cached battery colour intact. */
static void fold_usb_state_set(uint8_t flags)
{
    const uint8_t retained =
        fold_usb_state_get() & (FOLD_USB_COLOR_MASK | FOLD_USB_FLAG_COLOR_VALID);

    nrf_power_gpregret_set(NRF_POWER, FOLD_USB_STATE_REG, retained | flags);
}

static void fold_mark_usb_runtime_close(void)
{
    fold_usb_state_set(FOLD_USB_FLAG_RUNTIME_CLOSE);
}

static void fold_cache_battery_color(uint8_t color)
{
    const uint8_t flags =
        fold_usb_state_get() &
        (FOLD_USB_FLAG_RUNTIME_CLOSE | FOLD_USB_FLAG_SESSION_NOTIFIED);

    nrf_power_gpregret_set(
        NRF_POWER, FOLD_USB_STATE_REG,
        flags | FOLD_USB_FLAG_COLOR_VALID |
            ((color << FOLD_USB_COLOR_SHIFT) & FOLD_USB_COLOR_MASK));
}

#if defined(CONFIG_RGBLED_WIDGET) && defined(CONFIG_ZMK_BATTERY_REPORTING) && \
    DT_HAS_CHOSEN(zmk_battery) && DT_HAS_COMPAT_STATUS_OKAY(gpio_leds)
static void fold_show_battery_immediately(void)
{
    const struct device *const battery = DEVICE_DT_GET(DT_CHOSEN(zmk_battery));
    const struct device *const led_dev =
        DEVICE_DT_GET(DT_COMPAT_GET_ANY_STATUS_OKAY(gpio_leds));
    const uint8_t led_indices[] = {
        DT_NODE_CHILD_IDX(DT_ALIAS(led_red)),
        DT_NODE_CHILD_IDX(DT_ALIAS(led_green)),
        DT_NODE_CHILD_IDX(DT_ALIAS(led_blue)),
    };
    struct sensor_value state_of_charge;
    uint8_t battery_level = 0;
    uint8_t color = CONFIG_RGBLED_WIDGET_BATTERY_COLOR_MISSING;
    int ret;

    if (device_is_ready(battery)) {
        ret = sensor_sample_fetch_chan(battery, SENSOR_CHAN_GAUGE_STATE_OF_CHARGE);
        if (ret == 0) {
            ret = sensor_channel_get(battery, SENSOR_CHAN_GAUGE_STATE_OF_CHARGE,
                                     &state_of_charge);
            if (ret == 0) {
                battery_level = CLAMP(state_of_charge.val1, 0, 100);
            }
        }
    }

    if (battery_level >= CONFIG_RGBLED_WIDGET_BATTERY_LEVEL_HIGH) {
        color = CONFIG_RGBLED_WIDGET_BATTERY_COLOR_HIGH;
    } else if (battery_level >= CONFIG_RGBLED_WIDGET_BATTERY_LEVEL_LOW) {
        color = CONFIG_RGBLED_WIDGET_BATTERY_COLOR_MEDIUM;
    } else if (battery_level > 0) {
        color = CONFIG_RGBLED_WIDGET_BATTERY_COLOR_LOW;
    }

    if (!device_is_ready(led_dev)) {
        LOG_ERR("RGB LED device is not ready for fast battery feedback");
        return;
    }

    for (uint8_t pos = 0; pos < ARRAY_SIZE(led_indices); pos++) {
        if ((color & BIT(pos)) != 0) {
            led_on(led_dev, led_indices[pos]);
        } else {
            led_off(led_dev, led_indices[pos]);
        }
    }

    /* Remember it so the next closed-lid insertion can light this colour
     * before the ADC is available. */
    fold_cache_battery_color(color);

    LOG_INF("Fast closed-USB battery feedback: %u%%", battery_level);
    k_busy_wait(CONFIG_RGBLED_WIDGET_BATTERY_BLINK_MS * 1000U);

    for (uint8_t pos = 0; pos < ARRAY_SIZE(led_indices); pos++) {
        led_off(led_dev, led_indices[pos]);
    }
}
#endif

/*
 * Waking from a closed lid is a System OFF wake, which on nRF52840 is a
 * full chip reset: the UF2 bootloader runs, then Zephyr boots, and only
 * then would kobitokey_fold_init() at POST_KERNEL 95 acknowledge the USB
 * insertion. Measuring k_uptime_get() there put the Zephyr half at roughly
 * 61-120ms, but moving the buzz to PRE_KERNEL_2 changed nothing
 * perceptible, which means most of that is spent before PRE_KERNEL_2 runs
 * -- plausibly waiting on the 32.768kHz crystal inside the system clock
 * driver, which initializes at that same level.
 *
 * So the acknowledgement runs at PRE_KERNEL_1 priority 0, the first
 * opportunity to run C code at all. nrf_gpio writes registers directly and
 * needs no driver or clock setup, so this works even that early.
 *
 * Pins are read through nrf_gpio rather than their devicetree specs
 * because the vbus and fold initializers have not configured them yet.
 * Both signals are active-high, so a raw read matches the logical level.
 *
 * ここでやるのは色だけだ。振動は後の kobitokey_fold_init() が鳴らす。
 *
 * 以前はここで P1.11 を出力にしてERMを回していた。新基板ではその
 * P1.11 は DRV2605L の SDA で、ここで叩けば起動のたびにI2Cの
 * データ線を引っ張ることになる。
 *
 * そして DRV2605L はI2C越しにしか鳴らせない。I2Cドライバが動き出すのは
 * POST_KERNEL 50 で、この PRE_KERNEL_1 よりずっと後だ。だから振動は、
 * I2Cが使える最初の地点である POST_KERNEL 95 の蓋判定と同じ場所まで
 * 下ろしてある。色だけがここに残っているのは、色ならこの早さで出せて、
 * 出せるものを遅らせる理由が無いからだ。
 *
 * The battery reading itself stays in kobitokey_fold_init(): the ADC and
 * sensor driver are not available this early, so only the remembered
 * colour can be shown here.
 */

#define FOLD_LED_PIN(alias) NRF_GPIO_PIN_MAP(0, DT_GPIO_PIN(DT_ALIAS(alias), gpios))

BUILD_ASSERT(DT_SAME_NODE(DT_GPIO_CTLR(DT_ALIAS(led_red), gpios),
                          DT_NODELABEL(gpio0)) &&
             DT_SAME_NODE(DT_GPIO_CTLR(DT_ALIAS(led_green), gpios),
                          DT_NODELABEL(gpio0)) &&
             DT_SAME_NODE(DT_GPIO_CTLR(DT_ALIAS(led_blue), gpios),
                          DT_NODELABEL(gpio0)),
             "Early RGB feedback assumes all LED channels are on gpio0");
BUILD_ASSERT((DT_GPIO_FLAGS(DT_ALIAS(led_red), gpios) & GPIO_ACTIVE_LOW) &&
             (DT_GPIO_FLAGS(DT_ALIAS(led_green), gpios) & GPIO_ACTIVE_LOW) &&
             (DT_GPIO_FLAGS(DT_ALIAS(led_blue), gpios) & GPIO_ACTIVE_LOW),
             "Early RGB feedback assumes active-low LED channels");

/*
 * Drive the RGB channels straight from their registers, which works before
 * the LED driver initializes at POST_KERNEL 90. Channels are active low, so
 * a lit channel is driven low. The widget's colour mask is bit0 red, bit1
 * green, bit2 blue -- the same order as the pins below.
 */
static void fold_early_leds_set(uint8_t color)
{
    const uint32_t pins[] = {
        FOLD_LED_PIN(led_red),
        FOLD_LED_PIN(led_green),
        FOLD_LED_PIN(led_blue),
    };

    for (size_t i = 0; i < ARRAY_SIZE(pins); i++) {
        nrf_gpio_cfg_output(pins[i]);

        if ((color & BIT(i)) != 0U) {
            nrf_gpio_pin_clear(pins[i]);
        } else {
            nrf_gpio_pin_set(pins[i]);
        }
    }
}

#if defined(CONFIG_KOBITOKEY_FOLD_CHARGE_LED)
/*
 * CHG# はオープンドレインなので、充電していない間は浮く。内部プルアップで
 * Highに落ち着かせてから読む。
 */
static bool fold_charging(void)
{
    nrf_gpio_cfg_input(CHG_FAST_PIN, NRF_GPIO_PIN_PULLUP);
    k_busy_wait(50);

    return nrf_gpio_pin_read(CHG_FAST_PIN) == 0;
}

/*
 * 閉じてUSBに繋いでいる間の充電表示。点けた色は fold_power_off() が消さずに
 * 眠るので、System OFF中も出続ける。
 */
static void fold_show_charge_state(void)
{
    fold_early_leds_set(fold_charging() ? CONFIG_KOBITOKEY_FOLD_CHARGE_COLOR
                                        : CONFIG_KOBITOKEY_FOLD_CHARGED_COLOR);
}
#endif

static int kobitokey_fold_early_usb_ack_start(void)
{
    nrf_gpio_cfg_input(FOLD_FAST_PIN, NRF_GPIO_PIN_NOPULL);
    nrf_gpio_cfg_input(VBUS_FAST_PIN, NRF_GPIO_PIN_NOPULL);

    if (nrf_gpio_pin_read(FOLD_FAST_PIN) == 0) {
        return 0; /* Open: normal boot, nothing to acknowledge. */
    }

    if (nrf_gpio_pin_read(VBUS_FAST_PIN) == 0) {
        return 0; /* Closed on battery: no USB to acknowledge. */
    }

    if ((fold_usb_state_get() & (FOLD_USB_FLAG_RUNTIME_CLOSE |
                                 FOLD_USB_FLAG_SESSION_NOTIFIED)) != 0U) {
#if defined(CONFIG_KOBITOKEY_FOLD_CHARGE_LED)
        /* リセットでLEDのGPIOは初期化される。CHG#の変化で起きた場合に
         * 消えたまま眠り直さないよう、起きてすぐ点け直す。 */
        fold_show_charge_state();
#endif
        return 0; /* Same USB session as before; stay quiet. */
    }

    const uint8_t state = fold_usb_state_get();

    /* Show the remembered level. Skipped on the very first insertion, when
     * nothing has been cached yet; kobitokey_fold_init() still shows the
     * real level. */
    if ((state & FOLD_USB_FLAG_COLOR_VALID) != 0U) {
        fold_early_leds_set((state & FOLD_USB_COLOR_MASK) >> FOLD_USB_COLOR_SHIFT);
    }

    return 0;
}

SYS_INIT(kobitokey_fold_early_usb_ack_start, PRE_KERNEL_1, 0);

/*
 * Set once the closed-lid USB buzz has been played, so
 * kobitokey_fold_init() does not play it a second time.
 */
static bool fold_early_usb_buzz_done;

/*
 * 閉じたままUSBを挿した起動で、振動を鳴らせる最初の地点。
 *
 * 色は PRE_KERNEL_1 で出せるが、振動は DRV2605L に話しかけないと鳴らず、
 * その相手は I2C の先にいる。だからこの通知には「I2Cドライバが立ち上がる
 * POST_KERNEL 50 より前には鳴らせない」という物理的な下限がある。
 * ここはその下限の直後だ。
 *
 * 蓋の判定を待たない理由：kobitokey_fold_init() は POST_KERNEL 95 で、
 * しかも5回の読み取りに100msかける。通知としては、それだけ遅らせる価値が
 * ない。ここで一度読んで閉じていれば鳴らす。
 *
 * 万一その読みが外れていた場合に起きるのは「開いているのに一度鳴った」
 * だけで、電源は落ちない。落とすかどうかの判断は今までどおり
 * kobitokey_fold_init() が5回読んでから決める。鳴らす判断と切る判断で
 * 慎重さの度合いを変えているのは、外したときの代償が違うからだ。
 *
 * ピンは PRE_KERNEL_1 のフックが入力に設定済みだが、あちらが将来
 * 変わっても困らないよう、ここでも設定してから読む。
 */
static int kobitokey_fold_early_usb_buzz(void)
{
    nrf_gpio_cfg_input(FOLD_FAST_PIN, NRF_GPIO_PIN_NOPULL);
    nrf_gpio_cfg_input(VBUS_FAST_PIN, NRF_GPIO_PIN_NOPULL);

    if (nrf_gpio_pin_read(FOLD_FAST_PIN) == 0) {
        return 0; /* Open: this is a normal boot. */
    }

    if (nrf_gpio_pin_read(VBUS_FAST_PIN) == 0) {
        return 0; /* Closed on battery: nothing to acknowledge. */
    }

    if ((fold_usb_state_get() & (FOLD_USB_FLAG_RUNTIME_CLOSE |
                                 FOLD_USB_FLAG_SESSION_NOTIFIED)) != 0U) {
        return 0; /* Same USB session as before; stay quiet. */
    }

    kobitokey_haptic_usb_acknowledge();
    fold_early_usb_buzz_done = true;

    return 0;
}

/*
 * POST_KERNEL 60: I2C(50) と振動の初期化(55) の後、電池もLEDも要らない
 * ので、それらを待つ理由はない。
 */
SYS_INIT(kobitokey_fold_early_usb_buzz, POST_KERNEL, 60);

bool kobitokey_fold_is_closed(void)
{
    const int value = gpio_pin_get_dt(&fold_gpio);

    if (value < 0) {
        LOG_ERR("Failed to read fold sense GPIO: %d", value);
        return false;
    }

    /* Q2 inverts MAG_ON: the level-shifted signal is HIGH when closed. */
    return value != 0;
}

#if FOLD_HAS_TRACKBALL
static bool fold_trackball_suspended;

/*
 * Stop the ball reporting the moment the lid starts to close, before the
 * reading has been confirmed.
 *
 * Folding the keyboard rolls the ball, and the ball scrolls on every layer,
 * so the screen kept moving through the whole confirmation window and for as
 * long as the closing took. Suspending on the first closed reading cuts that
 * off at once: the driver drops its interrupt and puts the sensor into
 * power-down, so nothing is reported and nothing is buzzed.
 *
 * It is reversed if the reading does not hold, which costs one SPI exchange
 * on a false alarm and keeps the confirmation honest -- the point of
 * sampling several times is that a single reading may be wrong, and this
 * must not quietly become a one-reading decision to stop tracking.
 */
static void fold_trackball_suspend(bool suspend)
{
    const struct device *const trackball = DEVICE_DT_GET_ANY(pixart_paw3222);

    if (suspend == fold_trackball_suspended) {
        return;
    }

    if (trackball == NULL || !device_is_ready(trackball)) {
        return;
    }

    const int ret = pm_device_action_run(
        trackball,
        suspend ? PM_DEVICE_ACTION_SUSPEND : PM_DEVICE_ACTION_RESUME);

    if (ret < 0 && ret != -EALREADY) {
        LOG_ERR("Failed to %s trackball: %d", suspend ? "suspend" : "resume", ret);
        return;
    }

    fold_trackball_suspended = suspend;
}
#else
static inline void fold_trackball_suspend(bool suspend) { ARG_UNUSED(suspend); }
#endif

static void fold_power_off(void)
{
    int ret;

    /*
     * Runtime D/C -> B/A: put PAW3222 into enhanced power-down before GPIO
     * state is retained by System OFF. Usually already done by the check
     * above; this covers the paths that reach here without it. On a closed
     * boot the delayed PAW3222 initializer has not run yet, so the device is
     * not ready and no SPI transaction is attempted.
     */
    fold_trackball_suspend(true);

    /* Never retain an active motor output across nRF52840 System OFF. */
    kobitokey_haptic_shutdown();

#if defined(CONFIG_KOBITOKEY_FOLD_CHARGE_LED)
    if (kobitokey_vbus_is_connected()) {
        /* USB給電中は消さず、充電状態の色を点けたまま眠る。 */
        fold_show_charge_state();
    } else
#endif
    {
#if DT_HAS_COMPAT_STATUS_OKAY(gpio_leds)
    /* Force all three XIAO RGB channels off before GPIO state is retained. */
    const struct device *const led_dev =
        DEVICE_DT_GET(DT_COMPAT_GET_ANY_STATUS_OKAY(gpio_leds));

    if (device_is_ready(led_dev)) {
        led_off(led_dev, DT_NODE_CHILD_IDX(DT_ALIAS(led_red)));
        led_off(led_dev, DT_NODE_CHILD_IDX(DT_ALIAS(led_green)));
        led_off(led_dev, DT_NODE_CHILD_IDX(DT_ALIAS(led_blue)));
    }
#endif
    }

    /*
     * The lid is currently closed (HIGH). Arm a LOW-level wake source before
     * entering System OFF so opening the keyboard wakes the nRF52840.
     */
    ret = gpio_pin_interrupt_configure_dt(&fold_gpio, GPIO_INT_LEVEL_LOW);
    if (ret < 0) {
        LOG_ERR("Failed to arm fold wake source: %d", ret);
        return;
    }

    /* Also wake when VBUS changes so a real unplug clears the USB session. */
    ret = kobitokey_vbus_arm_change_wake();
    if (ret < 0) {
        LOG_ERR("Failed to arm VBUS wake source: %d", ret);
        return;
    }

    /*
     * Program the nRF GPIO SENSE fields explicitly as the final step before
     * System OFF. Opening drives FOLD low. VBUS must wake on the opposite of
     * its current level: high for insertion, low for a genuine unplug.
     */
    nrf_gpio_cfg_sense_input(FOLD_FAST_PIN,
                             NRF_GPIO_PIN_NOPULL,
                             NRF_GPIO_PIN_SENSE_LOW);
    nrf_gpio_cfg_sense_input(
        VBUS_FAST_PIN,
        NRF_GPIO_PIN_NOPULL,
        kobitokey_vbus_is_connected() ? NRF_GPIO_PIN_SENSE_LOW
                                      : NRF_GPIO_PIN_SENSE_HIGH);

#if defined(CONFIG_KOBITOKEY_FOLD_CHARGE_LED)
    if (kobitokey_vbus_is_connected()) {
        /* 充電が終わる(CHG#が変わる)と一度起きて、色を替えて眠り直す。 */
        nrf_gpio_cfg_sense_input(
            CHG_FAST_PIN,
            NRF_GPIO_PIN_PULLUP,
            fold_charging() ? NRF_GPIO_PIN_SENSE_HIGH : NRF_GPIO_PIN_SENSE_LOW);
    }
#endif

    LOG_INF("Keyboard closed; entering System OFF");

    /*
     * Opening from System OFF resets through the XIAO UF2 bootloader. Tell it
     * this is an intentional low-power wake so it jumps to ZMK immediately.
     * The bootloader consumes and clears this value.
     */
    nrf_power_gpregret_set(
        NRF_POWER,
        FOLD_BOOTLOADER_GPREGRET_REG,
        FOLD_BOOTLOADER_DFU_MAGIC_SKIP);

    sys_poweroff();

    CODE_UNREACHABLE;
}



static void fold_change_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    if (!kobitokey_fold_is_closed()) {
        /* Not shut after all: let the ball work again. */
        fold_trackball_suspend(false);
        return;
    }

    /*
     * Acted on straight away. This used to take five readings 25ms apart
     * before it would believe the lid, which was added on the theory that a
     * bad reading was switching the half off during use. That was wrong --
     * the half was dying because of the sensor driver -- and the delay was
     * long enough to feel, since the ball scrolls and the keyboard was still
     * awake while it ran.
     *
     * What is left guarding against a single bad reading is the debounce on
     * the interrupt, which is what it is for. If a spurious close ever does
     * get through, it powers the half off until the lid is worked again, so
     * this is the first place to look if that starts happening.
     */
    fold_trackball_suspend(true);

    LOG_INF("Fold read closed; powering off");

    if (kobitokey_vbus_is_connected()) {
        /* D -> B: suppress closed-USB feedback on the resulting boot. */
        fold_mark_usb_runtime_close();
    }
    fold_power_off();
}

/*
 * Only the boot check samples more than once; the runtime one acts on the
 * first reading. The difference is what a wrong answer costs. During use a
 * bad reading means a power-off the user can undo by working the lid. At
 * boot it means the half switches straight back off, which looks exactly
 * like it never woke up -- and it costs nothing to guard against, since an
 * open lid leaves on the first sample without waiting at all.
 */
#define FOLD_CONFIRM_SAMPLES 5
#define FOLD_CONFIRM_INTERVAL_MS 25

/*
 * The boot-time equivalent of the confirmation the runtime path does.
 *
 * A single reading was enough to switch the half off for good, with no
 * second look and nothing written down. Any reason the line is briefly high
 * as the half comes up -- a supply still settling, an input with no bias on
 * it -- reads exactly like a shut lid, and the half goes straight back to
 * sleep on the strength of one sample.
 *
 * Busy-waited rather than slept: this runs during system initialisation,
 * and the wait is short and happens only when the first reading says closed,
 * which is either a real close about to power off anyway or the fault being
 * chased.
 */
static bool fold_closed_confirmed_at_boot(void)
{
    for (uint8_t i = 0; i < FOLD_CONFIRM_SAMPLES; i++) {
        if (!kobitokey_fold_is_closed()) {
            LOG_INF("Boot fold reading did not hold; staying awake");
            return false;
        }

        if (i + 1 < FOLD_CONFIRM_SAMPLES) {
            k_busy_wait(FOLD_CONFIRM_INTERVAL_MS * 1000U);
        }
    }

    return true;
}

static void fold_gpio_interrupt_handler(
    const struct device *device,
    struct gpio_callback *callback,
    gpio_port_pins_t pins)
{
    ARG_UNUSED(device);
    ARG_UNUSED(callback);
    ARG_UNUSED(pins);

    k_work_reschedule(
        &fold_change_work,
        K_MSEC(CONFIG_KOBITOKEY_FOLD_DEBOUNCE_MS));
}

static int kobitokey_fold_init(void)
{
    int ret;
    uint8_t usb_state;

    if (!gpio_is_ready_dt(&fold_gpio)) {
        LOG_ERR("Fold sense GPIO controller is not ready");
        return -ENODEV;
    }

    ret = gpio_pin_configure_dt(&fold_gpio, GPIO_INPUT);
    if (ret < 0) {
        LOG_ERR("Failed to configure fold sense GPIO: %d", ret);
        return ret;
    }

    k_work_init_delayable(&fold_change_work, fold_change_work_handler);

    gpio_init_callback(
        &fold_gpio_callback,
        fold_gpio_interrupt_handler,
        BIT(fold_gpio.pin));

    ret = gpio_add_callback(fold_gpio.port, &fold_gpio_callback);
    if (ret < 0) {
        LOG_ERR("Failed to add fold sense callback: %d", ret);
        return ret;
    }

    ret = gpio_pin_interrupt_configure_dt(&fold_gpio, GPIO_INT_EDGE_BOTH);
    if (ret < 0) {
        LOG_ERR("Failed to configure fold sense interrupt: %d", ret);
        gpio_remove_callback(fold_gpio.port, &fold_gpio_callback);
        return ret;
    }

    usb_state = fold_usb_state_get();

    if (fold_closed_confirmed_at_boot()) {
        if (kobitokey_vbus_is_connected()) {
            const bool suppress_closed_usb_feedback =
                (usb_state & (FOLD_USB_FLAG_RUNTIME_CLOSE |
                              FOLD_USB_FLAG_SESSION_NOTIFIED)) != 0U;

            if (!suppress_closed_usb_feedback) {
                /*
                 * A -> B: charging feedback before returning to System OFF.
                 *
                 * The buzz happens here rather than in the PRE_KERNEL_1
                 * hook because the DRV2605L can only be reached over I2C,
                 * and the I2C driver initialises at POST_KERNEL 50 -- ahead
                 * of this, but well behind that hook. It is late enough to
                 * work and early enough to matter: nothing has powered off
                 * yet, and the battery display below spends most of a
                 * second anyway.
                 *
                 * kobitokey_haptic_usb_acknowledge() brings the chip up on
                 * the spot and waits for the effect to finish, so the power
                 * is not cut out from under a buzz that just started.
                 *
                 * Normally the POST_KERNEL 60 hook has already done it,
                 * some 100ms earlier; this is the fallback for the case
                 * where the lid only read closed on the confirmed sample.
                 */
                if (!fold_early_usb_buzz_done) {
                    kobitokey_haptic_usb_acknowledge();
                }
#if defined(CONFIG_RGBLED_WIDGET) && defined(CONFIG_ZMK_BATTERY_REPORTING) && \
    DT_HAS_CHOSEN(zmk_battery) && DT_HAS_COMPAT_STATUS_OKAY(gpio_leds)
                fold_show_battery_immediately();
#endif
            } else {
                LOG_INF("Suppressing repeated closed-USB feedback");
            }

            /* Keep suppressing resets/bounce until a real VBUS-low boot. */
            fold_usb_state_set(FOLD_USB_FLAG_SESSION_NOTIFIED);
            fold_power_off();
        } else {
            /* B -> A: a genuine unplug ends the notification session. */
            fold_usb_state_set(0U);
            /* The Hall sensor is battery-powered and already stable. */
            fold_power_off();
        }
    } else {
        /* Opening starts normal operation; no closed-USB session remains. */
        fold_usb_state_set(0U);
        LOG_INF("Keyboard open at boot");
    }

    return 0;
}

/*
 * Battery and LED devices initialize at POST_KERNEL priority 90. Run directly
 * after them and VBUS sense (94), before BLE/USB/ZMK application services.
 */
SYS_INIT(kobitokey_fold_init, POST_KERNEL, 95);
