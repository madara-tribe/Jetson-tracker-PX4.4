# PIRカメラ不作動の原因分析と解決レポート

## 対象システム

PX System — Night/Pattern-C。PIR センサーで起動し、IR カメラ
(HD USB Camera 32e4:9230) の映像に YOLO11 を適用して人物を検出・追尾する
ROS2 構成 (px2 推論ノード + px3 サーボ/PIR ノード + watch_node)。

## 要約

current コードでは「PIR は反応するがカメラが映像をキャプチャできず、
WATCH と DETECT を延々繰り返すだけ」という症状が発生した。previous
(旧 Pattern-C) では同じハードで正常動作していた。原因はハードや配線では
なく、**px2 のカメラオープン経路とフォーマット設定の退行 (regression)**
だった。具体的には (1) OpenCV のバックエンド未指定オープン、(2) MJPG
フォーマット未指定、(3) DETECT 遷移時のカメラ設定再適用の欠落、の3点が
重なり、有効な映像フレームが取得できていなかった。この3点を修正すること
で、previous と同等の安定したキャプチャが回復した。

---

## 1. 症状

- PIR は検知し、px2 は DETECT+TRACK モードへ遷移する (ログ上は正常)。
- しかし `px2/data` フォルダに画像が一枚も保存されない。
  previous では保存されていた。
- DETECT に数秒滞在した後、`lost_max_frames` に達して WATCH へ戻り、
  これを繰り返す。
- 検出系のログ (`[yolo] accepted detection`, `publishState`,
  `bbox_center=...`) が一度も出力されない。

これらは「カメラから有効なフレームが得られず、YOLO が何も検出できて
いない」状態と完全に整合する。`saveFrameIfNeeded` は検出があったときしか
保存しないため、検出ゼロなら保存もゼロになる。

---

## 2. 切り分けの経緯

調査は仮説と反証を繰り返して原因を絞り込んだ。

第一に、docker の `--group-add` によるデバイス権限の不一致を疑ったが、
ログにカメラ設定行 (`Camera settings: ...`) が出ていた。この行は
`cap_.open()` 成功後にしか到達しないため、権限は通っており原因ではないと
判断した。

第二に、GStreamer 警告 (`pipeline have not been created`) を current
固有の問題と疑ったが、previous のログにも同一の3行が出ていた。つまり
これは差分ではなく、OpenCV が GStreamer で開くのに失敗し V4L2 へ
フォールバックする際の通常の警告だった。

第三に、解像度 1920x1080 が高すぎる説を検討したが、previous の保存画像も
1920x1080 であり (`detect_thirds_56.png` が 1080x1920x3)、解像度自体は
原因でないと確認できた。

決定的だったのは2つの事実である。MJPG を明示した `camera_run.py` では
コンテナ内で 1920x1080 が撮影・保存できたこと。そして previous と current
の唯一の本質的なログ差分が、フォーマット確立のタイミングと有効 fps
(previous は安定、current は `@ 6.00`) にあったことである。

---

## 3. 根本原因

3つの要因が重なって有効フレームが取得できていなかった。

**要因A: バックエンド未指定のカメラオープン。**
current は `cap_.open(device_path)` をバックエンド指定なしで呼んでいた。
この場合 OpenCV はまず GStreamer で開こうとして失敗し、その後 V4L2 へ
フォールバックする。この経路で開かれたキャプチャは不安定だった。
成功実績のある `camera_run.py` は `cv2.VideoCapture(path, cv2.CAP_V4L2)`
と最初から V4L2 を明示しており、この不安定経路を踏まない。

**要因B: MJPG フォーマットの未指定。**
`applyCameraSettingsToCap` が `CAP_PROP_FOURCC` を設定していなかったため、
カメラはデフォルトの YUYV (無圧縮) で開かれた。YUYV の 1920x1080 は
USB2.0 の帯域 (約 60 MB/s) を大きく超える (約 124 MB/s) ため、ドライバは
fps を 6 まで落とすか、フレームを破断させる。ログの
`Camera settings: 1920x1080 @ 6.00 (requested ... @ 30.00)` がこの帯域
破綻の証拠である。破断・劣化したフレームでは YOLO は人物を検出できない。

**要因C: DETECT 遷移時のカメラ設定再適用の欠落。**
previous は WATCH 用の軽い設定でいったんストリームを確立し、DETECT 遷移時
に `applyCameraSettings(active...)` を呼んでストリームを張り直していた。
この「張り直し」が有効フレーム取得の鍵だった。current ではコンストラクタ
で一度設定するだけで、DETECT 遷移時の再適用が削除されていたため、不安定な
ままだった。

要因 B と C のどちらかでも欠けると、有効フレームが流れない。B (MJPG) が
無ければ帯域破綻し、C (再適用) が無ければ不安定なまま推論に入る。両方が
揃って初めて、DETECT で正常な映像が流れる状態になる。

### previous と current の差分対比

| 項目 | previous (動く) | current 修正前 (動かない) |
|---|---|---|
| オープン方法 | V4L2 明示 (※ Python テスト側) | バックエンド未指定 |
| FOURCC | — | 未指定 → YUYV で開かれる |
| 有効 fps | 安定 | `@ 6.00` (帯域破綻) |
| 解像度設定の流れ | WATCH 軽設定 → DETECT で張り直し | 起動時に一度だけ |
| data への保存 | あり | ゼロ (検出ゼロのため) |

---

## 4. 解決策

px2 のカメラ取得を、成功実績のある `camera_run.py` と同じ
「V4L2 明示 + MJPG 強制」に揃え、さらに previous の「DETECT 時に再適用」
を復活させた。具体的な変更は次の3点。

**修正1: V4L2 バックエンドの明示 (inference.cpp)**

```cpp
cap_.open(device_path, cv::CAP_V4L2);
```

GStreamer の不安定なフォールバック経路を回避し、最初から V4L2 で確実に
開く。

**修正2: MJPG フォーマットの強制 (capture_settings.hpp)**

幅・高さの設定より前に FOURCC を MJPG に指定する。

```cpp
cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M','J','P','G'));
if (fps > 0.0)  cap.set(cv::CAP_PROP_FPS, fps);
if (width > 0)  cap.set(cv::CAP_PROP_FRAME_WIDTH, width);
if (height > 0) cap.set(cv::CAP_PROP_FRAME_HEIGHT, height);
```

MJPG は圧縮済みのため 1920x1080 でも USB 帯域に収まり、要求 fps の有効な
ストリームになる。

**修正3: DETECT 遷移時のカメラ設定再適用 (inference.cpp)**

`enterDetectTrackMode()` で、推論に入る直前にカメラ設定を再適用して
ストリームを張り直す。

```cpp
void OnnxInferenceNode::enterDetectTrackMode() {
  mode_ = Mode::DETECT_TRACK;
  // ... PD リセット ...
  vision::capture::applyCameraSettingsToCap(
      cap_, get_logger(), active_width_, active_height_, active_fps_);
  publishWakeup(true);
}
```

### 修正後の動作ロジック

V4L2 で開く → MJPG で解像度・fps を確立 → DETECT 遷移で同設定を張り直して
推論。これにより DETECT 中に有効フレームが安定供給され、YOLO が人物を検出
し、`saveFrameIfNeeded` が発火して画像が保存されるようになった。

---

## 5. 補足: 修正過程で生じた副次的バグと是正

修正3を入れた際、`enterDetectTrackMode()` から `active_fps_` が参照でき
ずコンパイルエラーになったため、応急処置として namespace 直下にグローバル
変数 `double active_fps_ = 30.0;` を定義した。これはコンパイルは通るが、
コンストラクタ内のローカル `const double active_fps_` (launch 値) と
グローバル変数 (30.0 固定) という2つの異なる値が併存し、launch で
`active_fps` を変えても DETECT 側に反映されないという地雷を生む。

正しい是正は `active_fps_` をメンバ変数化することである。`inference.h` に
`double active_fps_ = 30.0;` を宣言し、コンストラクタはローカル宣言を外して
メンバへ代入、グローバル定義は削除する。これでコンストラクタと
`enterDetectTrackMode()` の両方が同一のメンバを参照し、launch 値が一貫して
効くようになる。

---

## 6. 教訓

- USB カメラを高解像度で使うときは FOURCC=MJPG を明示すること。未指定だと
  YUYV で開かれ、USB2.0 帯域を超えて fps 低下やフレーム破断を起こす。
  有効 fps が要求値より極端に低い (例: 30 要求で 6) ときは帯域破綻を疑う。
- OpenCV の `VideoCapture` は `/dev/video/<name>` のようなパスだと
  バックエンド自動判定が外れやすい。`cv::CAP_V4L2` を明示すると安定する。
- 「`cap_.read()` が成功している = 有効な画像が得られている」とは限らない。
  検出ゼロ・保存ゼロという症状は、フレームは返るが中身が無効なケースと
  整合する。
- 既知の動作する最小例 (ここでは `camera_run.py`) と本番コードのカメラ
  オープン手順を突き合わせると、原因の切り分けが速い。
- 動かすための応急処置 (グローバル変数の追加など) は、スコープと値の一貫性
  を壊すことがある。動作確認後に正しい設計 (メンバ変数化) へ戻すこと。
