/*
 * 長押しが確定した瞬間に、軽い刻みをひとつ返す。
 *
 * hold-tap のキーは、押した時点ではまだ何になるか決まっていない。単押し
 * なら文字、長押しなら修飾やレイヤー。その分かれ目は指の側からは見えない
 * ので、tapping-term を数えて待つことになる。ここで鳴らすのはその境目だ。
 * 「もう修飾になった」と手に返せば、数えなくてよくなる。
 *
 * 拾い方
 *
 *   修飾キー: zmk_keycode_state_changed の修飾コードの押下。
 *   レイヤー: zmk_layer_state_changed の有効化。
 *
 * どちらも hold-tap がホールドを選んだその場で上がってくる。専用の
 * 「ホールドが確定した」イベントは ZMK には無いが、確定して初めて
 * 束ねられた振る舞いが走るので、結果として同じ瞬間になる。
 *
 * 押しっぱなしだったかを、どう見分けるか
 *
 * 素の &kp LSHFT も、修飾コードの押下という点では hold-tap のホールドと
 * 区別がつかない。区別しないと、大文字を打つたびに鳴ってしまう。
 *
 * 修飾のほうは、イベントが持っている時刻で分かる。hold-tap は束ねた
 * 振る舞いを呼ぶとき、確定した時刻ではなく *最初にキーが下りた時刻* を
 * 渡す (behavior_hold_tap.c の press_binding)。だから今との差が、その
 * キーが押されていた長さそのものになる。素の &kp なら差はほぼゼロだ。
 *
 * レイヤーのほうは時刻を持たない。raise_layer_state_changed はその場で
 * k_uptime_get() を入れるだけだからだ。代わりに、いま押されたままの
 * キーのうち *いちばん古いもの* を見る。&mo はキーが下りた直後に上がる
 * のでゼロに近く、&lt のホールドは tapping-term ぶん古い。他のキーで
 * 早く確定した場合も、古いほうが hold-tap のキーなので正しく出る。
 *
 * この見分け方には、押下の記録とレイヤー変化のどちらが先に届くかに
 * 依存しないという利点もある。&mo の押下がまだ記録されていなければ
 * 「押されたままのキーは無い」となって、やはり鳴らない。どちらに転んでも
 * 答えが同じなら、順番を当てにしなくていい。
 *
 * どちらの手を鳴らすか
 *
 * 判断しているのは central だけだが、鳴らす先は押された手に合わせる。
 * 左手のキーを長押しして右手が鳴ったら、返事をしている手が違う。
 *
 * 押された位置の source は zmk_position_state_changed が持っている。
 * 位置ごとに押された時刻と一緒に控えておいて、鳴らす段になって引く。
 * 自分の側なら直接、向こう側なら zmk_split_central_invoke_behavior() で
 * 向こうの haptic_tick behavior を呼ぶ。汎用のメッセージを送る口は
 * 無いので、鳴らす動作を behavior の形にしてある
 * (behavior_haptic_tick.c)。
 *
 * どの位置が長押しだったかは、修飾とレイヤーで引き方が違う。
 *
 *   修飾は、イベントの時刻がそのまま最初にキーが下りた時刻なので、
 *   控えてある押下時刻と突き合わせれば位置がひとつに決まる。
 *
 *   レイヤーは時刻を持たないので、押されたままのキーのうちいちばん
 *   古いものを長押しの主とみなす。他のキーを重ねて押していると外す
 *   ことがあるが、そのときも鳴りはする。鳴る手が逆になるだけだ。
 *
 * peripheral ではこのファイルが積まれない。Kconfig が central 以外では
 * 選べないようにしてある。ここが読むイベントは向こう側ではビルドすら
 * されないので、条件を付け忘れるとリンクで落ちる。
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/keys.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT)
#include <zmk/split/central.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE)
#include <zmk/split/bluetooth/service.h>
#endif

#include "kobitokey_haptic.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/*
 * 向こう側で鳴らすときに呼ぶ behavior の名前。kobitokey.dtsi のノード名と
 * 一致していなければならない。
 */
#define HOLD_TICK_BEHAVIOR_NAME "hap_tick"

/*
 * 名前が枠に収まっているか、ビルド時に見ておく。
 *
 * 分割リンクが behavior 名に割いている枠は終端込みで9バイトしかない。
 * はみ出すと strlcpy が黙って切り落とし、送信そのものは成功として返る。
 * 向こうでは切れた名前が引けずに何も起きないので、実機で「送れている
 * のに鳴らない」という、いちばん追いにくい形の故障になる。
 * 名前を変えるときにここで止まるようにしておく。
 */
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE)
BUILD_ASSERT(sizeof(HOLD_TICK_BEHAVIOR_NAME) <= ZMK_SPLIT_RUN_BEHAVIOR_DEV_LEN,
             "The haptic tick behavior's name does not fit in the split "
             "link's field, and would be truncated without an error");
#endif

/*
 * 押下時刻を覚えておく位置の数。
 *
 * 小人キーは40キーなので余裕がある。実際のキーマップより小さいことは
 * ないはずだが、位置は外から来る数字なので、書き込む前に必ず確かめる。
 */
#define HOLD_TICK_MAX_POSITIONS 64

/*
 * 押されたままのキーの、押された時刻と、どちらの半身から来たか。
 * 時刻が 0 の枠は「押されていない」を意味する。
 */
static int64_t hold_tick_press_time[HOLD_TICK_MAX_POSITIONS];
static uint8_t hold_tick_press_source[HOLD_TICK_MAX_POSITIONS];

/* 押されたままのキーが無いことを表す位置。 */
#define HOLD_TICK_NO_POSITION (-1)

static int hold_tick_oldest_held(void)
{
    int oldest = HOLD_TICK_NO_POSITION;

    for (int i = 0; i < HOLD_TICK_MAX_POSITIONS; i++) {
        if (hold_tick_press_time[i] == 0) {
            continue;
        }

        if (oldest == HOLD_TICK_NO_POSITION ||
            hold_tick_press_time[i] < hold_tick_press_time[oldest]) {
            oldest = i;
        }
    }

    return oldest;
}

/*
 * その時刻に押されて、まだ離されていない位置。
 *
 * hold-tap が束ねた振る舞いを呼ぶとき渡してくるのは、確定した時刻では
 * なく最初にキーが下りた時刻なので、控えてある押下時刻とそのまま
 * 突き合わせられる。
 */
static int hold_tick_held_at(int64_t pressed_at)
{
    if (pressed_at <= 0) {
        return HOLD_TICK_NO_POSITION;
    }

    for (int i = 0; i < HOLD_TICK_MAX_POSITIONS; i++) {
        if (hold_tick_press_time[i] == pressed_at) {
            return i;
        }
    }

    return HOLD_TICK_NO_POSITION;
}

/*
 * その位置は「押しっぱなしだった」と言えるか。
 *
 * 押されたままのキーが見つからなければ、長押しの結果ではありえない。
 */
static bool hold_tick_was_held(int position)
{
    if (position == HOLD_TICK_NO_POSITION) {
        return false;
    }

    return (k_uptime_get() - hold_tick_press_time[position]) >=
           (int64_t)CONFIG_KOBITOKEY_HAPTIC_HOLD_TICK_MIN_MS;
}

/*
 * そのキーが乗っている手を鳴らす。
 *
 * 向こう側だったときに送るのに失敗したら、こちら側で鳴らす。返す手は
 * 違ってしまうが、長押しが決まったことは伝わる。何も返さないよりは
 * ましだと見ている。
 */
static void hold_tick_play(int position)
{
    const uint8_t effect = CONFIG_KOBITOKEY_HAPTIC_HOLD_TICK_EFFECT;

#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    if (position != HOLD_TICK_NO_POSITION) {
        const uint8_t source = hold_tick_press_source[position];

        if (source != ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL) {
            struct zmk_behavior_binding binding = {
                .behavior_dev = HOLD_TICK_BEHAVIOR_NAME,
                .param1 = effect,
                .param2 = 0,
            };
            struct zmk_behavior_binding_event event = {
                .layer = 0,
                .position = position,
                .timestamp = k_uptime_get(),
                .source = source,
            };

            const int err = zmk_split_central_invoke_behavior(
                source, &binding, event, true);

            if (err == 0) {
                return;
            }

            LOG_WRN("Could not reach the peripheral's motor (%d); "
                    "ticking this half instead",
                    err);
        }
    }
#else
    ARG_UNUSED(position);
#endif

    kobitokey_haptic_effect(effect);
}

static int hold_tick_listener(const zmk_event_t *eh)
{
    const struct zmk_position_state_changed *pos =
        as_zmk_position_state_changed(eh);

    if (pos) {
        if (pos->position < HOLD_TICK_MAX_POSITIONS) {
            hold_tick_press_time[pos->position] =
                pos->state ? pos->timestamp : 0;
            hold_tick_press_source[pos->position] = pos->source;
        }

        return ZMK_EV_EVENT_BUBBLE;
    }

#if IS_ENABLED(CONFIG_KOBITOKEY_HAPTIC_HOLD_TICK_MODS)
    const struct zmk_keycode_state_changed *kc =
        as_zmk_keycode_state_changed(eh);

    if (kc) {
        if (kc->state && is_mod(kc->usage_page, kc->keycode)) {
            const int position = hold_tick_held_at(kc->timestamp);

            if (hold_tick_was_held(position)) {
                hold_tick_play(position);
            }
        }

        return ZMK_EV_EVENT_BUBBLE;
    }
#endif

#if IS_ENABLED(CONFIG_KOBITOKEY_HAPTIC_HOLD_TICK_LAYERS)
    const struct zmk_layer_state_changed *layer =
        as_zmk_layer_state_changed(eh);

    if (layer) {
        if (layer->state) {
            const int position = hold_tick_oldest_held();

            if (hold_tick_was_held(position)) {
                hold_tick_play(position);
            }
        }

        return ZMK_EV_EVENT_BUBBLE;
    }
#endif

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(kobitokey_hold_tick, hold_tick_listener);
ZMK_SUBSCRIPTION(kobitokey_hold_tick, zmk_position_state_changed);

#if IS_ENABLED(CONFIG_KOBITOKEY_HAPTIC_HOLD_TICK_MODS)
ZMK_SUBSCRIPTION(kobitokey_hold_tick, zmk_keycode_state_changed);
#endif

#if IS_ENABLED(CONFIG_KOBITOKEY_HAPTIC_HOLD_TICK_LAYERS)
ZMK_SUBSCRIPTION(kobitokey_hold_tick, zmk_layer_state_changed);
#endif
