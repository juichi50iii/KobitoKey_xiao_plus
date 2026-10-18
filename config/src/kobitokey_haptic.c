#include <stdbool.h>
#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#if DT_HAS_COMPAT_STATUS_OKAY(kobitokey_haptic_local_input)
#include <zephyr/input/input.h>
#endif

#include "kobitokey_haptic.h"

#if defined(CONFIG_KOBITOKEY_VBUS_SENSE)
#include "kobitokey_vbus.h"
#endif

LOG_MODULE_REGISTER(kobitokey_haptic, LOG_LEVEL_INF);

#define HAPTIC_NODE DT_NODELABEL(haptic_drv)
BUILD_ASSERT(DT_NODE_HAS_STATUS(HAPTIC_NODE, okay),
             "haptic_drv is missing: the DRV2605L must be in the devicetree");

/*
 * nRF52840 では TWIM/SPIM が番号ごとに同じペリフェラルを共有する。
 * トラックボールが spi0 を使っているので振動は i2c1 に置いたが、
 * その i2c1 は spi1 と排他だ。両方が有効になっても黙って壊れるだけで
 * 誰も教えてくれないので、ここで止める。
 */
#if DT_NODE_EXISTS(DT_NODELABEL(spi1))
BUILD_ASSERT(!DT_NODE_HAS_STATUS(DT_NODELABEL(spi1), okay),
             "i2c1 and spi1 share a peripheral: only one can be enabled");
#endif

static const struct i2c_dt_spec haptic_i2c = I2C_DT_SPEC_GET(HAPTIC_NODE);
static const struct gpio_dt_spec haptic_en =
    GPIO_DT_SPEC_GET(HAPTIC_NODE, en_gpios);

/*
 * ERMをGPIOで直接叩いていた頃の名残り。
 *
 * 旧基板は P1.11 をHIGHにしている間だけモーターが回る配線で、
 * 「何ミリ秒回すか」がそのまま振動の長さだった。
 * 新基板ではその P1.11 は I2C の SDA で、振動は DRV2605L に
 * 「エフェクト番号Nを再生しろ」と指示する形になった。
 *
 * 重要なのは、チップが自分で再生を終えることだ。
 * 消すための書き込みが要らないので、消す処理がキューに詰まって
 * 振動が伸びるという以前の事故が、構造として起きなくなっている。
 */

#define DRV_REG_STATUS        0x00
#define DRV_REG_MODE          0x01
#define DRV_REG_LIBRARY       0x03
#define DRV_REG_WAVEFORM(n)   (0x04 + (n))
#define DRV_REG_GO            0x0C
#define DRV_REG_RATED_VOLTAGE 0x16
#define DRV_REG_OD_CLAMP      0x17
#define DRV_REG_A_CAL_COMP    0x18
#define DRV_REG_A_CAL_BEMF    0x19
#define DRV_REG_FEEDBACK      0x1A
#define DRV_REG_CONTROL1      0x1B
#define DRV_REG_CONTROL3      0x1D
#define DRV_REG_CONTROL4      0x1E
#define DRV_REG_LRA_PERIOD    0x22

#define DRV_MODE_INTERNAL_TRIG 0x00
#define DRV_MODE_AUTO_CAL      0x07
#define DRV_MODE_STANDBY       0x40

#define DRV_LIBRARY_LRA 0x06

/*
 * FEEDBACK_CONTROL (0x1A) を組み立てる。
 *
 *   bit7    N_ERM_LRA        1 = LRA
 *   bit6:4  FB_BRAKE_FACTOR  0=1x 1=2x 2=3x 3=4x 4=6x 5=8x 6=16x 7=無効
 *   bit3:2  LOOP_GAIN        0=low 1=medium 2=high 3=very high
 *   bit1:0  BEMF_GAIN        較正が入れる
 *
 * ブレーキ係数はLRAの「キレ」を直接決める。止めたい瞬間に逆向きに
 * どれだけ強く叩くかで、大きいほど余韻が短い。
 */
#define DRV_FEEDBACK_LRA                                        \
    (0x80 |                                                     \
     ((CONFIG_KOBITOKEY_HAPTIC_BRAKE_FACTOR & 0x07) << 4) |     \
     ((CONFIG_KOBITOKEY_HAPTIC_LOOP_GAIN & 0x03) << 2))

#define DRV_STATUS_DIAG_RESULT BIT(3)

/*
 * 波形スロットに埋め込む「待ち」。0x80以上は待ち時間として扱われ、
 * 下位7bitが10ms単位。+5は四捨五入。ブートパターン(長い1回→短い1回
 * →短い1回)を1回のGOにまとめて渡すために使う。
 */
#define DRV_WAIT_MAX_MS 1270
#define DRV_WAIT(ms)    (0x80 | (uint8_t)(((ms) + 5) / 10))

#define HAPTIC_COOLDOWN_MS CONFIG_KOBITOKEY_HAPTIC_COOLDOWN_MS
#define HAPTIC_BOOT_DELAY_MS CONFIG_KOBITOKEY_HAPTIC_BOOT_DELAY_MS

/*
 * 刻みの間隔を測るときに信用する最短の時間。
 *
 * トラックボールのドライバは一定の周期で報告するが、イベントが
 * その周期どおりに届くとは限らない。XとYが同じ報告から出れば
 * 二つはほぼ同時刻に並ぶし、リンクが詰まれば二つがまとめて来る。
 * 1msの隙間で割ると、ふつうの動きが何倍もの速さに化ける。
 * ドライバの周期より短い隙間はその周期として扱う。
 */
#define HAPTIC_SPEED_MIN_INTERVAL_MS 10

/* EN low to first transfer. The datasheet asks for 250us; a millisecond
 * costs nothing and only happens at boot. */
#define HAPTIC_ENABLE_SETTLE_MS 1

/* Auto-calibration is specified well under a second. */
#define HAPTIC_AUTOCAL_TIMEOUT_MS 1500


static int64_t last_haptic_time;
static bool haptic_ready;

#if !IS_ENABLED(CONFIG_KOBITOKEY_HAPTIC_TICK_BY_DISTANCE)
/* 直前に刻みを求められた時刻と、そこから起こした速度。 */
static int64_t last_pulse_request_time;
static uint32_t pulse_speed;
#endif

/*
 * ワークキューとENピンの用意ができているか。
 *
 * 初期化されていない k_work を cancel したり、出力に設定していないピンを
 * 叩いたりしないための目印。初期化の順番は動くものなので、順番が正しい
 * ことを前提にせず、その場で確かめる。
 */
static bool haptic_initialized;

/* Effect requested but not yet written out. Read and cleared by the
 * haptic thread, written by whoever asked for a buzz. */
static atomic_t haptic_pending_effect;
static atomic_t haptic_pending_gap_ms; /* 0 = ひとつだけ、それ以外 = 同じものを間を空けて2回 */

static struct k_work_q haptic_workq;
static K_THREAD_STACK_DEFINE(haptic_workq_stack,
                             CONFIG_KOBITOKEY_HAPTIC_WORKQ_STACK_SIZE);
static struct k_work haptic_play_work;
static struct k_work_delayable haptic_setup_work;


static int drv_write(uint8_t reg, uint8_t value)
{
    return i2c_reg_write_byte_dt(&haptic_i2c, reg, value);
}


static int drv_read(uint8_t reg, uint8_t *value)
{
    return i2c_reg_read_byte_dt(&haptic_i2c, reg, value);
}


static void haptic_enable_set(bool on)
{
    (void)gpio_pin_set_dt(&haptic_en, on ? 1 : 0);
}


/*
 * 波形スロットを書いてGOを立てる。
 *
 * slots は 0 終端。最大8スロットで、超えた分は書かない。
 */
static int drv_play(const uint8_t *slots, size_t count)
{
    size_t i;

    for (i = 0; i < count && i < 8; i++) {
        int ret = drv_write(DRV_REG_WAVEFORM(i), slots[i]);

        if (ret != 0) {
            return ret;
        }
    }

    if (i < 8) {
        int ret = drv_write(DRV_REG_WAVEFORM(i), 0);

        if (ret != 0) {
            return ret;
        }
    }

    return drv_write(DRV_REG_GO, 1);
}


static int drv_play_effect(uint8_t effect)
{
    const uint8_t slots[] = { effect };

    return drv_play(slots, ARRAY_SIZE(slots));
}


#if IS_ENABLED(CONFIG_KOBITOKEY_HAPTIC_AUTOCAL)

/*
 * 自動キャリブレーション。
 *
 * モーターを実際に振って、補償・逆起電力・ゲインの各レジスタを
 * チップ自身に決めさせる。結果はログに出すので、値が安定したら
 * KOBITOKEY_HAPTIC_AUTOCAL を n にして直接書き込んでよい。
 */
static int drv_autocal(void)
{
    const int64_t deadline = k_uptime_get() + HAPTIC_AUTOCAL_TIMEOUT_MS;
    uint8_t status = 0;
    uint8_t comp = 0;
    uint8_t bemf = 0;
    uint8_t feedback = 0;
    int ret;

    ret = drv_write(DRV_REG_MODE, DRV_MODE_AUTO_CAL);
    if (ret != 0) {
        return ret;
    }

    ret = drv_write(DRV_REG_GO, 1);
    if (ret != 0) {
        return ret;
    }

    for (;;) {
        uint8_t go = 0;

        ret = drv_read(DRV_REG_GO, &go);
        if (ret != 0) {
            return ret;
        }

        if ((go & 0x01) == 0) {
            break;
        }

        if (k_uptime_get() > deadline) {
            LOG_ERR("Haptic auto-calibration did not finish");
            return -ETIMEDOUT;
        }

        k_sleep(K_MSEC(10));
    }

    ret = drv_read(DRV_REG_STATUS, &status);
    if (ret != 0) {
        return ret;
    }

    if (status & DRV_STATUS_DIAG_RESULT) {
        /* The chip could not characterise the motor: usually the LRA is
         * not connected, or the drive is clamped too low to move it. */
        LOG_ERR("Haptic auto-calibration failed (status 0x%02x)", status);
        return -EIO;
    }

    (void)drv_read(DRV_REG_A_CAL_COMP, &comp);
    (void)drv_read(DRV_REG_A_CAL_BEMF, &bemf);
    (void)drv_read(DRV_REG_FEEDBACK, &feedback);

    LOG_INF("Haptic calibrated: comp=%u bemf=%u bemf_gain=%u",
            comp, bemf, feedback & 0x03);

    return 0;
}

#endif /* CONFIG_KOBITOKEY_HAPTIC_AUTOCAL */


/*
 * Kconfigに書いてある較正値を入れる。
 *
 * 自動較正を走らせる余裕がない経路のための出発点でもある。既定値のままの
 * チップでもエフェクトは鳴るが、共振から外れた分だけ弱く濁る。一度
 * 自動較正を通してログの値をここに写しておけば、どちらの経路でも同じ
 * 手応えになる。
 */
static int haptic_write_calibration(void)
{
    int ret;

    ret = drv_write(DRV_REG_A_CAL_COMP, CONFIG_KOBITOKEY_HAPTIC_CAL_COMP);
    if (ret != 0) {
        return ret;
    }

    ret = drv_write(DRV_REG_A_CAL_BEMF, CONFIG_KOBITOKEY_HAPTIC_CAL_BEMF);
    if (ret != 0) {
        return ret;
    }

    return drv_write(DRV_REG_FEEDBACK,
                     DRV_FEEDBACK_LRA |
                         (CONFIG_KOBITOKEY_HAPTIC_CAL_BEMF_GAIN & 0x03));
}


/*
 * 実測された共振周期をログに出す。
 *
 * 較正が通ったかどうかとは無関係に読める。DRV2605L は再生中も
 * オートレゾナンスで共振を追い、このレジスタを更新し続けるからだ。
 * むしろ実際に鳴らした後に読む方が、「動作時にどこで共振しているか」
 * という知りたいことに近い。
 *
 * DRIVE_TIME はこの周期の半分に合わせるものなので、推奨値も一緒に出す。
 * データシートの公称値でも他人の設定でもなく、目の前の個体の値で決める
 * ためにこれがある。
 */
static void haptic_log_resonance(void)
{
    uint8_t period = 0;

    if (drv_read(DRV_REG_LRA_PERIOD, &period) != 0 || period == 0) {
        LOG_WRN("Haptic resonance could not be read");
        return;
    }

    /* 1周期 = period * 98.46us */
    const uint32_t period_us = (period * 9846U) / 100U;
    const uint32_t freq_x100 = (100U * 1000000U) / period_us;
    const uint32_t half_us = period_us / 2U;
    const uint32_t drive_time = (half_us > 500U) ? ((half_us - 500U) / 100U) : 0U;

    LOG_INF("Haptic resonance: period=%u (%u.%02u Hz), half %u us"
            " -> DRIVE_TIME %u",
            period, freq_x100 / 100U, freq_x100 % 100U, half_us, drive_time);
}


static int haptic_bring_up(bool run_autocal)
{
    int ret;

    /* Out of standby, and into the mode the sequencer runs in. */
    ret = drv_write(DRV_REG_MODE, DRV_MODE_INTERNAL_TRIG);
    if (ret != 0) {
        return ret;
    }

    ret = drv_write(DRV_REG_FEEDBACK, DRV_FEEDBACK_LRA);
    if (ret != 0) {
        return ret;
    }

    ret = drv_write(DRV_REG_RATED_VOLTAGE,
                    CONFIG_KOBITOKEY_HAPTIC_RATED_VOLTAGE);
    if (ret != 0) {
        return ret;
    }

    ret = drv_write(DRV_REG_OD_CLAMP, CONFIG_KOBITOKEY_HAPTIC_OD_CLAMP);
    if (ret != 0) {
        return ret;
    }

    /* Closed loop, and the drive shape the LRA library expects. */
    ret = drv_write(DRV_REG_CONTROL3, 0xA0);
    if (ret != 0) {
        return ret;
    }

    /*
     * 駆動時間をこのLRAの半周期に合わせる。
     *
     * 自動較正はこれを起点に共振点を探す。初期値のままだと 2.4ms、
     * つまり約208Hz のモーターを想定した探索になり、240Hz の実物では
     * 最大の逆起電力に届かないまま失敗する。ここを書かずに較正だけ
     * 走らせていたのが status 0xec の正体だった。
     *
     * bit7 の STARTUP_BOOST は残す（立ち上がりを速くする側の設定）。
     */
    ret = drv_write(DRV_REG_CONTROL1,
                    0x80 | (CONFIG_KOBITOKEY_HAPTIC_DRIVE_TIME & 0x1F));
    if (ret != 0) {
        return ret;
    }

    /* 測定に使える時間。較正は起動時の一度きりなので急ぐ理由がない。 */
    ret = drv_write(DRV_REG_CONTROL4,
                    (CONFIG_KOBITOKEY_HAPTIC_AUTO_CAL_TIME & 0x03) << 4);
    if (ret != 0) {
        return ret;
    }

    /* Start from the written-down values either way: if calibration is
     * about to run it overwrites them, and if it is skipped they are what
     * the motor is driven with. */
    ret = haptic_write_calibration();
    if (ret != 0) {
        return ret;
    }

#if IS_ENABLED(CONFIG_KOBITOKEY_HAPTIC_AUTOCAL)
    if (run_autocal) {
        /*
         * 較正に失敗しても黙らせない。
         *
         * 以前はここで諦めてENを落としていたが、それは捨てすぎだった。
         * 較正が失敗するのは「測りきれなかった」という意味で、モーターが
         * 動かないという意味ではない。現に較正そのものが motor を振って
         * いる。書いてある値のまま鳴らせば、精度は落ちるが手応えは残る。
         *
         * 完全な無音は、原因を探すときに一番手がかりの少ない状態でもある。
         */
        ret = drv_autocal();
        if (ret != 0) {
            LOG_WRN("Falling back to the written-down calibration");

            ret = haptic_write_calibration();
            if (ret != 0) {
                return ret;
            }
        }

        /* Auto-calibration leaves the part in its own mode. */
        ret = drv_write(DRV_REG_MODE, DRV_MODE_INTERNAL_TRIG);
        if (ret != 0) {
            return ret;
        }
    }
#else
    ARG_UNUSED(run_autocal);
#endif

    return drv_write(DRV_REG_LIBRARY, DRV_LIBRARY_LRA);
}


/*
 * その場で立ち上げて、その場で鳴らして、鳴り終わるまで待つ。
 *
 * 「閉じたままUSBを挿した」起動のためにある。あの経路は蓋を確かめたら
 * すぐSystem OFFに入るので、500ms後に予約された通常のセットアップは
 * 永遠に走らない。かといって鳴らせない道理はなく、I2Cドライバは
 * POST_KERNEL 50 で、蓋を判定する POST_KERNEL 95 の時点でもう動いている。
 *
 * 自動較正はここでは走らせない。1秒近くかかる上にそれ自体が唸るので、
 * 通知としては邪魔になる。書いてある較正値だけ入れて鳴らす。
 */
static bool haptic_play_and_wait_now(uint8_t effect)
{
    const int64_t deadline =
        k_uptime_get() + CONFIG_KOBITOKEY_HAPTIC_PLAY_SETTLE_MS;

    if (!device_is_ready(haptic_i2c.bus)) {
        return false;
    }

    if (!haptic_initialized) {
        /* Called before haptic_init(): the pin is not an output yet. */
        if (!gpio_is_ready_dt(&haptic_en)) {
            return false;
        }
        (void)gpio_pin_configure_dt(&haptic_en, GPIO_OUTPUT_INACTIVE);
    }

    haptic_enable_set(true);
    k_busy_wait(HAPTIC_ENABLE_SETTLE_MS * 1000U);

    if (haptic_bring_up(false) != 0) {
        LOG_ERR("Haptic could not be brought up in time for the buzz");
        haptic_enable_set(false);
        return false;
    }

    if (drv_play_effect(effect) != 0) {
        haptic_enable_set(false);
        return false;
    }

    /*
     * Hold here until the chip says it is done, so the caller does not cut
     * the power out from under a buzz that has only just started. Polling
     * GO rather than sleeping a guessed length returns as soon as the
     * effect actually ends.
     */
    for (;;) {
        uint8_t go = 0;

        if (k_uptime_get() > deadline) {
            break;
        }

        if (drv_read(DRV_REG_GO, &go) != 0) {
            break;
        }

        if ((go & 0x01) == 0) {
            break;
        }

        k_busy_wait(2000U);
    }

    last_haptic_time = k_uptime_get();

    return true;
}


/*
 * バッテリー起動時のブート振動。
 *
 * 長い1回 → GAP1 → 短い1回 → GAP2 → 短い1回(ブーブッブッ)。
 *
 * チップの波形シーケンサに、効果と待ちを交互に並べた1本のスロット列
 * として丸ごと預ける。GOを立てた時点でこちらの仕事は終わり、鳴らし
 * 終わるまで待つ必要も、次の一発を自分で予約し直す必要もない。
 * 5スロット(効果・待ち・効果・待ち・効果)なので、8スロットの上限にも
 * 余裕がある。
 */
static void haptic_play_boot_pattern(void)
{
    const uint8_t slots[] = {
        CONFIG_KOBITOKEY_HAPTIC_BOOT_EFFECT_LONG,
        DRV_WAIT(MIN(CONFIG_KOBITOKEY_HAPTIC_BOOT_GAP1_MS, DRV_WAIT_MAX_MS)),
        CONFIG_KOBITOKEY_HAPTIC_BOOT_EFFECT_SHORT,
        DRV_WAIT(MIN(CONFIG_KOBITOKEY_HAPTIC_BOOT_GAP2_MS, DRV_WAIT_MAX_MS)),
        CONFIG_KOBITOKEY_HAPTIC_BOOT_EFFECT_SHORT,
    };

    (void)drv_play(slots, ARRAY_SIZE(slots));
    last_haptic_time = k_uptime_get();
}


#if CONFIG_KOBITOKEY_HAPTIC_CAL_SURVEY > 0

/*
 * 較正を繰り返して、値が再現するかを見る。
 *
 * 1回通っただけでは、その回が何を掴んだのか分からない。実際この個体は
 * DIAG_RESULT=0 を返しながら bemf=22 / 共振508Hz という、明らかに倍音を
 * 掴んだ結果を「成功」と報告している。
 *
 * 共振はエフェクトを鳴らしている最中に読む。0x22 はループが駆動している
 * 間に更新されるレジスタで、鳴り終わった後の値は当てにならない。前回
 * この読み方を間違えて、236Hz という数字を実測と呼んでしまった。
 */
static void haptic_calibration_survey(void)
{
    LOG_INF("--- calibration survey: %d runs ---",
            CONFIG_KOBITOKEY_HAPTIC_CAL_SURVEY);

    for (int i = 0; i < CONFIG_KOBITOKEY_HAPTIC_CAL_SURVEY; i++) {
        uint8_t comp = 0, bemf = 0, fb = 0, period = 0;
        const int cal = drv_autocal();

        (void)drv_read(DRV_REG_A_CAL_COMP, &comp);
        (void)drv_read(DRV_REG_A_CAL_BEMF, &bemf);
        (void)drv_read(DRV_REG_FEEDBACK, &fb);

        /* 鳴らしている最中に共振を読む */
        (void)drv_write(DRV_REG_MODE, DRV_MODE_INTERNAL_TRIG);
        (void)drv_play_effect(118);      /* long buzz */
        k_sleep(K_MSEC(60));
        (void)drv_read(DRV_REG_LRA_PERIOD, &period);
        (void)drv_write(DRV_REG_GO, 0);

        const uint32_t hz_x100 =
            period ? (100U * 1000000U) / ((period * 9846U) / 100U) : 0U;

        LOG_INF("run %d: %s comp=%u bemf=%u gain=%u period=%u (%u.%02u Hz)",
                i + 1, (cal == 0) ? "pass" : "FAIL",
                comp, bemf, fb & 0x03, period,
                hz_x100 / 100U, hz_x100 % 100U);

        k_sleep(K_MSEC(300));
    }

    LOG_INF("--- survey done ---");
}

#endif


static void haptic_setup_work_handler(struct k_work *work)
{
    int ret;

    ARG_UNUSED(work);

    haptic_enable_set(true);
    k_sleep(K_MSEC(HAPTIC_ENABLE_SETTLE_MS));

    if (!device_is_ready(haptic_i2c.bus)) {
        LOG_ERR("Haptic I2C bus is not ready");
        haptic_enable_set(false);
        return;
    }

    ret = haptic_bring_up(true);
    if (ret != 0) {
        LOG_ERR("Haptic driver setup failed (%d); staying silent", ret);
        haptic_enable_set(false);
        return;
    }

    haptic_ready = true;

#if CONFIG_KOBITOKEY_HAPTIC_CAL_SURVEY > 0
    haptic_calibration_survey();
#endif

    haptic_play_boot_pattern();
}


static void haptic_play_work_handler(struct k_work *work)
{
    const uint8_t effect = (uint8_t)atomic_set(&haptic_pending_effect, 0);
    const uint32_t gap_ms = (uint32_t)atomic_set(&haptic_pending_gap_ms, 0);

    ARG_UNUSED(work);

    if (effect == 0 || !haptic_ready) {
        return;
    }

    if (gap_ms > 0) {
        /* 同じエフェクトを、間を空けて2回。内蔵の2連打は間隔が固定なので、
         * 起動のパターンと同じく、波形スロットに待ちを挟んで組む。 */
        const uint8_t slots[] = {
            effect,
            DRV_WAIT(MIN(gap_ms, DRV_WAIT_MAX_MS)),
            effect,
        };

        (void)drv_play(slots, ARRAY_SIZE(slots));
        return;
    }

    (void)drv_play_effect(effect);
}


/*
 * 鳴らす要求を出すだけ。I2Cは専用スレッドが受け持つ。
 *
 * 入力経路からは絶対にI2Cを叩かない。バスが詰まったときに巻き添えで
 * ポインタが止まるのが、この基板で一番避けたい失敗だからだ。
 */
static void haptic_request(uint8_t effect)
{
    if (!haptic_ready) {
        return;
    }

    atomic_set(&haptic_pending_gap_ms, 0);
    atomic_set(&haptic_pending_effect, effect);
    (void)k_work_submit_to_queue(&haptic_workq, &haptic_play_work);
}


#if IS_ENABLED(CONFIG_KOBITOKEY_HAPTIC_TICK_BY_DISTANCE)

/*
 * 符号付きの位置を、ノッチ幅で割った余りとして持つ。
 *
 * 前に進んで境目を跨いだら余りがノッチ幅を超え、そこで1つ鳴らして
 * 余りを戻す。戻る方向に動けば余りはマイナスに寄っていき、同じ境目を
 * また跨げば再び鳴る。境目の手前で行ったり戻ったりしても、跨がない
 * 限りは鳴らない ── 距離を積算するのではなく、位置がどちらの側に
 * いるかで判定しているからだ。
 */
static int32_t haptic_notch_remainder;

static void haptic_pulse_by_distance(int32_t value)
{
    /*
     * 余りは生カウントのまま持つ ── これは正確さのために崩さない。
     * 「共通単位」に換算するのは閾値の側で、TICK_NOTCH に半身ごとの
     * TICK_SCALE を掛けたものを、実際に比べる幅として使う。
     */
    const int32_t notch = (int32_t)CONFIG_KOBITOKEY_HAPTIC_TICK_NOTCH *
                           (int32_t)CONFIG_KOBITOKEY_HAPTIC_TICK_SCALE;

    if (notch <= 0) {
        return;
    }

    haptic_notch_remainder += value;

    while (haptic_notch_remainder >= notch) {
        haptic_notch_remainder -= notch;
        kobitokey_haptic_effect(CONFIG_KOBITOKEY_HAPTIC_EFFECT);
    }

    while (haptic_notch_remainder <= -notch) {
        haptic_notch_remainder += notch;
        kobitokey_haptic_effect(CONFIG_KOBITOKEY_HAPTIC_EFFECT);
    }
}

#else

/*
 * いまの速さに見合う、刻みと刻みのあいだの最短時間。
 *
 * 遅いうちは広く、速くなるほど狭くなる。詰めているのは間隔であって
 * 毎秒の回数ではない。回数は間隔の逆数なので、間隔を直線で詰めると
 * 回数のほうは勝手に加速する。範囲の半分まで来ても回数はまだ遅い側に
 * 寄っていて、伸びの大半は上のほうに乗る。盛り上がって聞こえるのは
 * この非対称のおかげで、曲線を足さなくてもこれだけで出る。
 *
 * 遅い側と速い側が逆転していたら、間隔を固定にして黙って従う。
 * 設定の書き間違いで刻みが消えるより、変化しないほうがましだ。
 */
static uint32_t haptic_cooldown_for_speed(uint32_t speed)
{
    const uint32_t widest = CONFIG_KOBITOKEY_HAPTIC_COOLDOWN_SLOW_MS;
    const uint32_t tightest = HAPTIC_COOLDOWN_MS;
    const uint32_t slow = CONFIG_KOBITOKEY_HAPTIC_COOLDOWN_SLOW_SPEED;
    const uint32_t fast = CONFIG_KOBITOKEY_HAPTIC_COOLDOWN_FAST_SPEED;

    if (widest <= tightest || fast <= slow) {
        return tightest;
    }

    if (speed <= slow) {
        return widest;
    }

    if (speed >= fast) {
        return tightest;
    }

    /*
     * 範囲のどこまで来たかを1000倍で持ち、曲線の指数だけ掛ける。
     * 掛けるたびに割り戻すのは、指数3で桁が10^9に届いて32bitを
     * 溢れさせないため。
     */
    const uint32_t span = fast - slow;
    const uint32_t into = speed - slow;
    uint64_t shaped = 1000;

    for (int i = 0; i < CONFIG_KOBITOKEY_HAPTIC_COOLDOWN_CURVE; i++) {
        shaped = shaped * into / span;
    }

    return widest - (uint32_t)((uint64_t)(widest - tightest) * shaped / 1000);
}


/*
 * 速さを覚えなおす。
 *
 * 刻みを鳴らすかどうかに関わらず、求められたら必ず通す。ここを
 * 鳴らしたときだけにすると、速さを間引いた結果でしか測れなくなり、
 * 間隔を決める材料が間隔そのものに引きずられて回らなくなる。
 */
static void haptic_note_speed(int32_t value)
{
    const int64_t now = k_uptime_get();
    const int64_t since =
        last_pulse_request_time > 0 ? now - last_pulse_request_time : 0;

    last_pulse_request_time = now;

    /*
     * 間があいたら、そこで一度終わったものとみなして速さを捨てる。
     * 次の一転がしは必ず広い間隔から始まって、また詰まっていく。
     */
    if (since <= 0 || since >= CONFIG_KOBITOKEY_HAPTIC_COOLDOWN_IDLE_MS) {
        pulse_speed = 0;
        return;
    }

    const int64_t interval = MAX(since, (int64_t)HAPTIC_SPEED_MIN_INTERVAL_MS);
    const uint32_t instant =
        (uint32_t)((int64_t)(value < 0 ? -value : value) * 1000 / interval);

    /* 直前の値を3、今回を1で混ぜる。単発の跳ねで間隔が揺れない。 */
    pulse_speed = (pulse_speed * 3 + instant) / 4;
}

#endif /* CONFIG_KOBITOKEY_HAPTIC_TICK_BY_DISTANCE */


void kobitokey_haptic_pulse_rel(int32_t value)
{
#if IS_ENABLED(CONFIG_KOBITOKEY_HAPTIC_TICK_BY_DISTANCE)
    haptic_pulse_by_distance(value);
#else
    haptic_note_speed(value);

    const int64_t now = k_uptime_get();

    if (now - last_haptic_time < (int64_t)haptic_cooldown_for_speed(pulse_speed)) {
        return;
    }

    last_haptic_time = now;

    haptic_request(CONFIG_KOBITOKEY_HAPTIC_EFFECT);
#endif
}


/*
 * 指定のエフェクトをひとつ鳴らす。転がしの速さは見ない。
 *
 * スクロールの刻みと同じ下限だけは守る。長押しの確定と転がしの刻みが
 * 同じ瞬間に来ることはありうるし、そのとき二重に鳴ると、どちらの
 * 知らせなのか分からない濁った一発になる。
 */
void kobitokey_haptic_effect(uint8_t effect)
{
    const int64_t now = k_uptime_get();

    if (now - last_haptic_time < HAPTIC_COOLDOWN_MS) {
        return;
    }

    last_haptic_time = now;

    haptic_request(effect);
}


void kobitokey_haptic_double(uint8_t effect, uint16_t gap_ms)
{
    const int64_t now = k_uptime_get();

    if (!haptic_ready || now - last_haptic_time < HAPTIC_COOLDOWN_MS) {
        return;
    }

    last_haptic_time = now;

    atomic_set(&haptic_pending_gap_ms, gap_ms);
    atomic_set(&haptic_pending_effect, effect);
    (void)k_work_submit_to_queue(&haptic_workq, &haptic_play_work);
}


void kobitokey_haptic_pulse_ms(uint32_t duration_ms)
{
    /*
     * 長さの指定は受け取るが使わない。
     *
     * どれだけ振動するかはエフェクトの波形が決めるもので、こちらが
     * ミリ秒で決める性質のものではなくなった。呼び出し側を変えずに
     * 済ませるために引数だけ残してある。
     */
    ARG_UNUSED(duration_ms);

    kobitokey_haptic_pulse();
}


void kobitokey_haptic_pulse(void)
{
    /*
     * 動いた量が分からない呼び出し。1として数えておく。速さを測る
     * 材料にはならないが、0を渡して平均を引き下げるよりは害がない。
     */
    kobitokey_haptic_pulse_rel(1);
}


bool kobitokey_haptic_quiet_for_ms(uint32_t quiet_ms)
{
    if (last_haptic_time == 0) {
        /* Never pulsed since boot. */
        return true;
    }

    return (k_uptime_get() - last_haptic_time) >= (int64_t)quiet_ms;
}


void kobitokey_haptic_usb_acknowledge(void)
{
    if (haptic_ready) {
        /* Running normally: hand it to the haptic thread like any other
         * buzz, so nothing blocks on the bus here. */
        last_haptic_time = k_uptime_get();
        haptic_request(CONFIG_KOBITOKEY_HAPTIC_USB_EFFECT);
        return;
    }

    /*
     * Asked for before the chip is up. That is the closed-lid USB boot,
     * where there is no "later" to defer to -- the half powers off as soon
     * as the lid reading is confirmed. Do the whole thing here and now.
     */
    if (!haptic_play_and_wait_now(CONFIG_KOBITOKEY_HAPTIC_USB_EFFECT)) {
        return;
    }

    /*
     * The chip is configured now, so the deferred setup would only repeat
     * it and then play the boot pattern -- which is not what a charging
     * notice should sound like. Cancel it and take ownership.
     *
     * Only if there is something to cancel: this can run before
     * haptic_init(), and cancelling a work item that was never initialised
     * is not something to do to save a branch.
     */
    if (haptic_initialized) {
        (void)k_work_cancel_delayable(&haptic_setup_work);
        haptic_ready = true;
    }
}


/*
 * 確実に黙らせる。
 *
 * ENをLOWにするのはI2Cを一切通らないので、バスがどうなっていても効く。
 * 電源を切る直前に呼ばれる以上、ここは失敗しようのない手段でなければ
 * ならない。
 */
void kobitokey_haptic_shutdown(void)
{
    haptic_ready = false;

    if (haptic_initialized) {
        (void)k_work_cancel_delayable(&haptic_setup_work);
        (void)k_work_cancel(&haptic_play_work);
        haptic_enable_set(false);
        return;
    }

    /* Before init: the pin may not be an output yet, so make it one and
     * drive it low rather than writing to a pin that is still an input. */
    if (gpio_is_ready_dt(&haptic_en)) {
        (void)gpio_pin_configure_dt(&haptic_en, GPIO_OUTPUT_INACTIVE);
    }
}


#if defined(CONFIG_KOBITOKEY_VBUS_SENSE)

static void kobitokey_haptic_vbus_changed(bool connected)
{
    if (!connected) {
        return;
    }

    kobitokey_haptic_usb_acknowledge();
}

#endif


#if DT_HAS_COMPAT_STATUS_OKAY(kobitokey_haptic_local_input)

/*
 * 左手peripheral用。
 *
 * input processorとして挟まず、
 * input callbackでRELイベントを横から見る。
 *
 * これによりtb_left_splitの送信経路を邪魔しない。
 */
static void kobitokey_haptic_local_input_cb(struct input_event *event)
{
    if (event->type != INPUT_EV_REL || event->value == 0) {
        return;
    }

    if (event->code == INPUT_REL_X ||
        event->code == INPUT_REL_Y ||
        event->code == INPUT_REL_WHEEL ||
        event->code == INPUT_REL_HWHEEL) {
        kobitokey_haptic_pulse_rel(event->value);
    }
}

INPUT_CALLBACK_DEFINE(NULL, kobitokey_haptic_local_input_cb);

#endif


static int haptic_init(void)
{
    if (!gpio_is_ready_dt(&haptic_en)) {
        LOG_ERR("Haptic enable pin is not ready");
        return -ENODEV;
    }

    /* Start held down: the chip stays in shutdown until setup runs. */
    (void)gpio_pin_configure_dt(&haptic_en, GPIO_OUTPUT_INACTIVE);

    k_work_queue_init(&haptic_workq);
    k_work_queue_start(&haptic_workq, haptic_workq_stack,
                       K_THREAD_STACK_SIZEOF(haptic_workq_stack),
                       K_PRIO_PREEMPT(10),
                       &(struct k_work_queue_config){ .name = "haptic" });

    k_work_init(&haptic_play_work, haptic_play_work_handler);
    k_work_init_delayable(&haptic_setup_work, haptic_setup_work_handler);

    haptic_initialized = true;

#if defined(CONFIG_KOBITOKEY_VBUS_SENSE)
    kobitokey_vbus_set_callback(kobitokey_haptic_vbus_changed);
#endif

    k_work_schedule_for_queue(&haptic_workq, &haptic_setup_work,
                              K_MSEC(HAPTIC_BOOT_DELAY_MS));

    return 0;
}

/*
 * POST_KERNEL 55: GPIO(40) と I2C(50) の後、かつ「閉じたままUSB」の
 * 通知を出すフック(60)より前。
 *
 * 以前は APPLICATION に置いていたが、それでは遅すぎた。蓋を判定して
 * 電源を切る kobitokey_fold_init() は POST_KERNEL 95 で、APPLICATION は
 * その後だ。つまり閉じたまま起動した場合、ここが走る前に電源が落ちて
 * いた。ENピンもワークキューも用意されていない状態で shutdown() が
 * 呼ばれることになり、それ自体も筋が悪い。
 */
#define HAPTIC_INIT_PRIORITY 55
BUILD_ASSERT(HAPTIC_INIT_PRIORITY > CONFIG_I2C_INIT_PRIORITY &&
                 HAPTIC_INIT_PRIORITY > CONFIG_GPIO_INIT_PRIORITY,
             "The haptic must initialise after the buses it talks over");

SYS_INIT(
    haptic_init,
    POST_KERNEL,
    HAPTIC_INIT_PRIORITY);
