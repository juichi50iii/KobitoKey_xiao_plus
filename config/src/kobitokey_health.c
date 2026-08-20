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
 * into something countable:
 *
 *   1 blink  watchdog. The firmware stopped feeding it, so it had hung.
 *   2 blinks no bit set at all, so the core lost power rather than being
 *            reset. Brownout, a loose battery, or protection tripping.
 *   3 blinks woke from System OFF, which is the fold path doing its job.
 *   4 blinks reset pin -- a manual reset or the battery being reconnected.
 *   5 blinks software reset, which is what entering the bootloader looks like.
 *   6 blinks lockup: a fault inside a fault.
 *
 * One and two point in opposite directions -- one is this firmware, the other
 * is the supply -- and until now both looked identical from the outside.
 *
 * It is counted rather than coloured, and shown late rather than at once,
 * because the first attempt was neither. That one painted a colour on these
 * same LEDs for three seconds starting at boot, which is exactly when the
 * widget paints them too, for battery level and then for connection state --
 * in overlapping colours. What came out was a sequence that had to be picked
 * apart by reasoning about which indicator owned which flash, and a reading
 * that needs an argument to defend is not evidence.
 *
 * So this one is separated from the widget on both axes. White is a colour
 * the widget cannot produce here: the mask is bit0 red, bit1 green, bit2
 * blue, and nothing configured on this half asks for all three -- not the
 * battery levels, not the connection states, not any layer colour. And the
 * wait puts the pattern well clear of the widget's boot display in time as
 * well. A count needs no vocabulary to read, only fingers.
 *
 * Showing it on every boot settles the other question the earlier indicators
 * left open. They only lit for the one case they watched for, so staying dark
 * could always have meant a broken indicator rather than an absent fault.
 * This runs unconditionally, so if it works at all, you will see it work.
 *
 * The watchdog is the other half. A hung processor here had no way back and
 * sat dead until the lid was worked; with the watchdog it resets itself after
 * a few seconds and reconnects, and leaves one blink behind to say that it
 * did. Feeding it from the system workqueue is deliberate -- that queue
 * carries the sensor and haptic work, so anything that blocks it long enough
 * to matter also stops the feeding. For the same reason the blink pattern is
 * driven by rescheduling rather than by sleeping through it: an indicator
 * that occupied that queue for several seconds would trip the very watchdog
 * it exists to report on.
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

/*
 * Widget colour mask: bit0 red, bit1 green, bit2 blue. All three is the one
 * combination nothing else on this half asks for, which is what makes the
 * pattern below unmistakably ours.
 */
#define HEALTH_WHITE 0x7U
#define HEALTH_OFF   0x0U

/* Blink counts, in the order the causes are tested for. */
#define HEALTH_CODE_WATCHDOG   1
#define HEALTH_CODE_POWER_LOSS 2
#define HEALTH_CODE_SYSTEM_OFF 3
#define HEALTH_CODE_RESET_PIN  4
#define HEALTH_CODE_SOFT_RESET 5
#define HEALTH_CODE_LOCKUP     6

#define HEALTH_LED_PIN(alias) \
    NRF_GPIO_PIN_MAP(0, DT_GPIO_PIN(DT_ALIAS(alias), gpios))

BUILD_ASSERT(DT_SAME_NODE(DT_GPIO_CTLR(DT_ALIAS(led_red), gpios),
                          DT_NODELABEL(gpio0)) &&
                 DT_SAME_NODE(DT_GPIO_CTLR(DT_ALIAS(led_green), gpios),
                              DT_NODELABEL(gpio0)) &&
                 DT_SAME_NODE(DT_GPIO_CTLR(DT_ALIAS(led_blue), gpios),
                              DT_NODELABEL(gpio0)),
             "Health LEDs are assumed to be on gpio0");

static struct k_work_delayable health_blink_work;

static uint8_t health_code;        /* blinks per round */
static uint8_t health_rounds_left;
static uint8_t health_blinks_left;
static bool health_lit;

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

/*
 * One step of the pattern per call, rescheduling itself for the next one, so
 * the workqueue stays free between steps.
 */
static void health_blink_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    if (health_lit) {
        health_leds_set(HEALTH_OFF);
        health_lit = false;

        if (health_blinks_left > 0U) {
            k_work_reschedule(&health_blink_work,
                              K_MSEC(CONFIG_KOBITOKEY_HEALTH_BLINK_OFF_MS));
            return;
        }

        /*
         * A round has finished. The count for the next one is loaded here,
         * along with taking one off the rounds remaining -- both in the same
         * place, because separating them is what made this repeat forever.
         * The count was reloaded here while the decrement lived in a branch
         * that only ran when the count had reached zero, which reloading it
         * guaranteed it never would.
         */
        if (health_rounds_left > 0U) {
            health_rounds_left--;
            health_blinks_left = health_code;

            k_work_reschedule(&health_blink_work,
                              K_MSEC(CONFIG_KOBITOKEY_HEALTH_GAP_MS));
        }

        /* Otherwise done, and the pins are already back with the widget. */
        return;
    }

    if (health_blinks_left == 0U) {
        /* Nothing left to show. */
        return;
    }

    health_blinks_left--;
    health_leds_set(HEALTH_WHITE);
    health_lit = true;

    k_work_reschedule(&health_blink_work,
                      K_MSEC(CONFIG_KOBITOKEY_HEALTH_BLINK_ON_MS));
}

static uint8_t health_code_for(uint32_t reasons)
{
    if ((reasons & NRF_POWER_RESETREAS_DOG_MASK) != 0U) {
        return HEALTH_CODE_WATCHDOG;
    }

    if ((reasons & NRF_POWER_RESETREAS_LOCKUP_MASK) != 0U) {
        return HEALTH_CODE_LOCKUP;
    }

    if ((reasons & NRF_POWER_RESETREAS_OFF_MASK) != 0U) {
        return HEALTH_CODE_SYSTEM_OFF;
    }

    if ((reasons & NRF_POWER_RESETREAS_SREQ_MASK) != 0U) {
        return HEALTH_CODE_SOFT_RESET;
    }

    if ((reasons & NRF_POWER_RESETREAS_RESETPIN_MASK) != 0U) {
        return HEALTH_CODE_RESET_PIN;
    }

    /*
     * Every bit clear means the register itself lost its contents, which
     * only happens when the core lost power rather than being reset.
     */
    return HEALTH_CODE_POWER_LOSS;
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

    health_code = health_code_for(reasons);

    LOG_INF("Boot cause RESETREAS=0x%08x -> %u blink(s)",
            reasons, health_code);

    /* The first round is loaded here, so the counter holds the repeats. */
    health_rounds_left = CONFIG_KOBITOKEY_HEALTH_ROUNDS - 1;
    health_blinks_left = health_code;
    health_lit = false;

    /*
     * Held off until the widget has finished its own boot display, so the
     * two never share the LEDs at the same moment.
     */
    k_work_init_delayable(&health_blink_work, health_blink_work_handler);
    k_work_reschedule(&health_blink_work,
                      K_MSEC(CONFIG_KOBITOKEY_HEALTH_DELAY_MS));

    health_watchdog_start();

    return 0;
}

/*
 * After the LED driver and the fold sense (95), so this runs on a boot that
 * is actually staying awake and does not fight the closed-lid path.
 */
SYS_INIT(kobitokey_health_init, POST_KERNEL, 96);
