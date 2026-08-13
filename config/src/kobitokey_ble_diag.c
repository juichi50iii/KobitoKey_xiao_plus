#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/logging/log.h>

#include "kobitokey_haptic.h"

LOG_MODULE_REGISTER(kobitokey_ble_diag, CONFIG_ZMK_LOG_LEVEL);

/*
 * Temporary diagnostic build: there is no serial console available, so this
 * reports the negotiated BLE connection interval via haptic pulses instead.
 * Fires 1.5s after the interval settles (each new param-update event
 * reschedules it), so it reflects the interval actually used once the link
 * is idle, not a fleeting intermediate value from connection setup.
 *
 * Pulse count buckets (interval = raw units * 1.25ms):
 *   1 pulse  : <= 10ms  (close to the requested 7.5ms)
 *   2 pulses : 10-20ms
 *   3 pulses : 20-40ms
 *   4 pulses : > 40ms
 */

#define DIAG_PULSE_MS 60
#define DIAG_GAP_MS 250
#define DIAG_START_DELAY_MS 1500

static struct k_work_delayable diag_work;
static uint16_t last_interval;

static void diag_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    uint32_t interval_ms = (uint32_t)last_interval * 5 / 4;
    int pulses;

    if (interval_ms <= 10) {
        pulses = 1;
    } else if (interval_ms <= 20) {
        pulses = 2;
    } else if (interval_ms <= 40) {
        pulses = 3;
    } else {
        pulses = 4;
    }

    LOG_INF("BLE conn interval: %u ms (%d pulses)", interval_ms, pulses);

    for (int i = 0; i < pulses; i++) {
        kobitokey_haptic_pulse_ms(DIAG_PULSE_MS);
        k_sleep(K_MSEC(DIAG_PULSE_MS + DIAG_GAP_MS));
    }
}

static void diag_le_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency,
                                   uint16_t timeout)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(latency);
    ARG_UNUSED(timeout);

    last_interval = interval;
    k_work_reschedule(&diag_work, K_MSEC(DIAG_START_DELAY_MS));
}

BT_CONN_CB_DEFINE(kobitokey_ble_diag_conn_callbacks) = {
    .le_param_updated = diag_le_param_updated,
};

static int kobitokey_ble_diag_init(void)
{
    k_work_init_delayable(&diag_work, diag_work_handler);
    return 0;
}

SYS_INIT(kobitokey_ble_diag_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
