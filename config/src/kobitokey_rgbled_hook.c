#include <stdbool.h>

#include <zephyr/kernel.h>

#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/split/bluetooth/peripheral.h>

#if defined(CONFIG_KOBITOKEY_FOLD_SENSE)
#include "kobitokey_fold.h"
#endif

/*
 * Override the weak hook provided by the forked zmk-rgbled-widget module.
 *
 * The widget shows connectivity twice over: once from its init thread, and
 * again whenever the state changes afterwards. Booting therefore gave green
 * for the battery, then red, then blue -- and the red was only ever saying
 * "not connected yet", about a second before it connected. It reads as
 * something having gone wrong on a keyboard that is working perfectly, and
 * the answer arrives before there is time to wonder about it. That is worse
 * than saying nothing.
 *
 * So the boot indication now runs only when it has something worth showing.
 * If the link is already up by the time the widget gets there, it blinks
 * blue as before. If it is not, the boot indication is skipped and the blue
 * arrives on its own when the connection completes, from the state-change
 * path the widget already has.
 *
 * The check has to be here rather than a flat "skip it": simply never
 * showing it would leave a half that connected early with no indication at
 * all, since there would be no later change for the widget to report.
 *
 * Closed lid: still suppressed outright, since the fold handler is on its
 * way to powering down and nothing should be lit on the way out.
 */
bool rgbled_widget_show_initial_connectivity(void)
{
#if defined(CONFIG_KOBITOKEY_FOLD_SENSE)
    if (kobitokey_fold_is_closed()) {
        return false;
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    /* Peripheral: the only link it has is the one to the other half. */
    return zmk_split_bt_peripheral_is_connected();
#else
    /* Central: USB counts as connected, and the widget shows it in its own
     * colour, so let it speak for itself. */
#if IS_ENABLED(CONFIG_RGBLED_WIDGET_CONN_SHOW_USB)
    if (zmk_endpoints_selected().transport == ZMK_TRANSPORT_USB) {
        return true;
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_BLE)
    return zmk_ble_active_profile_is_connected();
#else
    return true;
#endif
#endif
}
