#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * ひと刻み。動いた量を渡す形が本命で、渡された量から転がりの速さを
 * 起こし、速いほど刻みの間隔を詰める。量の分からない呼び出しのために
 * 引数なしの版も残してあるが、そちらは速さを測る材料にはならない。
 */
void kobitokey_haptic_pulse_rel(int32_t value);
void kobitokey_haptic_pulse(void);
void kobitokey_haptic_pulse_ms(uint32_t duration_ms);

/*
 * 指定のエフェクトをひとつ。転がしの速さとは関係ない知らせ用。
 * スクロールの刻みと最短間隔だけは共有する。
 */
void kobitokey_haptic_effect(uint8_t effect);

/*
 * 同じエフェクトを、gap_ms の間を空けて2回。再ロックの「トゥットゥ」用。
 * 最短間隔は kobitokey_haptic_effect() と共有する。
 */
void kobitokey_haptic_double(uint8_t effect, uint16_t gap_ms);
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
