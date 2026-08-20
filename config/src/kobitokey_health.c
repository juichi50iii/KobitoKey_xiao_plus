/*
 * KobitoKey health: say why the last boot happened, and recover from a hang.
 *
 * A half was dying mid-use, staying dead until the lid was worked, and not
 * lighting any of the indicators around the firmware's own power-off paths.
 * Tying the load switch permanently on did not stop it either, so the supply
 * was never being cut: the processor had power the whole time and was simply
 * not running. Working the lid revived it because that path ends in a reset,
 * not because it had anything to do with the lid.
 *
 * Nothing in the build could tell those cases apart, because nothing recorded
 * why a boot happened. The hardware does record it -- RESETREAS survives the
 * reset that set it -- and reading it at startup turns an invisible failure
 * into a colour:
 *
 *   red     watchdog. The firmware stopped feeding it, so it had hung.
 *   green   woke from System OFF, which is the fold path doing its job.
 *   blue    reset pin -- a manual reset or the battery being reconnected.
 *   cyan    software reset, which is what entering the bootloader looks like.
 *   magenta lockup: a fault inside a fault.
 *   yellow  none of the above, so the core lost power. Brownout, a loose
 *           battery connection, or protection tripping.
 *
 * Yellow and red point in opposite directions -- one is the supply, the other
 * is this firmware -- and until now both looked identical from the outside.
 *
 * Showing this at every boot also settles a question the earlier indicators
 * left open. They were only supposed to light on a fault, so their staying
 * dark could always have meant the indicator itself was broken rather than
 * the fault being absent. This one lights on every boot without exception, so
 * if it is working you will know, and its colours can then be believed.
 *
 * The watchdog is the other half. A hung processor here had no way back and
 * sat dead until the lid was worked; with the watchdog it resets itself after
 * a few seconds and reconnects, and leaves red behind to say that it did.
 * Feeding it from the system workqueue is deliberate -- that queue carries
 * the sensor and haptic work, so anything that blocks it long enough to
 * matter also stops the feeding.
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <hal/nrf_gpio.h>
#include <hal/nrf_power.h>

LOG_MODULE_REGISTER(kobitokey_health, LOG_LEVEL_INF);

/* Widget colour mask: bit0 red, bit1 green, bit2 blue. */
#define HEALTH_RED     BIT(0)
#define HEALTH_GREEN   BIT(1)
#define HEALTH_BLUE    BIT(2)
#define HEALTH_YELLOW  (HEALTH_RED | HEALTH_GREEN)
#define HEALTH_MAGENTA (HEALTH_RED | HEALTH_BLUE)
#define HEALTH_CYAN    (HEALTH_GREEN | HEALTH_BLUE)

#define HEALTH_SHOW_MS CONFIG_KOBITOKEY_HEALTH_SHOW_MS

#define HEALTH_LED_PIN(alias) \
    NRF_GPIO_PIN_MAP(0, DT_GPIO_PIN(DT_ALIAS(alias), gpios))

BUILD_ASSERT(DT_SAME_NODE(DT_GPIO_CTLR(DT_ALIAS(led_red), gpios),
                          DT_NODELABEL(gpio0)) &&
                 DT_SAME_NODE(DT_GPIO_CTLR(DT_ALIAS(led_green), gpios),
                              DT_NODELABEL(gpio0)) &&
                 DT_SAME_NODE(DT_GPIO_CTLR(DT_ALIAS(led_blue), gpios),
                              DT_NODELABEL(gpio0)),
             "Health LEDs are assumed to be on gpio0");

static struct k_work_delayable health_show_work;

/*
 * Driven through the registers rather than the LED driver. The widget owns
 * these pins and writes them for battery and connection state; going straight
 * to the port means the last writer takes the pin, and it also removes the
 * device-ready check that an earlier indicator could fail silently on.
 */
static void health_leds_set(uint8_t color)
{
    const uint32_t pins[] = {
        HEALTH_LED_PIN(led_red),
        HEALTH_LED_PIN(led_green),
        HEALTH_LED_PIN(led_blue),
    };

    for (size_t i = 0; i < ARRAY_SIZE(pins); i++) {
        nrf_gpio_cfg_output(pins[i]);

        /* Active low: a lit channel is driven low. */
        if ((color & BIT(i)) != 0U) {
            nrf_gpio_pin_clear(pins[i]);
        } else {
            nrf_gpio_pin_set(pins[i]);
        }
    }
}

static void health_show_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    /* Hand the pins back to the widget. */
    health_leds_set(0U);
}

static uint8_t health_color_for(uint32_t reasons)
{
    if ((reasons & NRF_POWER_RESETREAS_DOG_MASK) != 0U) {
        return HEALTH_RED;
    }

    if ((reasons & NRF_POWER_RESETREAS_LOCKUP_MASK) != 0U) {
        return HEALTH_MAGENTA;
    }

    if ((reasons & NRF_POWER_RESETREAS_OFF_MASK) != 0U) {
        return HEALTH_GREEN;
    }

    if ((reasons & NRF_POWER_RESETREAS_SREQ_MASK) != 0U) {
        return HEALTH_CYAN;
    }

    if ((reasons & NRF_POWER_RESETREAS_RESETPIN_MASK) != 0U) {
        return HEALTH_BLUE;
    }

    /*
     * Every bit clear means the register itself lost its contents, which
     * only happens when the core lost power rather than being reset.
     */
    return HEALTH_YELLOW;
}

#if IS_ENABLED(CONFIG_WATCHDOG)
#define HEALTH_WDT_NODE DT_NODELABEL(wdt0)

static const struct device *const health_wdt = DEVICE_DT_GET(HEALTH_WDT_NODE);
static int health_wdt_channel = -1;

static struct k_work_delayable health_feed_work;

static void health_feed_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    (void)wdt_feed(health_wdt, health_wdt_channel);

    k_work_reschedule(&health_feed_work,
                      K_MSEC(CONFIG_KOBITOKEY_HEALTH_FEED_MS));
}

static void health_watchdog_start(void)
{
    struct wdt_timeout_cfg config = {
        .window = {
            .min = 0U,
            .max = CONFIG_KOBITOKEY_HEALTH_WDT_MS,
        },
        /* No callback: the point is to reset, and a handler that runs on a
         * wedged system is not something to depend on. */
        .callback = NULL,
        .flags = WDT_FLAG_RESET_SOC,
    };

    if (!device_is_ready(health_wdt)) {
        LOG_ERR("Watchdog is not ready; a hang will stay hung");
        return;
    }

    health_wdt_channel = wdt_install_timeout(health_wdt, &config);
    if (health_wdt_channel < 0) {
        LOG_ERR("Failed to install watchdog timeout: %d", health_wdt_channel);
        return;
    }

    /*
     * Left running while the core sleeps. Pausing it there would be kinder to
     * the battery, but this half idles constantly between reports, and a hang
     * that parks the processor in a wait would then never be caught -- which
     * is the failure being chased.
     */
    const int ret = wdt_setup(health_wdt, 0U);

    if (ret < 0) {
        LOG_ERR("Failed to start watchdog: %d", ret);
        health_wdt_channel = -1;
        return;
    }

    k_work_init_delayable(&health_feed_work, health_feed_work_handler);
    k_work_reschedule(&health_feed_work,
                      K_MSEC(CONFIG_KOBITOKEY_HEALTH_FEED_MS));

    LOG_INF("Watchdog armed at %d ms", CONFIG_KOBITOKEY_HEALTH_WDT_MS);
}
#else
static void health_watchdog_start(void) {}
#endif

static int kobitokey_health_init(void)
{
    const uint32_t reasons = nrf_power_resetreas_get(NRF_POWER);

    /* Cleared here so the next boot reports its own cause and not this one;
     * the bits latch until written back. */
    nrf_power_resetreas_clear(NRF_POWER, reasons);

    LOG_INF("Boot cause RESETREAS=0x%08x", reasons);

    health_leds_set(health_color_for(reasons));

    k_work_init_delayable(&health_show_work, health_show_work_handler);
    k_work_reschedule(&health_show_work, K_MSEC(HEALTH_SHOW_MS));

    health_watchdog_start();

    return 0;
}

/*
 * After the LED driver and the fold sense (95), so this runs on a boot that
 * is actually staying awake and does not fight the closed-lid path.
 */
SYS_INIT(kobitokey_health_init, POST_KERNEL, 96);
