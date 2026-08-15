#pragma once

#include <stdint.h>

void kobitokey_haptic_pulse(void);
void kobitokey_haptic_pulse_ms(uint32_t duration_ms);
void kobitokey_haptic_usb_acknowledge(void);
void kobitokey_haptic_shutdown(void);
