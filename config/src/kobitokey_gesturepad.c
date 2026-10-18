#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/init.h>
#include <stdio.h>
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/behavior.h>
#include <zmk/hid.h>
#include <zmk/endpoints.h>

#if defined(CONFIG_RGBLED_WIDGET)
#include <zmk_rgbled_widget/widget.h>
#endif

#include "kobitokey_gesturepad.h"

#if defined(CONFIG_KOBITOKEY_HAPTIC)
#include "kobitokey_haptic.h"
#endif

LOG_MODULE_REGISTER(kobitokey_gesturepad, LOG_LEVEL_INF);

#define GESTUREPAD_NODE DT_NODELABEL(gesturepad)
BUILD_ASSERT(DT_NODE_HAS_STATUS(GESTUREPAD_NODE, okay),
             "gesturepad is missing: the CY8CMBR3106S must be in the devicetree");

static const struct i2c_dt_spec gesturepad_i2c = I2C_DT_SPEC_GET(GESTUREPAD_NODE);
static const struct gpio_dt_spec gesturepad_hi =
    GPIO_DT_SPEC_GET(GESTUREPAD_NODE, hi_gpios);

/*
 * レジスタアドレスは CY8CMBR3xxx Register TRM (文書番号 001-92243) より。
 * メインのデータシート(001-92218)にはレジスタ区分(コンフィギュレー
 * ション/コマンド/ステータス)の範囲しか書かれておらず、個別アドレスは
 * こちらの別冊にしかない。
 *
 * SENSOR_EN や SLIDER1_CFG などスライダーを有効化する側の設定は、まだ
 * ここでは書いていない。しきい値・感度は実機でタッチしながら詰める
 * ものなので、データシートの数字だけを見て決め打ちすると外れる。
 * 工場出荷時設定のままで SLIDER1_POSITION が常に 0xFF(タッチなし)を
 * 返すなら、それが原因の可能性が高い ── 基板が届いたらここを埋める。
 */
#define REG_SLIDER1_POSITION 0xb0 /* 8bit, 0-254 = 位置, 255 = タッチなし */
#define REG_BUTTON_STAT      0xaa /* 16bit, bit0=CS0 ... bit15=CS15 */
#define REG_CTRL_CMD         0x86 /* 書き込んだ値のコマンドを実行する */
#define REG_SLIDER1_THRESHOLD 0x63 /* スライダーの指の判定のしきい値(1-255、工場出荷は128) */
#define REG_CONFIG_CRC       0x7e /* 設定領域(0x00-0x7D)のCRC。下位が0x7E、上位が0x7F */
#define REG_CTRL_CMD_ERR     0x89 /* 直前のコマンドの結果。0 = 正常、0xFE = CRC不一致 */
#define REG_FAMILY_ID        0x8f /* CY8CMBR3xxx は 0x9A */
#define CTRL_CMD_SAVE_CHECK_CRC 0x02 /* CRCを検証して、合えば設定を不揮発メモリへ保存する */
#define CTRL_CMD_SW_RESET    0xff /* ソフトウェアリセット: 基準(ベースライン)を取り直す */
#define REG_DIFFERENCE_COUNT 0xba /* センサー0〜15の差分カウント(各16bit、2バイトずつ) */

#define GESTUREPAD_NO_TOUCH 255

/*
 * HI パルスを受けてからI2Cを読みに行くまでの猶予。
 *
 * データシート上は読みに行けない理由は無いが、fold センサーのデバウンス
 * と同じ発想で、割り込みそのものからは間を置かずに専用ワークキューへ
 * 逃がすだけにしておく(I2C はそのワークキュー上でだけ触る)。
 */
#define GESTUREPAD_DEBOUNCE_MS 5

static struct k_work_q gesturepad_workq;
static K_THREAD_STACK_DEFINE(gesturepad_workq_stack,
                             CONFIG_KOBITOKEY_GESTUREPAD_WORKQ_STACK_SIZE);
static struct k_work_delayable gesturepad_read_work;
static struct gpio_callback gesturepad_hi_callback;

/* この2つは gesturepad_workq 上でしか触らないので、ロックなしで足りる。
 * kobitokey_gesturepad_position() から読まれる last_position だけは
 * 他のスレッドからも見られるが、int の読み書きは nRF52840 では
 * アトミックなので、ここでも特別な保護はしていない。 */
static int gesturepad_last_position = -1;
static int gesturepad_prev_position = -1;

/*
 * PC側で本当に効いているかをとりあえず確認するための、暫定のボリューム
 * 割り当て。「長押しでアームしてからジェスチャー」という本来やりたい
 * 形にはまだ入っていない ── なぞればそのまま音量が動く、検証専用の
 * 配線だと思ってほしい。
 *
 * 位置の生カウントを直接キーに変換すると感度が高すぎるので、haptic の
 * 距離ノッチ(haptic_pulse_by_distance)と同じやり方で、一定カウント
 * 分動くごとに1段としてまとめる。
 */
static int32_t gesturepad_volume_remainder;

/*
 * 誤動作防止のロック。触れただけでは音量/輝度は動かず、
 * HOLD_MS 動かさずに押さえ続けると1発振動してアームされ、そこから
 * 輝度/音量の調整が効く。最後の操作から LOCK_MS 経つと再びロックする。
 *
 * 状態は gesturepad_workq 上でしか触らない(タイマーも同じキュー)。
 */
static bool gesturepad_armed;

/*
 * 目盛り触覚。パッドを TICK_SPACING カウントごとのマスに区切り、指が隣の
 * マスへ入った瞬間に1回だけ鳴らす。動くたびに鳴らすのではなく、境界を
 * またいだときだけなので、物理ダイヤルのクリック感になる。-1 は基準なし。
 */
static int gesturepad_tick_cell = -1;
static int gesturepad_hold_anchor = -1;
static struct k_work_delayable gesturepad_arm_work;
static struct k_work_delayable gesturepad_lock_work;
static bool gesturepad_lock_forced; /* 長押し・キー入力による再ロック(触れていても閉じる) */

/*
 * 押さえている間(解除前)は、センサーからの通知を待たずにこちらから読む。
 * 指を止めると HI# が来なくなることがあり、触れた直後に一度「タッチなし」が
 * 混ざっただけで、長押しの数えが取り消されたまま二度と動かなくなる。
 * 「タッチなし」も NO_TOUCH_CONFIRM 回続くまでは離したと見なさない。
 */
#define GESTUREPAD_HOLD_POLL_MS 50

/*
 * チップは省電力で眠っていて、最初のI2Cアクセスには応答しない(NACK、
 * 実機ログでは -5)。そのアクセスで起きるので、少し待ってやり直せば通る。
 * 諦めてしまうと、指を止めた長押しではもう通知が来ず、二度と読まれない。
 */
#define GESTUREPAD_READ_RETRY_MS 10
/*
 * 5回(50ms)では、しばらく触らなかったあとの1発目が、起きるのを待ちきれずに
 * 捨てられて空振りになった。HI# は触れた瞬間の1回しか来ないので、捨てた
 * 触れ始めはあとから拾い直せない。30回(300ms)まで粘る。
 */
#define GESTUREPAD_READ_RETRY_MAX 30
static int gesturepad_read_retries;
#define GESTUREPAD_NO_TOUCH_CONFIRM 3
static int gesturepad_no_touch_count;

/*
 * 眠りから起きた直後は、触れた通知(HI#)が来ても、位置の読みがまだ「触れて
 * いない」のままのことがある。これを離したと読むと、触れ始めを丸ごと取り
 * こぼす(実機のログで、2回目のタップの触れ始めが -1 と読まれていた)。
 * 触れていないつもりのときに「触れていない」と読めたら、短い間隔で数回
 * 読み直してから、本当に何も無いと決める。
 */
#define GESTUREPAD_PHANTOM_RETRY_MS 20
#define GESTUREPAD_PHANTOM_RETRY_MAX 6
static int gesturepad_phantom_retries;

/*
 * 眠りから起きるのに約140msかかり、素早いタップ(100〜150ms)は、読める
 * ようになる前に終わってしまう。読めた時には、もう「触れていない」で、
 * タップを丸ごと取りこぼす(実機のログで、触れの通知はあったのに、位置は
 * 一度も読めなかった)。触れの通知が来て、読み直しても触れが見えなかった
 * ときは、読む前に終わった素早いタップとして数える。
 * 通知は触れていない間だけ数える(動いている最中の通知は関係ない)。
 */
static volatile bool gesturepad_hi_pending;

/*
 * 解除中のLED表示。レイヤー色と同じ「点きっぱなし」の枠を使う。
 *
 * ウィジェット(別リポジトリのフォーク)は、点灯中の色を led_layer_color に
 * 持ち、点滅のあとはその色へ戻す。その値と点灯指示のキュー(led_msgq)を
 * 外から借りて、解除中だけ黄色にしている。ウィジェット側の内部名に頼る
 * ので、フォークの該当箇所を変えたらここも合わせる必要がある。
 *
 * 右手(中央)は、解除時にレイヤー色を再計算させて元に戻す。左手(周辺)は
 * レイヤー色を持たないので、消すだけ。
 */
#if defined(CONFIG_RGBLED_WIDGET)
struct gesturepad_blink_item {
    uint8_t color;
    uint16_t duration_ms;
    uint16_t sleep_ms;
};
extern uint8_t led_layer_color;
extern struct k_msgq led_msgq;

static void gesturepad_indicator_set(bool on, uint8_t color)
{
#if SHOW_LAYER_COLORS
    if (!on) {
        /* led_layer_color が今の層の色と食い違うので、再計算で戻る。 */
        rgbled_widget_refresh_layer_color();
        return;
    }
#endif
    led_layer_color = on ? color : 0;

    struct gesturepad_blink_item item = {.color = led_layer_color};

    k_msgq_put(&led_msgq, &item, K_NO_WAIT);
}
#else
static void gesturepad_indicator_set(bool on, uint8_t color)
{
    ARG_UNUSED(on);
    ARG_UNUSED(color);
}
#endif

/*
 * 解除中は、レイヤーが変わってもモードの色を優先する。
 *
 * オートマウスなどでレイヤーが切り替わると、ウィジェットが点きっぱなしの
 * 色をそのレイヤーの色に書き換えて、モードの表示が消える。レイヤーが
 * 変わるたびに、少し待ってモードの色を取り戻す(ウィジェット側の書き換えが
 * 先に済むよう、すぐではなく待つ)。右手(中央)だけ。左手はレイヤー色を
 * 持たず、書き換えられない。
 */
#if defined(CONFIG_RGBLED_WIDGET) && SHOW_LAYER_COLORS
#define GESTUREPAD_INDICATOR_REASSERT_MS 20
static struct k_work_delayable gesturepad_indicator_work;

static void gesturepad_indicator_work_handler(struct k_work *work);

static int gesturepad_layer_listener(const zmk_event_t *eh)
{
    if (as_zmk_layer_state_changed(eh) != NULL && gesturepad_armed) {
        k_work_reschedule_for_queue(&gesturepad_workq, &gesturepad_indicator_work,
                                    K_MSEC(GESTUREPAD_INDICATOR_REASSERT_MS));
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(gesturepad_layer, gesturepad_layer_listener);
ZMK_SUBSCRIPTION(gesturepad_layer, zmk_layer_state_changed);
#endif

/*
 * 長押しの判定(解除前は「解除」、解除中は「再ロック」)で、今回の触れ方では
 * もう使い終わったことを覚えておく。解除した指がまだ触れたままでも、
 * すぐ再ロックが始まらないようにするため。指を離すと下ろす。
 */
static bool gesturepad_hold_done;

/*
 * 解除も再ロックも「タップ→長押し」: 短くタップして、DOUBLE_TAP_MS 以内に
 * もう一度触れ、そのまま HOLD_MS 動かさずに押さえる。持ったときの手が
 * パッドに触れ続けただけでは、タップが無いので、解除されない。
 * この「タップの直後に触れ始めた」状態を、触れている間だけ持つ。
 */
static bool gesturepad_primed;

/* 長押し→スワイプ: 止まった位置と時刻、ゾーンが決まったあとの待ち。 */
static int gesturepad_still_anchor = -1;
static int64_t gesturepad_still_since_ms;
static bool gesturepad_zone_pending;
static int gesturepad_zone_pending_pos;
static int64_t gesturepad_zone_pending_ms;

/* 解除前のフリック判定用の速さ(カウント/秒)。触れ始めで0に戻す。 */
static int32_t gesturepad_prime_velocity;

/*
 * 解除のフリックの余韻を、調整として数えないための状態。解除したあとも、
 * はじいた指はまだ動いているので、そのまま数えると、解除のフリックが
 * 音量・スクロールなどの操作になってしまう。解除したら、指が止まる(または
 * 離れる)まで、パッドの操作を無視する。解除は、解除だけに専念させる。
 */
static bool gesturepad_unlock_swallow;
static int gesturepad_swallow_ref_pos;

/* tap モード(解除後の選択肢)の、止まった位置と時刻、1回の触れで1回だけ入力するための印。 */
static int gesturepad_tap_anchor = -1;
static int64_t gesturepad_tap_since_ms;
static bool gesturepad_tap_fired;
static int64_t gesturepad_swallow_still_since_ms;
static int64_t gesturepad_prime_last_ms;

/*
 * 起動直後の誤爆よけ。折り畳みを開くときに手がパッドに触れていることが
 * あるので、起動から BOOT_GUARD_MS の間は解除の長押しを数えない。
 * 解除できるようになるのは、その後に次の2つを済ませてから:
 *
 *   - 指が触れていなければ、チップをリセットして基準を取り直す。
 *     起動の瞬間に触れていると、その状態を「触れていない」基準として
 *     覚えてしまい、あとの位置が不安定になりうる。触れている間は
 *     リセットしない(同じ誤った基準を作り直すだけなので)。
 *   - 触れていたら、その指を一度離すまで長押しを数えない。
 */
static bool gesturepad_guard_over;
static bool gesturepad_boot_recal_done;
static bool gesturepad_config_checked;
static struct k_work_delayable gesturepad_boot_work;

/*
 * ゾーン。キーマップの gesture_left / gesture_right ノードの子(zone1, zone2, ...)
 * の数が、そのままパッドの分割数になる。zone1 が一番上。
 *
 * 解除したときに押さえていた位置で、解除後に使うゾーンが決まる。位置は
 * 0 から POSITION_MAX まで(実機では 0-44 ほど)を、ゾーン数で等分する。
 * 各ゾーンには、キー指定(上げ・下げ・ダブルタップ)と、動き方(mode)を
 * キーマップの中で書く。
 */
#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#define GESTUREPAD_THIS_NODE DT_NODELABEL(gesture_left)
#define GESTUREPAD_IS_CENTRAL 0
#else
#define GESTUREPAD_THIS_NODE DT_NODELABEL(gesture_right)
#define GESTUREPAD_IS_CENTRAL 1
#endif

#define GESTUREPAD_ZONE_MAX 5
/* 子ノードの数(この Zephyr には DT_CHILD_NUM_STATUS_OKAY が無いので、1ずつ足す)。 */
#define GESTUREPAD_COUNT_ONE(node) +1
#define GESTUREPAD_ZONE_COUNT (0 DT_FOREACH_CHILD_STATUS_OKAY(GESTUREPAD_THIS_NODE, GESTUREPAD_COUNT_ONE))

BUILD_ASSERT(GESTUREPAD_ZONE_COUNT >= 1 && GESTUREPAD_ZONE_COUNT <= GESTUREPAD_ZONE_MAX,
             "gesture_left / gesture_right の zone は 1〜5 個");

enum gesturepad_mode_kind {
    GESTUREPAD_MODE_STEP,
    GESTUREPAD_MODE_FLICK,
    GESTUREPAD_MODE_STEP_FLIP,
    GESTUREPAD_MODE_SCROLL,
    GESTUREPAD_MODE_TAP,
    GESTUREPAD_MODE_CYCLE,
};

#define GESTUREPAD_ZONE_MODE(node) DT_ENUM_IDX_OR(node, mode, 0)
#define GESTUREPAD_ZONE_COLOR(node) DT_PROP_OR(node, led_color, 0)

/* zone1(一番上)から順。 */
static const uint8_t gesturepad_zone_mode[GESTUREPAD_ZONE_COUNT] = {
    DT_FOREACH_CHILD_STATUS_OKAY_SEP(GESTUREPAD_THIS_NODE, GESTUREPAD_ZONE_MODE, (, ))};
static const uint8_t gesturepad_zone_color_setting[GESTUREPAD_ZONE_COUNT] = {
    DT_FOREACH_CHILD_STATUS_OKAY_SEP(GESTUREPAD_THIS_NODE, GESTUREPAD_ZONE_COLOR, (, ))};

/* 色の指定が無いゾーンは、上から緑・黄・赤・マゼンタ・青。 */
static const uint8_t gesturepad_zone_color_default[GESTUREPAD_ZONE_MAX] = {2, 3, 1, 5, 4};

static int gesturepad_zone;

/*
 * cycle モード(Rhino のビュー切り替えなど)の、今の選択肢の番号と、反対側にして
 * いるかどうか。キーボードは Rhino の今のビューを知らないので、自分で覚えて
 * おく。毎回、絶対的なビュー(bindings / double-bindings)を指定して送るので、
 * ずれても、次の操作でその場で直る。解除をまたいで残す。
 */
#if GESTUREPAD_IS_CENTRAL
static int8_t gesturepad_cycle_idx[GESTUREPAD_ZONE_MAX];
static bool gesturepad_cycle_flipped[GESTUREPAD_ZONE_MAX];
#endif

/* 一番上を0として、位置からゾーンの番号を求める。 */
static int gesturepad_zone_for_position(int position)
{
    int from_bottom = (position * GESTUREPAD_ZONE_COUNT) / (CONFIG_KOBITOKEY_GESTUREPAD_POSITION_MAX + 1);

    if (from_bottom > GESTUREPAD_ZONE_COUNT - 1) {
        from_bottom = GESTUREPAD_ZONE_COUNT - 1;
    }

    /* 位置が大きいほうが上(右手)。左右の基板は向きが逆に付くので、左手は
     * 位置が小さいほうが上になる。 */
    return IS_ENABLED(CONFIG_KOBITOKEY_GESTUREPAD_ZONE_REVERSED)
               ? from_bottom
               : (GESTUREPAD_ZONE_COUNT - 1 - from_bottom);
}

static uint8_t gesturepad_zone_color(int zone)
{
    return gesturepad_zone_color_setting[zone] != 0 ? gesturepad_zone_color_setting[zone]
                                                    : gesturepad_zone_color_default[zone];
}

/*
 * 「フリック」のゾーン(mode = "flick"): なぞった量で何度も出すのではなく、
 * 1回のスワイプで1回だけ出す(曲送りなど、連打するものではない操作向け)。
 * 触れ始めの位置から FLICK_DISTANCE 動いた瞬間に1回出し、指を離すまで
 * 次は出さない。
 */
static int gesturepad_flick_origin = -1;

/*
 * フリップ。連続して動くモード(ボリュームなど)で、勢いを付けて離すと、
 * 離した瞬間に FLIP_STEPS 段を一気に足す。ゆっくり離したときは足さず、
 * 今までどおり。時間をかけて動き続ける慣性にすると、離したあとに時差を
 * 置いて伸びる感じになって気持ち悪かったので、一足飛びにしている。
 *
 * 速さは、なぞっている間の「位置の変化 ÷ 時間」(カウント/秒)で持つ。
 */
/* この時間以上、新しい位置が取れていなければ「止まってから離した」と見なす。 */
#define GESTUREPAD_FLIP_STALE_MS 150
static int32_t gesturepad_velocity;       /* カウント/秒、符号つき */
static int64_t gesturepad_last_sample_ms;

/*
 * ダブルタップ。解除中に、触れてから TAP_MS 以内に動かさずに離したものを
 * 1回のタップと数え、DOUBLE_TAP_MS 以内にもう1回あれば、そのモードの
 * 「タップ」のバインディング(曲送りなら再生/停止)を出す。
 */
static int64_t gesturepad_touch_start_ms;
static int gesturepad_touch_start_pos;
static bool gesturepad_touch_moved;
static int64_t gesturepad_last_tap_ms = INT64_MIN / 2;

static bool gesturepad_flick_fired;

/* 物理キーの位置はここより小さい。これ以上は左手のパッドが右手へ送る位置。 */
#define GESTUREPAD_FIRST_VIRTUAL_POSITION 40

#if defined(CONFIG_RGBLED_WIDGET) && SHOW_LAYER_COLORS
static void gesturepad_indicator_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    if (gesturepad_armed) {
        gesturepad_indicator_set(true, gesturepad_zone_color(gesturepad_zone));
    }
}
#endif

/*
 * 解除する。position は、解除の長押しをした位置(ゾーンを決める)。
 * 長押しの途中(アーム用のタイマー)と、長い触れが離れたとき(下の
 * gesturepad_read_work_handler)の、2か所から呼ばれる。
 */
static void gesturepad_arm_at(int position, uint8_t haptic_effect)
{
    gesturepad_armed = true;
    gesturepad_volume_remainder = 0;
    /* アーム時点の位置を基準にして、そこからの動きだけを数える。 */
    gesturepad_prev_position = position;
    gesturepad_tick_cell =
        position / CONFIG_KOBITOKEY_GESTUREPAD_TICK_SPACING;

    gesturepad_zone = gesturepad_zone_for_position(position);
    gesturepad_flick_origin = position;
    gesturepad_flick_fired = false;
    gesturepad_last_tap_ms = INT64_MIN / 2;


    LOG_INF("gesturepad: armed, zone %d of %d", gesturepad_zone + 1, GESTUREPAD_ZONE_COUNT);
    gesturepad_indicator_set(true, gesturepad_zone_color(gesturepad_zone));

#if defined(CONFIG_KOBITOKEY_HAPTIC)
    /* 解除は1発、再ロックは「トゥットゥ」の2連打。 */
    kobitokey_haptic_effect(haptic_effect);
#else
    ARG_UNUSED(haptic_effect);
#endif

    k_work_reschedule_for_queue(&gesturepad_workq, &gesturepad_lock_work,
                                K_MSEC(CONFIG_KOBITOKEY_GESTUREPAD_LOCK_MS));
}

static void gesturepad_arm_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    /* 解除中の「タップ→長押し」だけを扱う(再ロック)。解除は、長押しでゾーン
     * 確定→フリック(gesturepad_read_work_handler)。 */
    if (gesturepad_last_position < 0 || gesturepad_hold_done || !gesturepad_guard_over ||
        !gesturepad_primed || !gesturepad_armed) {
        return;
    }

    gesturepad_hold_done = true;

    /* 解除中の長押し: 再ロック。 */
    LOG_INF("gesturepad: lock by hold");
#if defined(CONFIG_KOBITOKEY_HAPTIC)
    /* キー入力や8秒での再ロックは鳴らさず、長押しのときだけ
     * 「閉じた」と分かるように鳴らす。 */
    kobitokey_haptic_double(CONFIG_KOBITOKEY_GESTUREPAD_LOCK_HAPTIC_EFFECT,
                            CONFIG_KOBITOKEY_GESTUREPAD_LOCK_HAPTIC_GAP_MS);
#endif
    gesturepad_lock_forced = true;
    k_work_reschedule_for_queue(&gesturepad_workq, &gesturepad_lock_work, K_NO_WAIT);
}

static void gesturepad_lock_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    const bool was_armed = gesturepad_armed;

    /* 触れている間は、自動ロックで閉じない(スクロールしながら指を置いたままに
     * しているだけのとき)。離してからあらためて数える。長押しやキー入力による
     * 再ロックは、この待ち時間を通らずに閉じる(下の force 経路)。 */
    if (was_armed && gesturepad_last_position >= 0 && !gesturepad_lock_forced) {
        k_work_reschedule_for_queue(&gesturepad_workq, &gesturepad_lock_work,
                                    K_MSEC(CONFIG_KOBITOKEY_GESTUREPAD_LOCK_MS));
        return;
    }
    gesturepad_lock_forced = false;

    gesturepad_armed = false;
    gesturepad_unlock_swallow = false;
    gesturepad_tap_fired = false;
    gesturepad_tap_anchor = -1;
    gesturepad_volume_remainder = 0;
    gesturepad_tick_cell = -1;
    gesturepad_flick_origin = -1;
    gesturepad_flick_fired = false;

    if (was_armed) {
        gesturepad_indicator_set(false, 0);
    }

    LOG_INF("gesturepad: locked");
}

/*
 * キー入力で再ロックする。パッドの音量/輝度は「調整するための一時的な状態」
 * なので、キーを打ち始めたら閉じる。仮想位置(左手のパッド自身が送るもの)は
 * 除く。自分で自分をロックしてしまうため。
 *
 * 右手(中央)は左右どちらのキーも見える。左手は左手のキーだけ。
 */
static int gesturepad_key_listener(const zmk_event_t *eh)
{
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev != NULL && ev->state && gesturepad_armed &&
        ev->position < GESTUREPAD_FIRST_VIRTUAL_POSITION) {
        gesturepad_lock_forced = true;
        k_work_reschedule_for_queue(&gesturepad_workq, &gesturepad_lock_work,
                                    K_NO_WAIT);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(gesturepad_key, gesturepad_key_listener);
ZMK_SUBSCRIPTION(gesturepad_key, zmk_position_state_changed);

/*
 * 1段ぶんの操作を出す。
 *
 * 右手(中央)は、ゾーンに書かれたキー指定をそのまま実行する。左手(周辺)は
 * HIDを持たず、キーを直接は出せないので、「左手の何番目のゾーンの何」を
 * 仮想のキー位置(40 + ゾーン×3 + 0上げ/1下げ/2ダブルタップ)にして、押して
 * 離したことにして右手へ送る。右手は、受け取った位置を、キーマップの
 * gesture_left の同じゾーンのキー指定に引き当てて実行する(下の
 * gesturepad_left_listener)。
 *
 * 左手は基板の向きの都合で、右へなぞると位置が減る。実機で輝度が逆に
 * 動いたので、左手は「上げ」「下げ」を入れ替えている(ZONE_REVERSED と同じ理由)。
 */
#define GESTUREPAD_BINDINGS_PER_ZONE 3

/* bindings の無いゾーン(scroll だけ許される)は、空の指定を3つ入れて幅をそろえる。 */
#define GESTUREPAD_ZONE_BINDINGS(node)                                                            \
    COND_CODE_1(DT_NODE_HAS_PROP(node, bindings),                                                 \
                (LISTIFY(DT_PROP_LEN(node, bindings), ZMK_KEYMAP_EXTRACT_BINDING, (, ), node)),   \
                ({0}, {0}, {0}))
/* 左手は、右手への受け渡しの番号(ゾーン×3)の都合で、すべて3つ固定。 */
#define GESTUREPAD_CHECK_ZONE(node)                                                               \
    BUILD_ASSERT(DT_NODE_HAS_PROP(node, bindings)                                                 \
                     ? (DT_PROP_LEN_OR(node, bindings, 0) == GESTUREPAD_BINDINGS_PER_ZONE)        \
                     : (GESTUREPAD_ZONE_MODE(node) == GESTUREPAD_MODE_SCROLL),                    \
                 "zone の bindings は「上げ」「下げ」「ダブルタップ」の3つ"                       \
                 "(使わないものは &none)。書かずに済むのは mode = \"scroll\" のゾーンだけ");

/* 右手は、tap のゾーンだけ、選択肢の数を1〜5で選べる(double-bindings は同じ数)。 */
#define GESTUREPAD_CHECK_ZONE_RIGHT(node)                                                         \
    BUILD_ASSERT(DT_NODE_HAS_PROP(node, bindings)                                                 \
                     ? (DT_PROP_LEN_OR(node, bindings, 0) == GESTUREPAD_BINDINGS_PER_ZONE ||      \
                        ((GESTUREPAD_ZONE_MODE(node) == GESTUREPAD_MODE_TAP ||                    \
                          GESTUREPAD_ZONE_MODE(node) == GESTUREPAD_MODE_CYCLE) &&                  \
                         DT_PROP_LEN_OR(node, bindings, 0) >= 1 &&                                \
                         DT_PROP_LEN_OR(node, bindings, 0) <= 5))                                 \
                     : (GESTUREPAD_ZONE_MODE(node) == GESTUREPAD_MODE_SCROLL),                    \
                 "zone の bindings は3つ(上げ・下げ・ダブルタップ)。tap / pick のゾーンだけ "         \
                 "1〜5個の選択肢にできる。書かずに済むのは scroll のゾーンだけ");                \
    BUILD_ASSERT(!DT_NODE_HAS_PROP(node, double_bindings) ||                                      \
                     ((GESTUREPAD_ZONE_MODE(node) == GESTUREPAD_MODE_TAP ||                       \
                       GESTUREPAD_ZONE_MODE(node) == GESTUREPAD_MODE_CYCLE) &&                     \
                      DT_PROP_LEN_OR(node, double_bindings, 0) ==                                 \
                          DT_PROP_LEN_OR(node, bindings, 0)),                                     \
                 "double-bindings は tap のゾーンだけ、bindings と同じ数で書く");

DT_FOREACH_CHILD_STATUS_OKAY(DT_NODELABEL(gesture_left), GESTUREPAD_CHECK_ZONE)
DT_FOREACH_CHILD_STATUS_OKAY(DT_NODELABEL(gesture_right), GESTUREPAD_CHECK_ZONE_RIGHT)

/* ゾーンの bindings の数(書かない scroll は、空の3つ分)。 */
#define GESTUREPAD_ZONE_BINDING_COUNT(node)                                                       \
    COND_CODE_1(DT_NODE_HAS_PROP(node, bindings), (DT_PROP_LEN(node, bindings)), (3))

/* double-bindings(tap のダブルタップ用)の取り出し。ZMK の取り出しは bindings 固定なので、写した。 */
#define GESTUREPAD_EXTRACT_DOUBLE(idx, node)                                                      \
    {                                                                                             \
        .behavior_dev = DEVICE_DT_NAME(DT_PHANDLE_BY_IDX(node, double_bindings, idx)),            \
        .param1 = COND_CODE_0(DT_PHA_HAS_CELL_AT_IDX(node, double_bindings, idx, param1), (0),    \
                              (DT_PHA_BY_IDX(node, double_bindings, idx, param1))),               \
        .param2 = COND_CODE_0(DT_PHA_HAS_CELL_AT_IDX(node, double_bindings, idx, param2), (0),    \
                              (DT_PHA_BY_IDX(node, double_bindings, idx, param2))),               \
    }
#define GESTUREPAD_ZERO_BINDING(idx, node) {0}
#define GESTUREPAD_ZONE_DOUBLES(node)                                                             \
    COND_CODE_1(DT_NODE_HAS_PROP(node, double_bindings),                                          \
                (LISTIFY(DT_PROP_LEN(node, double_bindings), GESTUREPAD_EXTRACT_DOUBLE, (, ),     \
                         node)),                                                                  \
                (LISTIFY(GESTUREPAD_ZONE_BINDING_COUNT(node), GESTUREPAD_ZERO_BINDING, (, ),      \
                         node)))

#if GESTUREPAD_IS_CENTRAL
static const struct zmk_behavior_binding gesturepad_right_bindings[] = {
    DT_FOREACH_CHILD_STATUS_OKAY_SEP(DT_NODELABEL(gesture_right), GESTUREPAD_ZONE_BINDINGS, (, ))};
static const struct zmk_behavior_binding gesturepad_right_doubles[] = {
    DT_FOREACH_CHILD_STATUS_OKAY_SEP(DT_NODELABEL(gesture_right), GESTUREPAD_ZONE_DOUBLES, (, ))};
static const uint8_t gesturepad_right_count[] = {DT_FOREACH_CHILD_STATUS_OKAY_SEP(
    DT_NODELABEL(gesture_right), GESTUREPAD_ZONE_BINDING_COUNT, (, ))};

/* 右手の bindings は、ゾーンごとに数が違うので、そのゾーンの先頭の位置を数える。 */
static int gesturepad_right_offset(int zone)
{
    int off = 0;

    for (int i = 0; i < zone; i++) {
        off += gesturepad_right_count[i];
    }

    return off;
}

static const struct zmk_behavior_binding gesturepad_left_bindings[] = {
    DT_FOREACH_CHILD_STATUS_OKAY_SEP(DT_NODELABEL(gesture_left), GESTUREPAD_ZONE_BINDINGS, (, ))};

static const uint8_t gesturepad_left_zone_mode[] = {
    DT_FOREACH_CHILD_STATUS_OKAY_SEP(DT_NODELABEL(gesture_left), GESTUREPAD_ZONE_MODE, (, ))};

/*
 * スクロールのゾーン: 1目盛りぶん、マウスのホイールを動かす。キー指定は
 * 使わず、HIDのマウスレポートに直接積む(トラックボールのスクロールと
 * 同じ経路。受け取る側のホイールの分解能の設定に応じて、1あたりの量が
 * 変わるので、SCROLL_UNITS で調整する)。
 */
static void gesturepad_scroll_tick(bool up)
{
    const int16_t units = CONFIG_KOBITOKEY_GESTUREPAD_SCROLL_UNITS *
                          ((up != IS_ENABLED(CONFIG_KOBITOKEY_GESTUREPAD_SCROLL_INVERT)) ? 1 : -1);

    zmk_hid_mouse_scroll_update(0, units);
    zmk_endpoints_send_mouse_report();
    zmk_hid_mouse_scroll_set(0, 0);
}

static void gesturepad_invoke(const struct zmk_behavior_binding *binding, bool pressed)
{
    const struct zmk_behavior_binding_event event = {
        .layer = 0,
        .position = 0,
        .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };

    /* bindings の無いゾーン(scroll のダブルタップ)は何もしない。 */
    if (binding->behavior_dev == NULL) {
        return;
    }

    (void)zmk_behavior_invoke_binding(binding, event, pressed);
}

/*
 * 左手のパッドが送ってきた位置を、キーマップの gesture_left の
 * ゾーンのキー指定に引き当てて実行する。
 *
 * この位置は物理キーではないので、キーマップ本体には渡さずここで
 * 止める(渡されると、範囲外の位置として扱われてしまう)。
 */
static int gesturepad_left_listener(const zmk_event_t *eh)
{
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev == NULL || ev->source == ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL ||
        ev->position < GESTUREPAD_FIRST_VIRTUAL_POSITION) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    const uint32_t index = ev->position - GESTUREPAD_FIRST_VIRTUAL_POSITION;

    if (index < ARRAY_SIZE(gesturepad_left_bindings)) {
        const uint32_t zone = index / GESTUREPAD_BINDINGS_PER_ZONE;
        const uint32_t which = index % GESTUREPAD_BINDINGS_PER_ZONE;

        if (zone < ARRAY_SIZE(gesturepad_left_zone_mode) &&
            gesturepad_left_zone_mode[zone] == GESTUREPAD_MODE_SCROLL && which < 2) {
            /* スクロールのゾーン: 押した瞬間にホイールを動かす(離した方は無視)。 */
            if (ev->state) {
                gesturepad_scroll_tick(which == 0);
            }
        } else {
            gesturepad_invoke(&gesturepad_left_bindings[index], ev->state);
        }
    }

    return ZMK_EV_EVENT_HANDLED;
}

ZMK_LISTENER(gesturepad_left, gesturepad_left_listener);
ZMK_SUBSCRIPTION(gesturepad_left, zmk_position_state_changed);
#endif

static void gesturepad_send_binding(int which)
{
    const int64_t now = k_uptime_get();

    LOG_INF("gesturepad: send zone %d, binding %d", gesturepad_zone + 1, which);

#if GESTUREPAD_IS_CENTRAL
    const struct zmk_behavior_binding *binding =
        &gesturepad_right_bindings[gesturepad_right_offset(gesturepad_zone) + which];

    gesturepad_invoke(binding, true);
    k_msleep(CONFIG_KOBITOKEY_GESTUREPAD_KEY_HOLD_MS);
    gesturepad_invoke(binding, false);
    ARG_UNUSED(now);
#else
    const uint32_t position = GESTUREPAD_FIRST_VIRTUAL_POSITION +
                              gesturepad_zone * GESTUREPAD_BINDINGS_PER_ZONE + which;

    raise_zmk_position_state_changed((struct zmk_position_state_changed){
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
        .position = position,
        .state = true,
        .timestamp = now,
    });

    /*
     * 押している間を空ける。メディア系のキーは一瞬でも通るが、スクロール
     * (押している間だけ動く)と、Ctrl/Cmd 付きのショートカット(修飾キーが
     * OSに届く前に離れてしまう)は、同じ瞬間に離すと動かなかった。
     * このスレッドはパッド専用なので、待っても他を止めない。
     */
    k_msleep(CONFIG_KOBITOKEY_GESTUREPAD_KEY_HOLD_MS);

    raise_zmk_position_state_changed((struct zmk_position_state_changed){
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
        .position = position,
        .state = false,
        .timestamp = k_uptime_get(),
    });
#endif
}

static void gesturepad_send_step(bool up)
{
    gesturepad_send_binding((up != IS_ENABLED(CONFIG_KOBITOKEY_GESTUREPAD_ZONE_REVERSED)) ? 0 : 1);
}

/* tap のゾーンの選択肢の数(右手は 1〜5 を選べる。左手は3つ固定)。 */
static int gesturepad_choice_count(int zone)
{
#if GESTUREPAD_IS_CENTRAL
    return gesturepad_right_count[zone];
#else
    ARG_UNUSED(zone);
    return GESTUREPAD_BINDINGS_PER_ZONE;
#endif
}

/*
 * tap モードで解除したあとの選択肢の番号。パッド全体を選択肢の数で等分して、
 * 位置が属する選択肢を、上から 0, 1, ... で返す(ゾーンの向きと同じ数え方)。
 * 3つなら1つが約17mm、4つなら約12mm。
 */
static int gesturepad_tap_index(int position)
{
    const int total = CONFIG_KOBITOKEY_GESTUREPAD_POSITION_MAX + 1;
    const int n = gesturepad_choice_count(gesturepad_zone);
    int sub = position * n / total;

    if (sub < 0) {
        sub = 0;
    } else if (sub > n - 1) {
        sub = n - 1;
    }

    return IS_ENABLED(CONFIG_KOBITOKEY_GESTUREPAD_ZONE_REVERSED) ? sub : (n - 1 - sub);
}

static void gesturepad_send_tap(void)
{
    if (gesturepad_zone_mode[gesturepad_zone] == GESTUREPAD_MODE_CYCLE) {
#if GESTUREPAD_IS_CENTRAL
        /*
         * cycle のダブルタップ: 今の選択肢の、反対側と元を行き来する(Top ↔ Bottom)。
         * 反対側が無い選択肢(&none)では、何もしない。
         */
        const int z = gesturepad_zone;
        const int off = gesturepad_right_offset(z);
        const int idx = gesturepad_cycle_idx[z];

        if (gesturepad_right_doubles[off + idx].behavior_dev != NULL) {
            gesturepad_cycle_flipped[z] = !gesturepad_cycle_flipped[z];

            const struct zmk_behavior_binding *binding =
                gesturepad_cycle_flipped[z] ? &gesturepad_right_doubles[off + idx]
                                            : &gesturepad_right_bindings[off + idx];

            LOG_INF("gesturepad: cycle %d %s", idx + 1, gesturepad_cycle_flipped[z] ? "flipped" : "front");
            gesturepad_invoke(binding, true);
            k_msleep(CONFIG_KOBITOKEY_GESTUREPAD_KEY_HOLD_MS);
            gesturepad_invoke(binding, false);
        }
#endif
        return;
    }

    if (gesturepad_zone_mode[gesturepad_zone] == GESTUREPAD_MODE_TAP) {
#if GESTUREPAD_IS_CENTRAL
        /*
         * tap のゾーンのダブルタップ: 触れた場所の選択肢の、もう一方(double-bindings)。
         * 入力しても、ロックには戻らない(通常の自動ロックだけ)。
         */
        const int idx = gesturepad_tap_index(gesturepad_touch_start_pos);
        const struct zmk_behavior_binding *binding =
            &gesturepad_right_doubles[gesturepad_right_offset(gesturepad_zone) + idx];

        LOG_INF("gesturepad: double tap, choice %d", idx + 1);
        gesturepad_invoke(binding, true);
        k_msleep(CONFIG_KOBITOKEY_GESTUREPAD_KEY_HOLD_MS);
        gesturepad_invoke(binding, false);
        gesturepad_tap_fired = true;
#endif
        return;
    }

    gesturepad_send_binding(2);
}

/* スクロールの1目盛り。キーを押している間は空けない(連続して出すため)。 */
static void gesturepad_send_scroll(bool up)
{
#if GESTUREPAD_IS_CENTRAL
    gesturepad_scroll_tick(up);
#else
    const uint32_t which = (up != IS_ENABLED(CONFIG_KOBITOKEY_GESTUREPAD_ZONE_REVERSED)) ? 0 : 1;
    const uint32_t position = GESTUREPAD_FIRST_VIRTUAL_POSITION +
                              gesturepad_zone * GESTUREPAD_BINDINGS_PER_ZONE + which;
    const int64_t now = k_uptime_get();

    raise_zmk_position_state_changed((struct zmk_position_state_changed){
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
        .position = position,
        .state = true,
        .timestamp = now,
    });
    raise_zmk_position_state_changed((struct zmk_position_state_changed){
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
        .position = position,
        .state = false,
        .timestamp = now,
    });
#endif
}

/*
 * 動いた量を、SCROLL_COUNTS_PER_UNIT カウントごとに1目盛りにまとめて出す。
 * 端数は持ち越す(離したら捨てる)。1回の読み取りで進める量には上限を付ける。
 */
static int32_t gesturepad_scroll_remainder;

static void gesturepad_scroll_by_distance(int32_t delta)
{
    const int32_t per = CONFIG_KOBITOKEY_GESTUREPAD_SCROLL_COUNTS_PER_UNIT;
    int32_t budget = 12;
    bool sent = false;

    gesturepad_scroll_remainder += delta;

    while (gesturepad_scroll_remainder >= per && budget-- > 0) {
        gesturepad_scroll_remainder -= per;
        gesturepad_send_scroll(true);
        sent = true;
    }

    while (gesturepad_scroll_remainder <= -per && budget-- > 0) {
        gesturepad_scroll_remainder += per;
        gesturepad_send_scroll(false);
        sent = true;
    }

#if defined(CONFIG_KOBITOKEY_HAPTIC)
    /* 動いた読み取りにつき1回(1目盛りごとに鳴らすと、速いときに詰まる)。
     * 最短間隔(HAPTIC_COOLDOWN_MS)は他の振動と共有されている。 */
    if (sent) {
        kobitokey_haptic_effect(CONFIG_KOBITOKEY_GESTUREPAD_SCROLL_HAPTIC_EFFECT);
    }
#endif
}

static void gesturepad_volume_by_distance(int32_t delta)
{
    const int32_t notch = CONFIG_KOBITOKEY_GESTUREPAD_VOLUME_NOTCH;

    if (notch <= 0) {
        return;
    }

    gesturepad_volume_remainder += delta;

    while (gesturepad_volume_remainder >= notch) {
        gesturepad_volume_remainder -= notch;
        gesturepad_send_step(true);
    }

    while (gesturepad_volume_remainder <= -notch) {
        gesturepad_volume_remainder += notch;
        gesturepad_send_step(false);
    }
}

static int gesturepad_read_u8(uint8_t reg, uint8_t *value)
{
    return i2c_reg_read_byte_dt(&gesturepad_i2c, reg, value);
}

static int gesturepad_read_u16(uint8_t reg, uint16_t *value)
{
    uint8_t buf[2];
    int ret = i2c_burst_read_dt(&gesturepad_i2c, reg, buf, sizeof(buf));

    if (ret != 0) {
        return ret;
    }

    /* リトルエンディアン(下位バイトが先のアドレス)。BUTTON_STAT の
     * ビット割り当て(表1.5.93)が bit0=CS0 から始まっているのと符合する。 */
    *value = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);

    return 0;
}

/*
 * 1回のタップを数える。解除前も数える(「タップ→長押し」の前半になる)。
 * 解除中に、続けてもう1回あれば、ダブルタップとして、ゾーンの「タップ」の
 * バインディングを出す。
 */
static void gesturepad_count_tap(int64_t now)
{
    if (gesturepad_armed &&
        now - gesturepad_last_tap_ms <= CONFIG_KOBITOKEY_GESTUREPAD_DOUBLE_TAP_MS) {
        gesturepad_last_tap_ms = INT64_MIN / 2;
        /* 振動は付けない。ダブルタップの直後は指がもう離れていて、
         * 鳴らしても手に伝わらない。 */
        gesturepad_send_tap();
    } else {
        gesturepad_last_tap_ms = now;
    }
}

/*
 * HI が降りるたびに呼ばれる。まだタップ/長押し判定は無く、読めた値を
 * ログに出すだけ ── 基板到着後の疎通確認と、この先の骨格の両方を兼ねる。
 *
 * 動作確認をしやすくするため、位置が前回から動いていたら振動させ
 * (トラックボールのスクロールとは別のエフェクトで、CONFIG_KOBITOKEY_
 * GESTUREPAD_HAPTIC_EFFECT参照)、あわせて暫定でボリュームアップ/
 * ダウンも送る(PC側でも実際に効いているかを見るための検証用配線)。
 * ジェスチャー判定(タップ/長押し/長押しアーム後にスクロール変換、
 * など本来やりたい形)はまだ無い。
 */
static void gesturepad_read_work_handler(struct k_work *work)
{
    uint8_t raw_position = GESTUREPAD_NO_TOUCH;
    uint16_t button_stat = 0;
    int ret;

    ARG_UNUSED(work);

    ret = gesturepad_read_u8(REG_SLIDER1_POSITION, &raw_position);
    if (ret == 0) {
        ret = gesturepad_read_u16(REG_BUTTON_STAT, &button_stat);
    }
    if (ret != 0) {
        if (gesturepad_read_retries++ < GESTUREPAD_READ_RETRY_MAX) {
            k_work_reschedule_for_queue(&gesturepad_workq, &gesturepad_read_work,
                                        K_MSEC(GESTUREPAD_READ_RETRY_MS));
        } else {
            LOG_WRN("gesture pad read failed (%d), giving up", ret);
            gesturepad_read_retries = 0;
        }
        return;
    }
    gesturepad_read_retries = 0;

    if (raw_position == GESTUREPAD_NO_TOUCH) {
        if (gesturepad_prev_position < 0) {
            if (gesturepad_phantom_retries < GESTUREPAD_PHANTOM_RETRY_MAX) {
                gesturepad_phantom_retries++;
                k_work_reschedule_for_queue(&gesturepad_workq, &gesturepad_read_work,
                                            K_MSEC(GESTUREPAD_PHANTOM_RETRY_MS));
                return;
            }
            gesturepad_phantom_retries = 0;

            if (gesturepad_hi_pending) {
                gesturepad_hi_pending = false;
                LOG_INF("gesturepad: tap missed while waking; counted as a tap");
                gesturepad_count_tap(k_uptime_get());
            }
        }

        if (!gesturepad_hold_done && gesturepad_hold_anchor >= 0 &&
            ++gesturepad_no_touch_count < GESTUREPAD_NO_TOUCH_CONFIRM) {
            /* 押さえている最中の一瞬の欠けかもしれない。確かめ直す。 */
            k_work_reschedule_for_queue(&gesturepad_workq, &gesturepad_read_work,
                                        K_MSEC(GESTUREPAD_HOLD_POLL_MS));
            return;
        }
    } else {
        gesturepad_no_touch_count = 0;
        gesturepad_phantom_retries = 0;
        gesturepad_hi_pending = false;
    }

    gesturepad_last_position =
        (raw_position == GESTUREPAD_NO_TOUCH) ? -1 : (int)raw_position;

    LOG_INF("gesturepad: position=%d button_stat=0x%04x",
            gesturepad_last_position, button_stat);

#if defined(CONFIG_KOBITOKEY_GESTUREPAD_DEBUG_DIFF)
    /*
     * 診断用: チップの生の信号(差分カウント)。指を離したあとも「触れている」と
     * 答え続けるのが、チップの判定の遅れなのか、信号そのものが下がらないのかを
     * 切り分ける。0でないセンサーだけを出す。
     */
    {
        uint8_t raw[32];
        char line[128];
        size_t n = 0;

        if (i2c_burst_read_dt(&gesturepad_i2c, REG_DIFFERENCE_COUNT, raw, sizeof(raw)) == 0) {
            for (int i = 0; i < 16 && n + 12 < sizeof(line); i++) {
                const int16_t v = (int16_t)((uint16_t)raw[i * 2] | ((uint16_t)raw[i * 2 + 1] << 8));

                if (v != 0) {
                    n += snprintf(line + n, sizeof(line) - n, " s%d=%d", i, v);
                }
            }
            line[n] = '\0';
            LOG_INF("gesturepad: diff%s", n ? line : " (all zero)");
        }
    }
#endif

    /* 勢いを付けて離したら、その向きに FLIP_STEPS 段を一気に足す。 */
    if (gesturepad_last_position < 0 && gesturepad_prev_position >= 0 &&
        gesturepad_armed && !gesturepad_unlock_swallow &&
        gesturepad_zone_mode[gesturepad_zone] == GESTUREPAD_MODE_STEP_FLIP) {
        const int64_t age = k_uptime_get() - gesturepad_last_sample_ms;

        LOG_INF("gesturepad: release speed %d counts/s (age %d ms)",
                (int)gesturepad_velocity, (int)age);

        if (age <= GESTUREPAD_FLIP_STALE_MS &&
            abs(gesturepad_velocity) >= CONFIG_KOBITOKEY_GESTUREPAD_INERTIA_THRESHOLD) {
            for (int i = 0; i < CONFIG_KOBITOKEY_GESTUREPAD_FLIP_STEPS; i++) {
                gesturepad_send_step(gesturepad_velocity > 0);
            }
            gesturepad_velocity = 0;
        }
    }

    {
        const int64_t now = k_uptime_get();
        const int prev = gesturepad_prev_position;

        if (gesturepad_last_position >= 0 && prev < 0) {
            gesturepad_touch_start_ms = now;
            gesturepad_touch_start_pos = gesturepad_last_position;
            gesturepad_touch_moved = false;
            gesturepad_still_anchor = gesturepad_last_position;
            gesturepad_still_since_ms = now;
            gesturepad_zone_pending = false;
            gesturepad_prime_velocity = 0;
            gesturepad_prime_last_ms = now;
            /* 直前にタップがあれば、この触れ方は「タップ→長押し」の後半になりうる。 */
            /* 「タップ→長押し」は、解除中の再ロックにだけ使う(解除は、長押しで
             * ゾーン確定→フリック)。 */
            gesturepad_primed =
                gesturepad_armed &&
                now - gesturepad_last_tap_ms <= CONFIG_KOBITOKEY_GESTUREPAD_DOUBLE_TAP_MS;
        } else if (gesturepad_last_position >= 0 &&
                   abs(gesturepad_last_position - gesturepad_touch_start_pos) >
                       CONFIG_KOBITOKEY_GESTUREPAD_HOLD_TOLERANCE) {
            gesturepad_touch_moved = true;
        } else if (gesturepad_last_position < 0 && prev >= 0 && !gesturepad_touch_moved &&
                   !gesturepad_hold_done &&
                   now - gesturepad_touch_start_ms <= CONFIG_KOBITOKEY_GESTUREPAD_TAP_MS) {
            /* 1回のタップ(解除前も数える。「タップ→長押し」の前半になる)。 */
            gesturepad_count_tap(now);
        }

    }

    /*
     * 解除の2つめの方法: 長押し→フリック。1回の連続した触れの中で、
     *   1. 解除したいゾーンで、ZONE_HOLD_MS 止まる(止まった位置でゾーンが決まり、
     *      合図の振動が1発鳴る)
     *   2. そこから素早くはじく(フリック) → はじいた瞬間に解除
     * 離れの検出に頼らない(タップ→長押しは、眠りの直後にタップの離れをチップが
     * 報告せず、空振りした)。位置の変化は眠りの直後でも正しく報告される。
     * 握りは、止まっているだけでは何も起きず、そのあと意図してはじかないと
     * 解除されない。ゾーンは触れた場所で決まるので、狙いから逆算しなくていい。
     * 解除中は、なぞる操作と区別できないので、解除前だけ。
     */
    /* 触れている間の速さ(速かったほうを重く見る。離す直前に減速しても、
     * はじいた勢いを拾う)。 */
    if (gesturepad_last_position >= 0 && gesturepad_prev_position >= 0) {
        const int64_t tv = k_uptime_get();
        const int64_t dtv = tv - gesturepad_prime_last_ms;

        if (dtv > 0 && dtv <= GESTUREPAD_FLIP_STALE_MS) {
            const int32_t inst =
                (int32_t)((gesturepad_last_position - gesturepad_prev_position) * 1000 / dtv);

            gesturepad_prime_velocity = (abs(inst) > abs(gesturepad_prime_velocity))
                                            ? inst
                                            : (gesturepad_prime_velocity * 3 + inst) / 4;
        } else {
            gesturepad_prime_velocity = 0;
        }
        gesturepad_prime_last_ms = tv;
    }

    if (!gesturepad_armed && gesturepad_guard_over && !gesturepad_hold_done &&
        gesturepad_last_position >= 0) {
        const int64_t t = k_uptime_get();

        if (gesturepad_zone_pending) {
            /*
             * ゾーンが決まったあとの、フリック(素早くはじく動き)で解除する。
             * 速さと、動いた量の両方を見て、はじいた瞬間に解除する(止まるのは
             * 待たない)。解除の振動は、曲送りのフリックと同じ。
             */
            if (abs(gesturepad_prime_velocity) >=
                    CONFIG_KOBITOKEY_GESTUREPAD_FLICK_UNLOCK_SPEED &&
                abs(gesturepad_last_position - gesturepad_zone_pending_pos) >=
                    CONFIG_KOBITOKEY_GESTUREPAD_FLICK_UNLOCK_MIN_COUNTS) {
                LOG_INF("gesturepad: flick (%d counts/s) after hold; unlocking zone at %d",
                        (int)gesturepad_prime_velocity, gesturepad_zone_pending_pos);
                gesturepad_arm_at(gesturepad_zone_pending_pos,
                                  CONFIG_KOBITOKEY_GESTUREPAD_FLICK_HAPTIC_EFFECT);
                /* 解除の動きは、調整としては数えず、今の位置からなぞり始める。 */
                gesturepad_prev_position = gesturepad_last_position;
                gesturepad_tick_cell =
                    gesturepad_last_position / CONFIG_KOBITOKEY_GESTUREPAD_TICK_SPACING;
                gesturepad_flick_origin = gesturepad_last_position;
                gesturepad_zone_pending = false;
                gesturepad_hold_done = true;
                gesturepad_unlock_swallow = true;
                gesturepad_swallow_ref_pos = gesturepad_last_position;
                gesturepad_swallow_still_since_ms = t;
            } else if (t - gesturepad_zone_pending_ms >
                       CONFIG_KOBITOKEY_GESTUREPAD_FLICK_UNLOCK_WINDOW_MS) {
                /* はじかないまま時間が過ぎた: ゾーン決定を取り消して、数え直す。 */
                gesturepad_zone_pending = false;
                gesturepad_still_anchor = gesturepad_last_position;
                gesturepad_still_since_ms = t;
            }
        } else if (gesturepad_still_anchor < 0 ||
                   abs(gesturepad_last_position - gesturepad_still_anchor) >
                       CONFIG_KOBITOKEY_GESTUREPAD_HOLD_TOLERANCE) {
            gesturepad_still_anchor = gesturepad_last_position;
            gesturepad_still_since_ms = t;
        } else if (t - gesturepad_still_since_ms >= CONFIG_KOBITOKEY_GESTUREPAD_ZONE_HOLD_MS) {
            gesturepad_zone_pending = true;
            gesturepad_zone_pending_pos = gesturepad_still_anchor;
            gesturepad_zone_pending_ms = t;
            LOG_INF("gesturepad: zone chosen at %d; swipe to unlock", gesturepad_still_anchor);
#if defined(CONFIG_KOBITOKEY_HAPTIC)
            /* ゾーンが決まった合図(ここからスワイプしていい)。 */
            kobitokey_haptic_effect(CONFIG_KOBITOKEY_GESTUREPAD_ZONE_HAPTIC_EFFECT);
#endif
        }
    }

    if (gesturepad_armed) {
        /* 操作があるたびにロックまでの時間を数え直す(離したときも含む)。 */
        k_work_reschedule_for_queue(&gesturepad_workq, &gesturepad_lock_work,
                                    K_MSEC(CONFIG_KOBITOKEY_GESTUREPAD_LOCK_MS));
    }

    if (gesturepad_last_position < 0) {
        k_work_cancel_delayable(&gesturepad_arm_work);
        gesturepad_hold_anchor = -1;
        gesturepad_hold_done = false;
        gesturepad_primed = false;
        gesturepad_zone_pending = false;
    } else if (gesturepad_guard_over && gesturepad_primed && !gesturepad_hold_done &&
               (gesturepad_hold_anchor < 0 ||
                abs(gesturepad_last_position - gesturepad_hold_anchor) >
                    CONFIG_KOBITOKEY_GESTUREPAD_HOLD_TOLERANCE)) {
        /* 触れ始め、または押さえた位置から動いた: そこから数え直す。 */
        gesturepad_hold_anchor = gesturepad_last_position;
        k_work_reschedule_for_queue(&gesturepad_workq, &gesturepad_arm_work,
                                    K_MSEC(CONFIG_KOBITOKEY_GESTUREPAD_HOLD_MS));
    }

    /* 解除中に触れ直したら、そこからがフリックの起点。 */
    if (gesturepad_armed && gesturepad_last_position >= 0 &&
        gesturepad_prev_position < 0) {
        gesturepad_flick_origin = gesturepad_last_position;
        gesturepad_flick_fired = false;
    }

    /* 解除のフリックの余韻: 指が止まるまで、操作として数えない。 */
    bool swallowing = false;

    if (gesturepad_unlock_swallow && gesturepad_last_position >= 0) {
        const int64_t ts = k_uptime_get();

        if (abs(gesturepad_last_position - gesturepad_swallow_ref_pos) >= 2) {
            gesturepad_swallow_ref_pos = gesturepad_last_position;
            gesturepad_swallow_still_since_ms = ts;
        } else if (ts - gesturepad_swallow_still_since_ms >=
                   CONFIG_KOBITOKEY_GESTUREPAD_UNLOCK_SETTLE_MS) {
            gesturepad_unlock_swallow = false;
        }

        if (gesturepad_unlock_swallow) {
            swallowing = true;
            /* 今の位置を、次の基準にする。 */
            gesturepad_tick_cell =
                gesturepad_last_position / CONFIG_KOBITOKEY_GESTUREPAD_TICK_SPACING;
            gesturepad_flick_origin = gesturepad_last_position;
            gesturepad_velocity = 0;
            gesturepad_volume_remainder = 0;
            gesturepad_scroll_remainder = 0;
        } else {
            gesturepad_flick_origin = gesturepad_last_position;
            gesturepad_flick_fired = false;
        }
    }

    /*
     * tap モードのゾーンで解除したあと: パッド全体が3つの選択肢に分かれている。
     * 選びたい場所で TAP_HOLD_MS 動かさずに押さえると、合図の振動が入って、
     * その場所の選択肢が入力される。入力したら、すぐロックに戻る。
     * 1回の触れで1回だけ。
     */
    if (gesturepad_armed && !swallowing && gesturepad_last_position >= 0 &&
        gesturepad_zone_mode[gesturepad_zone] == GESTUREPAD_MODE_TAP && !gesturepad_tap_fired) {
        const int64_t tt = k_uptime_get();

        if (gesturepad_tap_anchor < 0 ||
            abs(gesturepad_last_position - gesturepad_tap_anchor) >
                CONFIG_KOBITOKEY_GESTUREPAD_HOLD_TOLERANCE) {
            gesturepad_tap_anchor = gesturepad_last_position;
            gesturepad_tap_since_ms = tt;
        } else if (tt - gesturepad_tap_since_ms >= CONFIG_KOBITOKEY_GESTUREPAD_TAP_HOLD_MS) {
            const int idx = gesturepad_tap_index(gesturepad_tap_anchor);

            LOG_INF("gesturepad: choice %d at %d", idx + 1, gesturepad_tap_anchor);
            gesturepad_tap_fired = true;
#if defined(CONFIG_KOBITOKEY_HAPTIC)
            kobitokey_haptic_effect(CONFIG_KOBITOKEY_GESTUREPAD_ZONE_HAPTIC_EFFECT);
#endif
            gesturepad_send_binding(idx);
            /* ロックには戻らない(続けて選べる。離してから8秒で、通常どおり閉じる)。 */
        }
    }

    /*
     * cycle モードのゾーンで解除したあと: 素早くはじく(フリック)と、選択肢を順に
     * 切り替える(上へ=次、下へ=前。一周する)。どこではじいてもよい。1回の触れで
     * 1回だけ。ロックには戻らない。ダブルタップは反対側(gesturepad_send_tap)。
     */
#if GESTUREPAD_IS_CENTRAL
    if (gesturepad_armed && !swallowing && gesturepad_last_position >= 0 &&
        gesturepad_zone_mode[gesturepad_zone] == GESTUREPAD_MODE_CYCLE && !gesturepad_tap_fired &&
        abs(gesturepad_prime_velocity) >= CONFIG_KOBITOKEY_GESTUREPAD_FLICK_UNLOCK_SPEED &&
        abs(gesturepad_last_position - gesturepad_touch_start_pos) >=
            CONFIG_KOBITOKEY_GESTUREPAD_FLICK_UNLOCK_MIN_COUNTS) {
        const int z = gesturepad_zone;
        const int n = gesturepad_choice_count(z);
        const int moved = gesturepad_last_position - gesturepad_touch_start_pos;
        /* 位置が大きいほうが上(右手)。左右の基板は向きが逆に付くので、反転する。 */
        const bool up = IS_ENABLED(CONFIG_KOBITOKEY_GESTUREPAD_ZONE_REVERSED) ? (moved < 0)
                                                                              : (moved > 0);

        gesturepad_cycle_idx[z] = (gesturepad_cycle_idx[z] + (up ? 1 : n - 1)) % n;
        gesturepad_cycle_flipped[z] = false;

        const struct zmk_behavior_binding *binding =
            &gesturepad_right_bindings[gesturepad_right_offset(z) + gesturepad_cycle_idx[z]];

        LOG_INF("gesturepad: cycle %s -> %d (%d counts/s)", up ? "next" : "prev",
                gesturepad_cycle_idx[z] + 1, (int)gesturepad_prime_velocity);
        gesturepad_tap_fired = true;
#if defined(CONFIG_KOBITOKEY_HAPTIC)
        kobitokey_haptic_effect(CONFIG_KOBITOKEY_GESTUREPAD_FLICK_HAPTIC_EFFECT);
#endif
        gesturepad_invoke(binding, true);
        k_msleep(CONFIG_KOBITOKEY_GESTUREPAD_KEY_HOLD_MS);
        gesturepad_invoke(binding, false);
    }
#endif

    if (gesturepad_armed && !swallowing && gesturepad_last_position >= 0 &&
        gesturepad_prev_position >= 0) {
        const int32_t delta = gesturepad_last_position - gesturepad_prev_position;

        {
            /* 速さ(カウント/秒)を、直近の値と平均して持つ。 */
            const int64_t t = k_uptime_get();
            const int64_t dt = t - gesturepad_last_sample_ms;

            if (dt > 0 && dt <= GESTUREPAD_FLIP_STALE_MS) {
                const int32_t inst = (int32_t)(delta * 1000 / dt);

                /*
                 * 直近の「速かった」ほうを重く見る。離す直前に指が少し
                 * 減速しても、はじいた勢いのほうを拾うため。速いときは
                 * そのまま採り、遅くなるときはゆっくり追従する。
                 */
                gesturepad_velocity = (abs(inst) > abs(gesturepad_velocity))
                                          ? inst
                                          : (gesturepad_velocity * 3 + inst) / 4;
            } else {
                gesturepad_velocity = 0;
            }
            gesturepad_last_sample_ms = t;
        }

        if (gesturepad_zone_mode[gesturepad_zone] == GESTUREPAD_MODE_TAP ||
            gesturepad_zone_mode[gesturepad_zone] == GESTUREPAD_MODE_CYCLE) {
            /* 選択肢のゾーンは、なぞる操作を持たない(上で選ぶ)。 */
        } else if (gesturepad_zone_mode[gesturepad_zone] == GESTUREPAD_MODE_FLICK) {
            const int moved = gesturepad_last_position - gesturepad_flick_origin;

            if (!gesturepad_flick_fired && gesturepad_flick_origin >= 0 &&
                abs(moved) >= CONFIG_KOBITOKEY_GESTUREPAD_FLICK_DISTANCE) {
                gesturepad_flick_fired = true;
                gesturepad_send_step(moved > 0);
#if defined(CONFIG_KOBITOKEY_HAPTIC)
                kobitokey_haptic_effect(CONFIG_KOBITOKEY_GESTUREPAD_FLICK_HAPTIC_EFFECT);
#endif
            }
        } else if (gesturepad_zone_mode[gesturepad_zone] == GESTUREPAD_MODE_SCROLL) {
            if (delta != 0) {
                gesturepad_scroll_by_distance(delta);
            }
        } else if (delta != 0) {
#if defined(CONFIG_KOBITOKEY_HAPTIC)
            /*
             * 鳴らすのは目盛りの境界をまたいだときだけ。エフェクトは
             * トラックボールのスクロール用(CONFIG_KOBITOKEY_HAPTIC_EFFECT)とは
             * 別に、専用の CONFIG_KOBITOKEY_GESTUREPAD_HAPTIC_EFFECT を使う。
             * 最短間隔(HAPTIC_COOLDOWN_MS)は kobitokey_haptic_effect() 経由で
             * 他の振動と共有される(意図的な仕様、kobitokey_haptic.c 参照)。
             */
            const int cell =
                gesturepad_last_position / CONFIG_KOBITOKEY_GESTUREPAD_TICK_SPACING;

            if (gesturepad_tick_cell >= 0 && cell != gesturepad_tick_cell) {
                kobitokey_haptic_effect(CONFIG_KOBITOKEY_GESTUREPAD_HAPTIC_EFFECT);
            }
            gesturepad_tick_cell = cell;
#endif
            gesturepad_volume_by_distance(delta);
        }
    }

    /*
     * 触れている間は、通知が来なくても読み続ける。眠りから起きた直後は、離した
     * ときの通知が出ないことがあり、タップの最中に読みに行かないと、離したのを
     * 見逃して「1.5秒押さえた」扱いになる(実機のログで、1発目のタップが
     * 空振りした原因)。
     */
    if (gesturepad_last_position >= 0) {
        k_work_reschedule_for_queue(&gesturepad_workq, &gesturepad_read_work,
                                    K_MSEC(GESTUREPAD_HOLD_POLL_MS));
    }

    /* タッチが離れたら基準を捨てる。次に触れた場所がどこであれ、
     * リフトオフ前の位置との差分をいきなり大きな1発として鳴らさない
     * ため。ノッチの余りも同じ理由で捨てる ── 前回のタッチの端数を
     * 次のタッチに持ち越さない。 */
    gesturepad_prev_position = gesturepad_last_position;
    if (gesturepad_last_position < 0) {
        gesturepad_volume_remainder = 0;
        gesturepad_scroll_remainder = 0;
        gesturepad_tick_cell = -1;
        gesturepad_flick_origin = -1;
        gesturepad_flick_fired = false;
        gesturepad_unlock_swallow = false;
        gesturepad_tap_fired = false;
        gesturepad_tap_anchor = -1;
    }
}

/*
 * チップの設定領域のCRC。CRC-16/CCITT-FALSE(多項式 0x1021、初期値 0xFFFF、
 * ビット反転なし、最終XORなし)を、0x00-0x7D の126バイトにかける。
 * 工場出荷の設定を実機から読み出して、保存されているCRC(0x5903)と一致する
 * ことを確かめてある。
 */
static uint16_t gesturepad_config_crc(const uint8_t *cfg)
{
    uint16_t crc = 0xffff;

    for (int i = 0; i < 126; i++) {
        crc ^= (uint16_t)cfg[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }

    return crc;
}

/* I2C。眠っているチップは最初のアクセスにNACKを返すので、ACKが来るまでやり直す。 */
static int gesturepad_read_retry(uint8_t reg, uint8_t *buf, size_t len)
{
    int ret = -EIO;

    for (int i = 0; i <= GESTUREPAD_READ_RETRY_MAX && ret != 0; i++) {
        ret = i2c_burst_read_dt(&gesturepad_i2c, reg, buf, len);
        if (ret != 0) {
            k_msleep(GESTUREPAD_READ_RETRY_MS);
        }
    }

    return ret;
}

static int gesturepad_write_retry(uint8_t reg, const uint8_t *buf, size_t len)
{
    int ret = -EIO;

    for (int i = 0; i <= GESTUREPAD_READ_RETRY_MAX && ret != 0; i++) {
        ret = i2c_burst_write_dt(&gesturepad_i2c, reg, buf, len);
        if (ret != 0) {
            k_msleep(GESTUREPAD_READ_RETRY_MS);
        }
    }

    return ret;
}

/*
 * チップの設定を、不揮発メモリへ保存して反映する(スライダーの指のしきい値)。
 *
 * 設定領域へのRAM書き込みは、保存してリセットするまで、チップの動作に効かない
 * (データシート)。以前、起動のたびに休止の設定をRAMへ書いていたが、実際には
 * 何も効いていなかった。
 *
 * 実機のログで、チップは最高感度のまま、スライダーの指のしきい値が128で、
 * 指を離しても生の信号が上限に張り付いて「触れている」と判定され続けて、
 * 最初の「タップ→長押し」が空振りした。しきい値を上げて、離れが早く検出
 * されるようにする。
 *
 * 安全策:
 *   - 家族IDが CY8CMBR3xxx(0x9A)でなければ何もしない。
 *   - しきい値がすでに目標値なら何もしない(書き込みは1回だけ)。
 *   - 今の設定とそのCRCが整合しているときだけ進める(想定外の設定は触らない)。
 *   - 保存はチップがCRCを検証し、合わなければ保存しない。保存の途中で電源が
 *     切れても、直前の有効な設定に戻る(データシート)。
 */
static void gesturepad_ensure_chip_config(void)
{
    const uint8_t target = CONFIG_KOBITOKEY_GESTUREPAD_CHIP_SLIDER_THRESHOLD;
    uint8_t cfg[0x80];
    uint8_t family = 0;

    if (gesturepad_read_retry(REG_FAMILY_ID, &family, 1) != 0 || family != 0x9a) {
        LOG_WRN("gesturepad: config skipped: family id 0x%02x", family);
        return;
    }

    for (int base = 0; base < 0x80; base += 16) {
        if (gesturepad_read_retry(base, &cfg[base], 16) != 0) {
            LOG_WRN("gesturepad: config skipped: read failed at 0x%02x", base);
            return;
        }
    }

    const uint16_t stored = (uint16_t)cfg[REG_CONFIG_CRC] | ((uint16_t)cfg[REG_CONFIG_CRC + 1] << 8);
    const uint16_t calc = gesturepad_config_crc(cfg);

    if (cfg[REG_SLIDER1_THRESHOLD] == target) {
        LOG_INF("gesturepad: chip slider threshold already %d", target);
        return;
    }

    if (calc != stored) {
        LOG_WRN("gesturepad: config skipped: crc mismatch (stored 0x%04x, calc 0x%04x)",
                stored, calc);
        return;
    }

    LOG_INF("gesturepad: saving slider threshold %d -> %d (crc 0x%04x)",
            cfg[REG_SLIDER1_THRESHOLD], target, stored);

    cfg[REG_SLIDER1_THRESHOLD] = target;

    const uint16_t crc = gesturepad_config_crc(cfg);
    const uint8_t crc_bytes[2] = {(uint8_t)(crc & 0xff), (uint8_t)(crc >> 8)};
    uint8_t err = 0xee;
    int ret = gesturepad_write_retry(REG_SLIDER1_THRESHOLD, &cfg[REG_SLIDER1_THRESHOLD], 1);

    if (ret == 0) {
        ret = gesturepad_write_retry(REG_CONFIG_CRC, crc_bytes, sizeof(crc_bytes));
    }
    if (ret == 0) {
        const uint8_t cmd = CTRL_CMD_SAVE_CHECK_CRC;

        ret = gesturepad_write_retry(REG_CTRL_CMD, &cmd, 1);
    }
    if (ret != 0) {
        LOG_WRN("gesturepad: config write failed (%d)", ret);
        return;
    }

    /* 保存には約220msかかり、その間チップはNACKを返す。 */
    k_msleep(400);
    ret = gesturepad_read_retry(REG_CTRL_CMD_ERR, &err, 1);
    LOG_INF("gesturepad: save result err=0x%02x (%d)", err, ret);

    if (ret != 0 || err != 0) {
        LOG_WRN("gesturepad: save failed; keeping the previous configuration");
        return;
    }

    /* 保存できたので、リセットして反映する。 */
    const uint8_t reset = CTRL_CMD_SW_RESET;

    (void)i2c_burst_write_dt(&gesturepad_i2c, REG_CTRL_CMD, &reset, 1);
    k_msleep(400);

    uint8_t check = 0;

    ret = gesturepad_read_retry(REG_SLIDER1_THRESHOLD, &check, 1);
    LOG_INF("gesturepad: slider threshold after reset = %d (%d)", check, ret);
}

static void gesturepad_boot_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    if (gesturepad_guard_over) {
        return;
    }

    if (!gesturepad_boot_recal_done && !gesturepad_config_checked) {
        gesturepad_config_checked = true;
        gesturepad_ensure_chip_config();
    }

    /* 2回目(リセット後の待ちが済んだ): 解除を受け付けるようにする。 */
    if (gesturepad_boot_recal_done) {
        gesturepad_guard_over = true;
        LOG_INF("gesturepad: boot guard over");
        return;
    }

    uint8_t raw_position = GESTUREPAD_NO_TOUCH;
    int ret = -EIO;

    /* 眠っているチップは最初のアクセスに応えないので、やり直す。 */
    for (int i = 0; i <= GESTUREPAD_READ_RETRY_MAX && ret != 0; i++) {
        ret = gesturepad_read_u8(REG_SLIDER1_POSITION, &raw_position);
        if (ret != 0) {
            k_msleep(GESTUREPAD_READ_RETRY_MS);
        }
    }

    if (ret == 0 && raw_position != GESTUREPAD_NO_TOUCH) {
        /* 触れている: 基準は壊さず、この指を離すまで長押しを数えない。 */
        LOG_INF("gesturepad: touched at boot; waiting for lift-off");
        gesturepad_hold_done = true;
        gesturepad_guard_over = true;
        return;
    }

#if defined(CONFIG_KOBITOKEY_GESTUREPAD_BOOT_RECAL)
    if (ret == 0) {
        LOG_INF("gesturepad: recalibrating (no touch at boot)");
        /* リセット中はNACKが返ることがあるが、構わない(実行はされる)。 */
        (void)i2c_reg_write_byte_dt(&gesturepad_i2c, REG_CTRL_CMD, CTRL_CMD_SW_RESET);
        k_work_reschedule_for_queue(&gesturepad_workq, &gesturepad_boot_work,
                                    K_MSEC(CONFIG_KOBITOKEY_GESTUREPAD_RECAL_SETTLE_MS));
        gesturepad_boot_recal_done = true;
        return;
    }
#endif

    if (ret == 0) {
    }

    gesturepad_guard_over = true;
    LOG_INF("gesturepad: boot guard over");
}

static void gesturepad_hi_interrupt_handler(
    const struct device *port,
    struct gpio_callback *callback,
    gpio_port_pins_t pins)
{
    ARG_UNUSED(port);
    ARG_UNUSED(callback);
    ARG_UNUSED(pins);

    if (gesturepad_last_position < 0) {
        gesturepad_hi_pending = true;
    }

    /*
     * 割り込みそのものからは絶対にI2Cを叩かない(haptic と同じ理由 ──
     * バスが詰まった時の巻き添えを割り込みコンテキストから起こさない)。
     * 専用ワークキューに逃がして、そこでだけI2Cを触る。
     */
    k_work_reschedule_for_queue(&gesturepad_workq, &gesturepad_read_work,
                                K_MSEC(GESTUREPAD_DEBOUNCE_MS));
}

int kobitokey_gesturepad_position(void)
{
    return gesturepad_last_position;
}

static int gesturepad_init(void)
{
    int ret;

    if (!device_is_ready(gesturepad_i2c.bus)) {
        LOG_ERR("Gesture pad I2C bus is not ready");
        return -ENODEV;
    }

    if (!gpio_is_ready_dt(&gesturepad_hi)) {
        LOG_ERR("Gesture pad HI pin is not ready");
        return -ENODEV;
    }

    ret = gpio_pin_configure_dt(&gesturepad_hi, GPIO_INPUT);
    if (ret < 0) {
        LOG_ERR("Failed to configure gesture pad HI pin: %d", ret);
        return ret;
    }

    k_work_queue_init(&gesturepad_workq);
    k_work_queue_start(&gesturepad_workq, gesturepad_workq_stack,
                       K_THREAD_STACK_SIZEOF(gesturepad_workq_stack),
                       K_PRIO_PREEMPT(10),
                       &(struct k_work_queue_config){ .name = "gesturepad" });

    k_work_init_delayable(&gesturepad_read_work, gesturepad_read_work_handler);
    k_work_init_delayable(&gesturepad_arm_work, gesturepad_arm_work_handler);
    k_work_init_delayable(&gesturepad_boot_work, gesturepad_boot_work_handler);
#if defined(CONFIG_RGBLED_WIDGET) && SHOW_LAYER_COLORS
    k_work_init_delayable(&gesturepad_indicator_work, gesturepad_indicator_work_handler);
#endif
    k_work_init_delayable(&gesturepad_lock_work, gesturepad_lock_work_handler);

    gpio_init_callback(&gesturepad_hi_callback, gesturepad_hi_interrupt_handler,
                       BIT(gesturepad_hi.pin));

    ret = gpio_add_callback(gesturepad_hi.port, &gesturepad_hi_callback);
    if (ret < 0) {
        LOG_ERR("Failed to add gesture pad HI callback: %d", ret);
        return ret;
    }

    /* HI/BUZ はアクティブLOWのパルス出力(プッシュプル、プルアップ不要)。
     * devicetree 側の GPIO_ACTIVE_LOW と合わせて、論理的な「アクティブへ
     * 遷移」で取る(物理的な立ち下がりと自動的に一致する)。 */
    ret = gpio_pin_interrupt_configure_dt(&gesturepad_hi, GPIO_INT_EDGE_TO_ACTIVE);
    if (ret < 0) {
        LOG_ERR("Failed to configure gesture pad HI interrupt: %d", ret);
        gpio_remove_callback(gesturepad_hi.port, &gesturepad_hi_callback);
        return ret;
    }

    /* 起動から BOOT_GUARD_MS 後(この初期化は既にその一部を使っている)。 */
    {
        const int64_t wait_ms =
            CONFIG_KOBITOKEY_GESTUREPAD_BOOT_GUARD_MS - k_uptime_get();

        k_work_schedule_for_queue(&gesturepad_workq, &gesturepad_boot_work,
                                  K_MSEC(wait_ms > 0 ? wait_ms : 0));
    }

    LOG_INF("Gesture pad ready (I2C addr 0x%02x); "
            "sensor activation/thresholds not yet configured",
            gesturepad_i2c.addr);

    return 0;
}

/*
 * POST_KERNEL 55: haptic と同じ理由でI2C(50)/GPIO(40)の後に置く。
 * 両者は i2c1 を共有するが、初期化の順序自体はどちらが先でも構わない
 * (Zephyr の I2C サブシステムがバスアクセスを直列化する)。
 */
#define GESTUREPAD_INIT_PRIORITY 55
BUILD_ASSERT(GESTUREPAD_INIT_PRIORITY > CONFIG_I2C_INIT_PRIORITY &&
                 GESTUREPAD_INIT_PRIORITY > CONFIG_GPIO_INIT_PRIORITY,
             "The gesture pad must initialise after the buses it talks over");

SYS_INIT(gesturepad_init, POST_KERNEL, GESTUREPAD_INIT_PRIORITY);
