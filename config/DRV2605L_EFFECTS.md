# DRV2605L 内蔵エフェクト一覧

TI DRV2605L データシート (SLOS854C) の "Waveform Library Effects List" より。
`CONFIG_KOBITOKEY_HAPTIC_EFFECT` などに書く番号はここから選ぶ。

LRA では `LIBRARY = 6` を選んでいるので、同じ番号でも ERM 用ライブラリとは
波形が異なる。末尾の % は強度。

## 基本（1〜16）

| # | 名前 | # | 名前 |
|---|---|---|---|
| 1 | Strong Click – 100% | 9 | Soft Bump – 30% |
| 2 | Strong Click – 60% | 10 | Double Click – 100% |
| 3 | Strong Click – 30% | 11 | Double Click – 60% |
| 4 | Sharp Click – 100% | 12 | Triple Click – 100% |
| 5 | Sharp Click – 60% | 13 | Soft Fuzz – 60% |
| 6 | Sharp Click – 30% | 14 | Strong Buzz – 100% |
| 7 | Soft Bump – 100% | 15 | 750 ms Alert – 100% |
| 8 | Soft Bump – 60% | 16 | 1000 ms Alert – 100% |

## クリックとティック（17〜26）

| # | 名前 | # | 名前 |
|---|---|---|---|
| 17 | Strong Click 1 – 100% | 22 | Medium Click 2 – 80% |
| 18 | Strong Click 2 – 80% | 23 | Medium Click 3 – 60% |
| 19 | Strong Click 3 – 60% | 24 | Sharp Tick 1 – 100% |
| 20 | Strong Click 4 – 30% | 25 | Sharp Tick 2 – 80% |
| 21 | Medium Click 1 – 100% | 26 | Sharp Tick 3 – 60% |

## 二連（27〜46）

| # | 名前 | # | 名前 |
|---|---|---|---|
| 27 | Short Double Click Strong 1 – 100% | 37 | Long Double Sharp Click Strong 1 – 100% |
| 28 | Short Double Click Strong 2 – 80% | 38 | Long Double Sharp Click Strong 2 – 80% |
| 29 | Short Double Click Strong 3 – 60% | 39 | Long Double Sharp Click Strong 3 – 60% |
| 30 | Short Double Click Strong 4 – 30% | 40 | Long Double Sharp Click Strong 4 – 30% |
| 31 | Short Double Click Medium 1 – 100% | 41 | Long Double Sharp Click Medium 1 – 100% |
| 32 | Short Double Click Medium 2 – 80% | 42 | Long Double Sharp Click Medium 2 – 80% |
| 33 | Short Double Click Medium 3 – 60% | 43 | Long Double Sharp Click Medium 3 – 60% |
| 34 | Short Double Sharp Tick 1 – 100% | 44 | Long Double Sharp Tick 1 – 100% |
| 35 | Short Double Sharp Tick 2 – 80% | 45 | Long Double Sharp Tick 2 – 80% |
| 36 | Short Double Sharp Tick 3 – 60% | 46 | Long Double Sharp Tick 3 – 60% |

## ブザーとパルス（47〜57）

| # | 名前 | # | 名前 |
|---|---|---|---|
| 47 | Buzz 1 – 100% | 53 | Pulsing Strong 2 – 60% |
| 48 | Buzz 2 – 80% | 54 | Pulsing Medium 1 – 100% |
| 49 | Buzz 3 – 60% | 55 | Pulsing Medium 2 – 60% |
| 50 | Buzz 4 – 40% | 56 | Pulsing Sharp 1 – 100% |
| 51 | Buzz 5 – 20% | 57 | Pulsing Sharp 2 – 60% |
| 52 | Pulsing Strong 1 – 100% | | |

## トランジション クリック / ハム（58〜69）

| # | 名前 | # | 名前 |
|---|---|---|---|
| 58 | Transition Click 1 – 100% | 64 | Transition Hum 1 – 100% |
| 59 | Transition Click 2 – 80% | 65 | Transition Hum 2 – 80% |
| 60 | Transition Click 3 – 60% | 66 | Transition Hum 3 – 60% |
| 61 | Transition Click 4 – 40% | 67 | Transition Hum 4 – 40% |
| 62 | Transition Click 5 – 20% | 68 | Transition Hum 5 – 20% |
| 63 | Transition Click 6 – 10% | 69 | Transition Hum 6 – 10% |

## ランプ ダウン 100→0%（70〜81）

| # | 名前 | # | 名前 |
|---|---|---|---|
| 70 | Ramp Down Long Smooth 1 | 76 | Ramp Down Long Sharp 1 |
| 71 | Ramp Down Long Smooth 2 | 77 | Ramp Down Long Sharp 2 |
| 72 | Ramp Down Medium Smooth 1 | 78 | Ramp Down Medium Sharp 1 |
| 73 | Ramp Down Medium Smooth 2 | 79 | Ramp Down Medium Sharp 2 |
| 74 | Ramp Down Short Smooth 1 | 80 | Ramp Down Short Sharp 1 |
| 75 | Ramp Down Short Smooth 2 | 81 | Ramp Down Short Sharp 2 |

## ランプ アップ 0→100%（82〜93）

| # | 名前 | # | 名前 |
|---|---|---|---|
| 82 | Ramp Up Long Smooth 1 | 88 | Ramp Up Long Sharp 1 |
| 83 | Ramp Up Long Smooth 2 | 89 | Ramp Up Long Sharp 2 |
| 84 | Ramp Up Medium Smooth 1 | 90 | Ramp Up Medium Sharp 1 |
| 85 | Ramp Up Medium Smooth 2 | 91 | Ramp Up Medium Sharp 2 |
| 86 | Ramp Up Short Smooth 1 | 92 | Ramp Up Short Sharp 1 |
| 87 | Ramp Up Short Smooth 2 | 93 | Ramp Up Short Sharp 2 |

## ランプ ダウン 50→0%（94〜105）

| # | 名前 | # | 名前 |
|---|---|---|---|
| 94 | Ramp Down Long Smooth 1 | 100 | Ramp Down Long Sharp 1 |
| 95 | Ramp Down Long Smooth 2 | 101 | Ramp Down Long Sharp 2 |
| 96 | Ramp Down Medium Smooth 1 | 102 | Ramp Down Medium Sharp 1 |
| 97 | Ramp Down Medium Smooth 2 | 103 | Ramp Down Medium Sharp 2 |
| 98 | Ramp Down Short Smooth 1 | 104 | Ramp Down Short Sharp 1 |
| 99 | Ramp Down Short Smooth 2 | 105 | Ramp Down Short Sharp 2 |

## ランプ アップ 0→50%（106〜117）

| # | 名前 | # | 名前 |
|---|---|---|---|
| 106 | Ramp Up Long Smooth 1 | 112 | Ramp Up Long Sharp 1 |
| 107 | Ramp Up Long Smooth 2 | 113 | Ramp Up Long Sharp 2 |
| 108 | Ramp Up Medium Smooth 1 | 114 | Ramp Up Medium Sharp 1 |
| 109 | Ramp Up Medium Smooth 2 | 115 | Ramp Up Medium Sharp 2 |
| 110 | Ramp Up Short Smooth 1 | 116 | Ramp Up Short Sharp 1 |
| 111 | Ramp Up Short Smooth 2 | 117 | Ramp Up Short Sharp 2 |

## 特殊（118〜123）

| # | 名前 |
|---|---|
| 118 | Long buzz for programmatic stopping – 100% |
| 119 | Smooth Hum 1（キック/ブレーキ パルスなし）– 50% |
| 120 | Smooth Hum 2（同上）– 40% |
| 121 | Smooth Hum 3（同上）– 30% |
| 122 | Smooth Hum 4（同上）– 20% |
| 123 | Smooth Hum 5（同上）– 10% |

---

## 選ぶときの手掛かり

- **トラックボールの刻み**: 1〜6、17〜26 のクリック/ティック系。
  短く単発で終わるものが向く。
- **通知**: 10〜12 の二連・三連、14〜16 のアラート系。
  「意図して鳴らした」と分かる長さがある。
- **ブート**: 47〜51 の Buzz、52〜57 の Pulsing。
- **ランプ系（70〜117）は単発の刻みには向かない。**
  音量が上がる/下がる過程を表す波形で、単体だと「ぼやけた唸り」になる。
  連続再生の中で使うもの。
- **119〜123 の Smooth Hum はキックもブレーキも無い。**
  つまり LRA の「キレ」を意図的に殺した波形で、余韻のある柔らかい振動になる。
