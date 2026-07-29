#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <kobitokey_vbus.h>

LOG_MODULE_REGISTER(kobitokey_vbus, LOG_LEVEL_INF);

#define VBUS_NODE DT_NODELABEL(vbus_sense)

#if !DT_NODE_HAS_STATUS(VBUS_NODE, okay)
#error "VBUS sense node 'vbus_sense' is missing or disabled"
#endif

static const struct gpio_dt_spec vbus_gpio =
    GPIO_DT_SPEC_GET(VBUS_NODE, gpios);

bool kobitokey_vbus_is_connected(void)
{
    const int value = gpio_pin_get_dt(&vbus_gpio);

    if (value < 0) {
        LOG_ERR("Failed to read VBUS sense GPIO: %d", value);
        return false;
    }

    /*
     * gpio_pin_get_dt() applies GPIO_ACTIVE_HIGH/LOW,
     * so a non-zero result means VBUS is present.
     */
    return value != 0;
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

    if (IS_ENABLED(CONFIG_KOBITOKEY_VBUS_LOG_BOOT_STATE)) {
        LOG_INF("VBUS %s",
                kobitokey_vbus_is_connected()
                    ? "detected"
                    : "not detected");
    }

    return 0;
}

SYS_INIT(kobitokey_vbus_init, APPLICATION, 90);