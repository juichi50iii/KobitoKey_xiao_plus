#include <stdbool.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include <dt-bindings/zmk/hid_usage.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk_rgbled_widget/widget.h>

#include "kobitokey_vbus.h"

/*
 * This is latched only at boot. A later USB connection must not hide the
 * layer color when the keyboard was already running from its battery.
 */
static atomic_t usb_only_boot;
static struct k_work enter_normal_mode_work;

static void enter_normal_mode_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    indicate_connectivity();
    rgbled_widget_refresh_layer_color();
}

static void request_normal_mode(void)
{
    if (atomic_cas(&usb_only_boot, 1, 0)) {
        k_work_submit(&enter_normal_mode_work);
    }
}

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
    return atomic_get(&usb_only_boot) == 0;
}

bool rgbled_widget_show_layer_indicators(void)
{
    return atomic_get(&usb_only_boot) == 0;
}

static int kobitokey_kana_combo_unlock(const zmk_event_t *event)
{
    const struct zmk_keycode_state_changed *keycode =
        as_zmk_keycode_state_changed(event);

    if (keycode != NULL && keycode->state &&
        keycode->usage_page == HID_USAGE_KEY &&
        keycode->keycode == HID_USAGE_KEY_KEYBOARD_LANG1) {
        request_normal_mode();
    }

    return 0;
}

ZMK_LISTENER(kobitokey_kana_combo_listener, kobitokey_kana_combo_unlock);
ZMK_SUBSCRIPTION(kobitokey_kana_combo_listener, zmk_keycode_state_changed);

static void kobitokey_rgbled_vbus_changed(bool connected)
{
    if (connected) {
        indicate_battery();
    } else if (atomic_get(&usb_only_boot) != 0) {
        /* If the board keeps running after a USB-powered boot, transition to
         * the normal battery-powered layer indication mode. */
        request_normal_mode();
    }
}

static int kobitokey_rgbled_hook_init(void)
{
    atomic_set(&usb_only_boot, kobitokey_vbus_is_connected() ? 1 : 0);
    k_work_init(&enter_normal_mode_work, enter_normal_mode_work_handler);
    kobitokey_vbus_set_callback(kobitokey_rgbled_vbus_changed);
    return 0;
}

SYS_INIT(kobitokey_rgbled_hook_init, APPLICATION, 90);
