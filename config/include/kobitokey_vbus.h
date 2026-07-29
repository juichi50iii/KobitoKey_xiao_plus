#pragma once

#include <stdbool.h>

/**
 * @brief Check whether USB VBUS is present.
 *
 * @return true if VBUS_SENSE is HIGH.
 * @return false if VBUS_SENSE is LOW or could not be read.
 */
bool kobitokey_vbus_is_connected(void);