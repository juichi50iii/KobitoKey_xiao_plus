#include <errno.h>
#include <stdbool.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <kobitokey_vbus.h>

LOG_MODULE_REGISTER(kobitokey_vbus, LOG_LEVEL_INF);

#define VBUS_NODE DT_NODELABEL(vbus_sense)

#if DT_NODE_HAS_STATUS(VBUS_NODE, okay)

static const struct gpio_dt_spec vbus_gpio =
    GPIO_DT_SPEC_GET(VBUS_NODE, gpios);

static struct gpio_callback vbus_gpio_callback;
static struct k_work_delayable vbus_change_work;

static bool vbus_initialized;
static bool previous_vbus_state;

static kobitokey_vbus_callback_t state_change_callback;

bool kobitokey_vbus_is_connected(void)
{
    const int value = gpio_pin_get_dt(&vbus_gpio);

    if (value < 0) {
        LOG_ERR("Failed to read VBUS sense GPIO: %d", value);
        return false;
    }

    /*
     * gpio_pin_get_dt() applies the GPIO_ACTIVE_HIGH/LOW flag.
     * A non-zero logical value therefore means VBUS is present.
     */
    return value != 0;
}

void kobitokey_vbus_set_callback(kobitokey_vbus_callback_t callback)
{
    state_change_callback = callback;
}

static void vbus_change_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    const bool current_vbus_state = kobitokey_vbus_is_connected();

    if (!vbus_initialized) {
        previous_vbus_state = current_vbus_state;
        vbus_initialized = true;
        return;
    }

    if (current_vbus_state == previous_vbus_state) {
        return;
    }

    previous_vbus_state = current_vbus_state;

    if (current_vbus_state) {
        LOG_INF("VBUS connected");
    } else {
        LOG_INF("VBUS disconnected");
    }

    if (state_change_callback != NULL) {
        state_change_callback(current_vbus_state);
    }
}

static void vbus_gpio_interrupt_handler(
    const struct device *device,
    struct gpio_callback *callback,
    gpio_port_pins_t pins)
{
    ARG_UNUSED(device);
    ARG_UNUSED(callback);
    ARG_UNUSED(pins);

    /*
     * Do not perform logging or haptic operations directly in interrupt
     * context. Reschedule the delayable work each time an edge occurs.
     */
    k_work_reschedule(
        &vbus_change_work,
        K_MSEC(CONFIG_KOBITOKEY_VBUS_DEBOUNCE_MS));
}

static int kobitokey_vbus_init(void)
{
    int ret;

    if (!gpio_is_ready_dt(&vbus_gpio)) {
        LOG_ERR("VBUS sense GPIO controller is not ready");
        return -ENODEV;
    }

    ret = gpio_pin_configure_dt(&vbus_gpio, GPIO_INPUT);
    if (ret < 0) {
        LOG_ERR("Failed to configure VBUS sense GPIO: %d", ret);
        return ret;
    }

    previous_vbus_state = kobitokey_vbus_is_connected();
    vbus_initialized = true;

#if defined(CONFIG_KOBITOKEY_VBUS_LOG_BOOT_STATE)
    LOG_INF(
        "VBUS %s at boot",
        previous_vbus_state ? "detected" : "not detected");
#endif

    k_work_init_delayable(
        &vbus_change_work,
        vbus_change_work_handler);

    gpio_init_callback(
        &vbus_gpio_callback,
        vbus_gpio_interrupt_handler,
        BIT(vbus_gpio.pin));

    ret = gpio_add_callback(vbus_gpio.port, &vbus_gpio_callback);
    if (ret < 0) {
        LOG_ERR("Failed to add VBUS GPIO callback: %d", ret);
        return ret;
    }

    ret = gpio_pin_interrupt_configure_dt(
        &vbus_gpio,
        GPIO_INT_EDGE_BOTH);

    if (ret < 0) {
        LOG_ERR("Failed to configure VBUS GPIO interrupt: %d", ret);
        gpio_remove_callback(vbus_gpio.port, &vbus_gpio_callback);
        return ret;
    }

    LOG_INF("VBUS change detection initialized");

    return 0;
}

SYS_INIT(kobitokey_vbus_init, APPLICATION, 80);

#else

/*
 * The left side currently has no VBUS sense GPIO.
 * Keep the shared public API available so common code can still link.
 */

bool kobitokey_vbus_is_connected(void)
{
    return false;
}

void kobitokey_vbus_set_callback(kobitokey_vbus_callback_t callback)
{
    ARG_UNUSED(callback);
}

#endif