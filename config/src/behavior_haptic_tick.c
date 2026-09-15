/*
 * その半身のモーターでエフェクトをひとつ鳴らすだけの behavior。
 *
 * キーに割り当てるためのものではない。左手のキーを長押ししたときに
 * 左手を鳴らすための、唯一の通り道として置いてある。
 *
 * 分割キーボードでは、押されたキーが単押しになったか長押しになったかを
 * 知っているのは central だけだ。peripheral はキーの位置を送るところ
 * までで、その先を知らない。だから左手を鳴らす合図は central から
 * 渡してやるしかない。
 *
 * ZMK が用意している経路は zmk_split_central_invoke_behavior() で、これは
 * 「向こう側の behavior を名前で呼ぶ」という形をしている。汎用の
 * メッセージを送る口は無い。呼びたいものが behavior でなければ届かない
 * ので、鳴らすという動作を behavior の形にしてある。キーマップに
 * 出てこない behavior なのはそのためだ。
 *
 * 引数はエフェクト番号ひとつ。押されたときだけ鳴らし、離されたときは
 * 何もしない。長押しが決まった瞬間を返すのが目的で、離した瞬間には
 * 返すものが無いからだ。
 */

#define DT_DRV_COMPAT kobitokey_behavior_haptic_tick

#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>

#include "kobitokey_haptic.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event)
{
    ARG_UNUSED(event);

    kobitokey_haptic_effect((uint8_t)binding->param1);

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event)
{
    ARG_UNUSED(binding);
    ARG_UNUSED(event);

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_haptic_tick_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &behavior_haptic_tick_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
