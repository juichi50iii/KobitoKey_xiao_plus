#pragma once

#include <stdbool.h>
#include <stdint.h>

void kobitokey_haptic_pulse(void);
void kobitokey_haptic_pulse_ms(uint32_t duration_ms);
void kobitokey_haptic_usb_acknowledge(void);
void kobitokey_haptic_shutdown(void);

/*
 * True when the motor has been left alone for at least quiet_ms.
 *
 * For anything that reads a sensor the motor can disturb. The weight keeps
 * turning well past the end of a drive pulse, so "the pin is low" is not the
 * same as "the board is still", and this answers the second question.
 */
bool kobitokey_haptic_quiet_for_ms(uint32_t quiet_ms);
