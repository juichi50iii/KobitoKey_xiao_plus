#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback invoked when the detected VBUS state changes.
 *
 * This callback runs from the Zephyr system workqueue, not directly
 * from the GPIO interrupt handler.
 *
 * @param connected true when USB VBUS is present.
 */
typedef void (*kobitokey_vbus_callback_t)(bool connected);

/**
 * @brief Check whether USB VBUS is currently present.
 *
 * @return true if VBUS_SENSE is active.
 * @return false if VBUS_SENSE is inactive or unavailable.
 */
bool kobitokey_vbus_is_connected(void);

/**
 * @brief Register a callback for VBUS state changes.
 *
 * Only one callback is currently supported. Registering another callback
 * replaces the previous one.
 *
 * @param callback Callback function, or NULL to unregister.
 */
void kobitokey_vbus_set_callback(kobitokey_vbus_callback_t callback);

/**
 * @brief Arm the opposite VBUS level as a System OFF wake source.
 *
 * Connected wakes on disconnect; disconnected wakes on connect.
 */
int kobitokey_vbus_arm_change_wake(void);

#ifdef __cplusplus
}
#endif
