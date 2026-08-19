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
#include <soc/nrfx_coredep.h>

#include "kobitokey_fold.h"
#include "kobitokey_haptic.h"
#include "kobitokey_vbus.h"

LOG_MODULE_REGISTER(kobitokey_fold, LOG_LEVEL_INF);

#define FOLD_NODE DT_NODELABEL(fold_sense)
#define TRACKBALL_NODE DT_NODELABEL(tb_right)

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
 * Set once the buzz for a closed-lid USB insertion has been started by the
 * early hooks below, so kobitokey_fold_init() does not repeat it.
 */
static bool fold_early_usb_ack_done;

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
 * So the buzz now *starts* at PRE_KERNEL_1 priority 0, the first
 * opportunity to run C code at all. nrf_gpio writes registers directly and
 * needs no driver or clock setup, so this works even that early. What it
 * cannot do is wait: k_busy_wait depends on the system timer, which is not
 * up yet. The pin is therefore only raised here and cleared by a second
 * hook once timing is available, making the buzz last as long as the gap
 * between the two.
 *
 * This also serves as a measurement: whatever delay remains after this is
 * bootloader time, which firmware cannot shorten.
 *
 * Pins are read through nrf_gpio rather than their devicetree specs
 * because the vbus and fold initializers have not configured them yet.
 * Both signals are active-high, so a raw read matches the logical level.
 *
 * The LED half stays in kobitokey_fold_init(): it needs a battery reading,
 * and the ADC and sensor driver are not available this early.
 */
#define FOLD_HAPTIC_PIN NRF_GPIO_PIN_MAP(1, 11)

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
        return 0; /* Same USB session as before; stay quiet. */
    }

    /* nrfx_coredep_delay_us is a calibrated CPU spin, so unlike
     * k_busy_wait it does not need the system timer and the whole pulse
     * fits in this one hook. An earlier attempt split it across two hooks
     * and cleared the pin at PRE_KERNEL_2, which stretched the buzz by the
     * entire gap between them -- well over 100ms, felt as an unpleasantly
     * strong jolt rather than a short tick. */
    const uint8_t state = fold_usb_state_get();

    /* Light the remembered colour first so it lands with the buzz rather
     * than after it. Skipped on the very first insertion, when nothing has
     * been cached yet; kobitokey_fold_init() still shows the real level. */
    if ((state & FOLD_USB_FLAG_COLOR_VALID) != 0U) {
        fold_early_leds_set((state & FOLD_USB_COLOR_MASK) >> FOLD_USB_COLOR_SHIFT);
    }

    nrf_gpio_cfg_output(FOLD_HAPTIC_PIN);
    nrf_gpio_pin_set(FOLD_HAPTIC_PIN);
    nrfx_coredep_delay_us(CONFIG_KOBITOKEY_HAPTIC_USB_PULSE_MS * 1000U);
    nrf_gpio_pin_clear(FOLD_HAPTIC_PIN);

    fold_early_usb_ack_done = true;

    return 0;
}

SYS_INIT(kobitokey_fold_early_usb_ack_start, PRE_KERNEL_1, 0);

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

static void fold_power_off(void)
{
    int ret;

#if DT_NODE_HAS_STATUS(TRACKBALL_NODE, okay)
    /*
     * Runtime D/C -> B/A: put PAW3222 into enhanced power-down before GPIO
     * state is retained by System OFF. On a closed boot the delayed PAW3222
     * initializer has not run yet, so device_is_ready() is false and no SPI
     * transaction is attempted.
     */
    const struct device *const trackball = DEVICE_DT_GET(TRACKBALL_NODE);

    if (device_is_ready(trackball)) {
        ret = pm_device_action_run(trackball, PM_DEVICE_ACTION_SUSPEND);
        if (ret < 0 && ret != -EALREADY) {
            LOG_ERR("Failed to suspend trackball: %d", ret);
        }
    }
#endif

    /* Never retain an active motor output across nRF52840 System OFF. */
    kobitokey_haptic_shutdown();

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

/*
 * A closed reading has to survive this many checks, this far apart, before
 * the half is allowed to switch itself off.
 *
 * The debounce alone was not enough. It takes one sample once the line has
 * settled, so anything holding the pin closed for that single moment powers
 * the half off, and this half has a vibration motor a short distance from a
 * magnetic sensor. Every scroll pulse drives an eccentric weight -- a moving
 * magnet in its own right, and a source of enough shake to vary the gap the
 * sensor is reading across. That was switching the left half off mid-use,
 * which looks exactly like the link dropping except that it never comes back
 * on its own.
 *
 * Sampling repeatedly over a window separates the two cases without needing
 * to know which of the motor's two effects is responsible: a real lid stays
 * shut, while a disturbance from a 7ms pulse does not survive being looked
 * at again a few times. Any single open reading abandons the attempt, so the
 * bias is towards staying on -- failing to power off wastes some charge and
 * is obvious, where powering off in error loses the half mid-sentence.
 *
 * The cost is that closing the lid takes about a quarter second longer to
 * take effect, which is not perceptible when the lid is already shut.
 */
#define FOLD_CONFIRM_SAMPLES 5
#define FOLD_CONFIRM_INTERVAL_MS 40

static uint8_t fold_confirm_count;

static void fold_change_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    if (!kobitokey_fold_is_closed()) {
        fold_confirm_count = 0;
        return;
    }

    if (++fold_confirm_count < FOLD_CONFIRM_SAMPLES) {
        /* Rescheduled rather than slept through: this runs on the system
         * workqueue, and stalling that for the length of the window would
         * hold up everything else queued behind it. */
        k_work_reschedule(
            &fold_change_work,
            K_MSEC(FOLD_CONFIRM_INTERVAL_MS));
        return;
    }

    LOG_INF("Fold confirmed closed over %d samples",
            FOLD_CONFIRM_SAMPLES);

    fold_confirm_count = 0;

    if (kobitokey_vbus_is_connected()) {
        /* D -> B: suppress closed-USB feedback on the resulting boot. */
        fold_mark_usb_runtime_close();
    }
    fold_power_off();
}

static void fold_gpio_interrupt_handler(
    const struct device *device,
    struct gpio_callback *callback,
    gpio_port_pins_t pins)
{
    ARG_UNUSED(device);
    ARG_UNUSED(callback);
    ARG_UNUSED(pins);

    /* A fresh edge means the state moved again, so any confirmation in
     * progress is about the previous state and starts over. */
    fold_confirm_count = 0;

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

    if (kobitokey_fold_is_closed()) {
        if (kobitokey_vbus_is_connected()) {
            const bool suppress_closed_usb_feedback =
                (usb_state & (FOLD_USB_FLAG_RUNTIME_CLOSE |
                              FOLD_USB_FLAG_SESSION_NOTIFIED)) != 0U;

            if (!suppress_closed_usb_feedback) {
                /* A -> B: charging feedback before returning to System OFF.
                 * The buzz normally already happened in the PRE_KERNEL_2
                 * hook; only fall back to it here if that did not run. */
                if (!fold_early_usb_ack_done) {
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
