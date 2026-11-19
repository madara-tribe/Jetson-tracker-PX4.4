# PX4.4 配線（Jetson Orin Nano 版）

PX4.3 からの変更点は **PC を Jetson Orin Nano に置き換えただけ**。
Arduino・サーボ・PIR センサーの配線は PX4.3 と同じ。

---

## 1. 全体図

```
                    ┌──────────────────────┐
  [19V AC アダプタ] ─┤ DC ジャック           │
   (開発キット付属)  │                      │
                    │  Jetson Orin Nano    │     Wi-Fi / Tailscale
                    │                      │ ~~~~~~~~~~~~~~~~~~~~~~ [Mac]  SSH・VNC
                    │  USB-A ●─────────────┼──────────────┐
                    │  USB-A ●──────┐      │              │
                    └───────────────┼──────┘              │
                                    │ USB                 │ USB（シリアル /dev/ttyACM0、115200）
                                    ▼                     ▼
                         [IR USB カメラ]           ┌──────────────┐
                         ELP 32e4:9230             │   Arduino    │
                         /dev/video/pir_camera     │ (Telemetrix) │
                                                   │              │
                                                   │ 5V  ●────────┼──── 赤 ──→ ブレッドボード ＋ 列
                                                   │ GND ●────────┼──── 黒 ──→ ブレッドボード − 列
                                                   │ D2  ●────────┼──── 黄 ──→ PIR OUT
                                                   │ D9  ●────────┼──── 黄 ──→ サーボ1 信号（縦 / Y）
                                                   │ D10 ●────────┼──── 黄 ──→ サーボ2 信号（横 / X）
                                                   └──────────────┘

   ブレッドボード ＋ 列（5V） ──┬── サーボ1 赤
                                ├── サーボ2 赤
                                └── PIR ＋

   ブレッドボード − 列（GND）──┬── サーボ1 茶
                                ├── サーボ2 茶
                                └── PIR −
```

---

## 2. 接続表

### 2-1. Jetson Orin Nano
| Jetson 側 | 相手 | ケーブル | Linux 上の名前 |
|---|---|---|---|
| DC ジャック | 19V AC アダプタ（付属品、日本プラグのコード） | — | — |
| USB-A | Arduino | USB（Arduino の種類に合うもの） | `/dev/ttyACM0` |
| USB-A | IR USB カメラ（ELP、32e4:9230） | USB | `/dev/video/pir_camera`（udev で固定） |
| Wi-Fi（内蔵） | ネットワーク | — | `wlP1p1s0` |
| DisplayPort | モニタ（初期設定のときだけ） | DP→HDMI 変換 | — |

### 2-2. Arduino
| Arduino ピン | 接続先 | 線の色（サーボ） | コード上の名前 |
|---|---|---|---|
| 5V | ブレッドボード ＋ 列 | — | — |
| GND | ブレッドボード − 列 | — | — |
| D2 | PIR センサー OUT | — | `PIR_PIN = 2`（デジタル入力、変化を報告） |
| D9 | サーボ1 信号 | 黄 | `SERVO_Y_PIN = 9`（縦、50〜130°、中心 90°） |
| D10 | サーボ2 信号 | 黄 | `SERVO_X_PIN = 10`（横、30〜150°、中心 90°） |
| USB | Jetson | — | 115200 baud |

### 2-3. ブレッドボード
| 列 | つながるもの |
|---|---|
| ＋（5V） | Arduino 5V、サーボ1 赤、サーボ2 赤、PIR ＋ |
| −（GND） | Arduino GND、サーボ1 茶、サーボ2 茶、PIR − |

### 2-4. 部品ごと
| 部品 | ピン | 接続先 |
|---|---|---|
| サーボ1（縦 / Y） | 赤 / 茶 / 黄 | 5V 列 / GND 列 / D9 |
| サーボ2（横 / X） | 赤 / 茶 / 黄 | 5V 列 / GND 列 / D10 |
| PIR センサー（SB612B） | ＋ / − / OUT | 5V 列 / GND 列 / D2 |

※ 縦・横の割り当ては `SeamlessTrack/src/px3/include/px3/px3_servo_node.h` の値（D10 = X、D9 = Y）。

---

## 3. 動作の流れ（配線との対応）

1. PIR が人を検知 → D2 が HIGH → Arduino が Jetson に報告 → `/pir/state` → `/px_wakeup`
2. px2 が WATCH から DETECT に切り替わり、カメラの映像で YOLO 推論
3. 人の位置からサーボの角度を計算 → Arduino 経由で D9（縦）・D10（横）に指令
4. 人を見失うと WATCH に戻る（カメラの映像は読み捨て、推論もしない）

→ **PIR の配線が外れていると、カメラは待機のままで何も起きない。** 動かないときは最初に PIR を疑う。

---

## 4. 注意

- **サーボの電源**: 今は 2 つのサーボを Arduino の 5V（USB 給電）から取っている。動いたときに電流が足りず、Arduino のリセットやサーボの震えが起きることがある。その場合はサーボの赤線を **別の 5V 電源（2A 以上）** から取り、**GND は Arduino と共通**にする。
- **抜けやすい箇所**: Arduino のピン、ブレッドボード、PIR のメス端子。強化案はネジ端子シールド、レバー式コネクタ、JST-XH ロックコネクタ、USB のケーブルクランプ。可動部（サーボ）の線は回転範囲分の余裕を持たせる。
- **Jetson の 40 ピンヘッダには直接つながない**: Jetson の GPIO は 3.3V で、サーボや PIR（5V）を直接扱えない。Arduino 経由のままにする。
- **起動直後の PIR**: 電源を入れてから反応するまで数十秒かかることがある。

---

## 5. 配線の確認手順

```bash
# 🟩 Jetson 本体
lsusb | grep 32e4                  # カメラ
ls -l /dev/video/pir_camera        # → /dev/videoX
ls /dev/ttyACM*                    # Arduino

# PIR 単体（PX を止めてから、ホストで）
python3 calibration/pir_telemetrix_validate.py

# 🐳 PX 起動中に、別の端末から（docker exec でコンテナに入る）
ros2 topic echo /pir/state         # 200 ms ごとに今の値が出る。手を振って true に変われば PIR〜Arduino〜Jetson は正常
ros2 topic pub --once --qos-durability transient_local --qos-reliability reliable \
  /px_wakeup std_msgs/msg/Bool "{data: true}"   # PIR なしでカメラ・推論・サーボだけ確認
```
