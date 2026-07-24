#define DT_DRV_COMPAT kobitokey_input_processor_haptic_right

#include <zephyr/device.h>
#include <zephyr/input/input.h>

#include <zmk/input_processor.h>

#include "kobitokey_haptic.h"

/*
 * 右central用 haptic input processor。
 *
 * Layer 4 scroll / Layer 5 flip のどちらでも、
 * 変換前のトラックボール生入力 INPUT_REL_X / INPUT_REL_Y を拾って鳴らす。
 *
 * 通常カーソル移動では、このprocessorを通さなければ鳴らない。
 */
static int haptic_right_handle_event(const struct device *dev,
                                     struct input_event *event,
                                     uint32_t param1,
                                     uint32_t param2,
                                     struct zmk_input_processor_state *state) {
    if (event->type == INPUT_EV_REL &&
        event->value != 0 &&
        (event->code == INPUT_REL_X ||
         event->code == INPUT_REL_Y)) {
        kobitokey_haptic_pulse();
    }

    return ZMK_INPUT_PROC_CONTINUE;
}

static const struct zmk_input_processor_driver_api haptic_right_driver_api = {
    .handle_event = haptic_right_handle_event,
};

#define HAPTIC_RIGHT_INST(n)                                                  \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL, 27,          \
                          &haptic_right_driver_api);

DT_INST_FOREACH_STATUS_OKAY(HAPTIC_RIGHT_INST)