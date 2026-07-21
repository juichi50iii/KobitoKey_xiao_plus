/*
 * KobitoKey axis scroll cut input processor
 *
 * Converts REL_X / REL_Y trackball movement into:
 *   REL_WHEEL   : vertical scroll
 *   REL_HWHEEL  : horizontal scroll
 *
 * Direction rule:
 *   0 deg   = up
 *   90 deg  = right
 *   180 deg = down
 *   270 deg = left
 *
 * Movements near diagonals are cut.
 */

#define DT_DRV_COMPAT kobitokey_axis_scroll_cut

#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/sys/util.h>

#include <drivers/input_processor.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

enum axis_scroll_cut_dir {
    AXIS_SCROLL_CUT_NONE = 0,
    AXIS_SCROLL_CUT_UP,
    AXIS_SCROLL_CUT_RIGHT,
    AXIS_SCROLL_CUT_DOWN,
    AXIS_SCROLL_CUT_LEFT,
};

struct axis_scroll_cut_config {
    int32_t threshold;
    int32_t tan_threshold_per_mille;
    bool invert_h;
    bool invert_v;
};

struct axis_scroll_cut_data {
    int16_t x;
    int16_t y;
    int16_t acc_x;
    int16_t acc_y;
    bool has_x;
};

static int32_t abs32(int32_t v) {
    return v < 0 ? -v : v;
}

static enum axis_scroll_cut_dir axis_scroll_cut_get_dir(int16_t dx, int16_t dy,
                                                        int32_t tan_threshold_per_mille) {
    int32_t ax = abs32(dx);
    int32_t ay = abs32(dy);

    if (ax == 0 && ay == 0) {
        return AXIS_SCROLL_CUT_NONE;
    }

    /*
     * 0° / 180° direction.
     *
     * Example:
     * tan(35°) is about 0.700.
     *
     * ax / ay <= 0.700 means:
     *   close enough to vertical axis
     */
    if (ax * 1000 <= ay * tan_threshold_per_mille) {
        if (dy < 0) {
            return AXIS_SCROLL_CUT_UP;
        } else {
            return AXIS_SCROLL_CUT_DOWN;
        }
    }

    /*
     * 90° / 270° direction.
     *
     * ay / ax <= 0.700 means:
     *   close enough to horizontal axis
     */
    if (ay * 1000 <= ax * tan_threshold_per_mille) {
        if (dx > 0) {
            return AXIS_SCROLL_CUT_RIGHT;
        } else {
            return AXIS_SCROLL_CUT_LEFT;
        }
    }

    /*
     * Diagonal zone.
     * Around 36°〜54°, 126°〜144°, 216°〜234°, 306°〜324°.
     */
    return AXIS_SCROLL_CUT_NONE;
}

static void axis_scroll_cut_reset_xy(struct axis_scroll_cut_data *data) {
    data->x = 0;
    data->y = 0;
    data->has_x = false;
}

static void axis_scroll_cut_reset_acc(struct axis_scroll_cut_data *data) {
    data->acc_x = 0;
    data->acc_y = 0;
}

static void axis_scroll_cut_make_scroll(const struct device *dev, int16_t *hscroll,
                                        int16_t *vscroll) {
    struct axis_scroll_cut_data *data = dev->data;
    const struct axis_scroll_cut_config *cfg = dev->config;

    *hscroll = 0;
    *vscroll = 0;

    enum axis_scroll_cut_dir dir =
        axis_scroll_cut_get_dir(data->x, data->y, cfg->tan_threshold_per_mille);

    if (dir == AXIS_SCROLL_CUT_NONE) {
        axis_scroll_cut_reset_acc(data);
        return;
    }

    switch (dir) {
    case AXIS_SCROLL_CUT_UP:
        data->acc_y += abs32(data->y);

        if (data->acc_y >= cfg->threshold) {
            *vscroll = cfg->invert_v ? -1 : 1;
            data->acc_y = 0;
        }

        data->acc_x = 0;
        break;

    case AXIS_SCROLL_CUT_DOWN:
        data->acc_y += abs32(data->y);

        if (data->acc_y >= cfg->threshold) {
            *vscroll = cfg->invert_v ? 1 : -1;
            data->acc_y = 0;
        }

        data->acc_x = 0;
        break;

    case AXIS_SCROLL_CUT_RIGHT:
        data->acc_x += abs32(data->x);

        if (data->acc_x >= cfg->threshold) {
            *hscroll = cfg->invert_h ? -1 : 1;
            data->acc_x = 0;
        }

        data->acc_y = 0;
        break;

    case AXIS_SCROLL_CUT_LEFT:
        data->acc_x += abs32(data->x);

        if (data->acc_x >= cfg->threshold) {
            *hscroll = cfg->invert_h ? 1 : -1;
            data->acc_x = 0;
        }

        data->acc_y = 0;
        break;

    default:
        axis_scroll_cut_reset_acc(data);
        break;
    }
}

static int axis_scroll_cut_handle_event(const struct device *dev, struct input_event *event,
                                        uint32_t param1, uint32_t param2,
                                        struct zmk_input_processor_state *state) {
    struct axis_scroll_cut_data *data = dev->data;

    ARG_UNUSED(param1);
    ARG_UNUSED(param2);
    ARG_UNUSED(state);

    if (event->type != INPUT_EV_REL) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    /*
     * Store X and stop the original X event.
     * We will decide scroll direction when Y arrives.
     */
    if (event->code == INPUT_REL_X) {
        data->x = event->value;
        data->has_x = true;

        event->value = 0;
        return ZMK_INPUT_PROC_STOP;
    }

    /*
     * When Y arrives, combine latest X + current Y,
     * then emit either vertical or horizontal scroll.
     */
    if (event->code == INPUT_REL_Y) {
        data->y = event->value;

        int16_t hscroll = 0;
        int16_t vscroll = 0;

        axis_scroll_cut_make_scroll(dev, &hscroll, &vscroll);

        axis_scroll_cut_reset_xy(data);

        if (vscroll != 0) {
            event->code = INPUT_REL_WHEEL;
            event->value = vscroll;
            return ZMK_INPUT_PROC_CONTINUE;
        }

        if (hscroll != 0) {
            event->code = INPUT_REL_HWHEEL;
            event->value = hscroll;
            return ZMK_INPUT_PROC_CONTINUE;
        }

        event->value = 0;
        return ZMK_INPUT_PROC_STOP;
    }

    return ZMK_INPUT_PROC_CONTINUE;
}

static struct zmk_input_processor_driver_api axis_scroll_cut_driver_api = {
    .handle_event = axis_scroll_cut_handle_event,
};

static int axis_scroll_cut_init(const struct device *dev) {
    return 0;
}

#define AXIS_SCROLL_CUT_INST(n)                                                                 \
    static struct axis_scroll_cut_data axis_scroll_cut_data_##n = {};                           \
    static const struct axis_scroll_cut_config axis_scroll_cut_config_##n = {                    \
        .threshold = DT_INST_PROP_OR(n, threshold, 8),                                          \
        .tan_threshold_per_mille = DT_INST_PROP_OR(n, tan_threshold_per_mille, 700),             \
        .invert_h = DT_INST_PROP_OR(n, invert_h, false),                                        \
        .invert_v = DT_INST_PROP_OR(n, invert_v, false),                                        \
    };                                                                                          \
    DEVICE_DT_INST_DEFINE(n, axis_scroll_cut_init, NULL, &axis_scroll_cut_data_##n,              \
                          &axis_scroll_cut_config_##n, POST_KERNEL,                             \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &axis_scroll_cut_driver_api);

DT_INST_FOREACH_STATUS_OKAY(AXIS_SCROLL_CUT_INST)