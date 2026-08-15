#include <stdbool.h>

#if defined(CONFIG_KOBITOKEY_FOLD_SENSE)
#include "kobitokey_fold.h"
#endif

/*
 * Override the weak hook provided by the forked
 * zmk-rgbled-widget module.
 *
 * Open:
 *   Show the initial Bluetooth/connectivity indication as usual, including
 *   when USB was already connected before the keyboard was opened.
 *
 * Closed:
 *   Suppress the connectivity indication while the fold handler powers down.
 */
bool rgbled_widget_show_initial_connectivity(void)
{
#if defined(CONFIG_KOBITOKEY_FOLD_SENSE)
    return !kobitokey_fold_is_closed();
#else
    return true;
#endif
}
