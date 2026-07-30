#include <stdbool.h>

#include "kobitokey_vbus.h"

/*
 * Override the weak hook provided by the forked
 * zmk-rgbled-widget module.
 *
 * Battery boot:
 *   Show the initial Bluetooth/connectivity indication as usual.
 *
 * USB boot:
 *   Skip the initial connectivity indication.
 *   The initial battery-level indication is still shown.
 */
bool rgbled_widget_show_initial_connectivity(void)
{
    return !kobitokey_vbus_is_connected();
}