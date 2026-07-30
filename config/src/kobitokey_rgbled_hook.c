#include <stdbool.h>

#include "kobitokey_vbus.h"

/*
 * Override the weak hook provided by the forked
 * zmk-rgbled-widget module.
 *
 * Suppress the initial LED indication only when the keyboard
 * boots with USB VBUS already present.
 */
bool rgbled_widget_skip_boot_indication(void)
{
    return kobitokey_vbus_is_connected();
}