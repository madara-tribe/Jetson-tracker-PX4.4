# Point 1 コード修正 Before/After（Pattern-C最新コード基準）

対象ファイル:
- `SeamlessTrack/src/px2/include/control/axis_pd_controller.hpp`
- `SeamlessTrack/src/px2/include/inference.h`
- `SeamlessTrack/src/px2/src/inference.cpp`

Pattern-B/Cとも該当箇所は同一コードのため、この修正はどちらにも適用可能。

---

## 修正① dead zone内でもpos_degが動き続ける不整合バグの解消【最重要】

### 修正の意味
現状は「誤差計算」と「サーボ位置の更新(pos_deg蓄積)」が`updateFromErrorRad()`という1つの関数の中で不可分に行われており、これが**dead zoneの内外に関わらず毎フレーム呼ばれてしまう**。一方、実際にサーボへ送る`publishState()`はdead zone外の時しか呼ばれない。結果、「送信していないつもりの間」も内部の目標角度(`pos_deg`)だけがbboxのノイズに応じて動き、実サーボ位置と内部推定位置が乖離していく。

この修正では、「誤差を計算するだけ(副作用なし)」の`errorDeg()`と、「実際にpos_degを進める(副作用あり)」の`commit()`を分離し、**dead zone外の軸だけ`commit()`を呼ぶ**ように変更する。

### Before

```cpp
// axis_pd_controller.hpp
double updateFromErrorRad(double err_rad, double dt = 1.0) {
  const double err_deg = err_rad * 180.0 / PX_PI;

  const double d_term = (dt > 0.0) ? Kd * (err_deg - prev_err_deg) / dt : 0.0;

  double delta_deg = Kp * err_deg + d_term;
  delta_deg = std::clamp(delta_deg, -max_step_deg, max_step_deg);

  pos_deg += delta_deg;
  prev_err_deg = err_deg;
  const double servo = center_deg + pos_deg;
  return std::clamp(servo, min_deg, max_deg);
}
```

```cpp
// inference.h
void computeAxisDeg(Axis axis, const cv::Rect2d& box, double dt);
```

```cpp
// inference.cpp
void OnnxInferenceNode::computeAxisDeg(Axis axis, const cv::Rect2d& box, double dt) {
  const cv::Point2f p_meas = vision::bbox::bboxCenter(box);

  if (axis == Axis::YAW) {
    const cv::Point2f p_tgt(static_cast<float>(active_width_ / 2.0), p_meas.y);
    const double yaw_rad = control::yawErrFromPixels(p_meas, p_tgt, calib_.K, calib_.D);
    servo_x_deg_ = yaw_axis_.updateFromErrorRad(yaw_rad, dt);
    RCLCPP_INFO(get_logger(), "yaw_rad=%.3f | err_deg=%.2f | servo_x_deg_=%.2f", yaw_rad,
                yaw_axis_.prev_err_deg, servo_x_deg_);
  } else {
    const cv::Point2f p_tgt(p_meas.x, static_cast<float>(active_height_ * kTargetYRatio));
    const double pitch_rad = control::pitchErrFromPixels(p_meas, p_tgt, calib_.K, calib_.D);
    servo_y_deg_ = pitch_axis_.updateFromErrorRad(pitch_rad, dt);
    RCLCPP_INFO(get_logger(), "pitch_rad=%.3f | err_deg=%.2f | servo_y_deg_=%.2f", pitch_rad,
                pitch_axis_.prev_err_deg, servo_y_deg_);
  }
}
```

```cpp
// inference.cpp: runPipelineLoop() 抜粋
computeAxisDeg(Axis::YAW, box, effective_dt);
computeAxisDeg(Axis::PITCH, box, effective_dt);
// 両軸ともdead zone内ならサーボコマンドを発行しない
const bool yaw_in_dead = std::abs(yaw_axis_.prev_err_deg) < kYawDeadZoneDeg;
const bool pitch_in_dead = std::abs(pitch_axis_.prev_err_deg) < kPitchDeadZoneDeg;
if (!yaw_in_dead || !pitch_in_dead) {
  publishState(static_cast<float>(servo_x_deg_), static_cast<float>(servo_y_deg_));
}
```

### After

```cpp
// axis_pd_controller.hpp
// 「誤差計算(副作用なし)」と「状態コミット(副作用あり)」を分離
double errorDeg(double err_rad) const {
  return err_rad * 180.0 / PX_PI;
}

// dead zone外と判定した時だけ呼ぶ。pos_deg/prev_err_degを実際に進める。
double commit(double err_deg, double dt = 1.0) {
  const double d_term = (dt > 0.0) ? Kd * (err_deg - prev_err_deg) / dt : 0.0;

  double delta_deg = Kp * err_deg + d_term;
  delta_deg = std::clamp(delta_deg, -max_step_deg, max_step_deg);

  pos_deg += delta_deg;      // ← dead zone内では呼ばれない = 実送信角度と一致し続ける
  prev_err_deg = err_deg;
  const double servo = center_deg + pos_deg;
  return std::clamp(servo, min_deg, max_deg);
}
```
（`updateFromErrorRad()`は削除。他ファイルからの参照は`inference.cpp`内のみのため影響範囲は閉じている）

```cpp
// inference.h
double computeAxisErrorDeg(Axis axis, const cv::Rect2d& box); // 戻り値: err_deg（副作用なし）
```

```cpp
// inference.cpp
double OnnxInferenceNode::computeAxisErrorDeg(Axis axis, const cv::Rect2d& box) {
  const cv::Point2f p_meas = vision::bbox::bboxCenter(box);

  if (axis == Axis::YAW) {
    const cv::Point2f p_tgt(static_cast<float>(active_width_ / 2.0), p_meas.y);
    const double yaw_rad = control::yawErrFromPixels(p_meas, p_tgt, calib_.K, calib_.D);
    const double err_deg = yaw_axis_.errorDeg(yaw_rad);
    RCLCPP_INFO(get_logger(), "yaw_rad=%.3f | err_deg=%.2f", yaw_rad, err_deg);
    return err_deg;
  } else {
    const cv::Point2f p_tgt(p_meas.x, static_cast<float>(active_height_ * kTargetYRatio));
    const double pitch_rad = control::pitchErrFromPixels(p_meas, p_tgt, calib_.K, calib_.D);
    const double err_deg = pitch_axis_.errorDeg(pitch_rad);
    RCLCPP_INFO(get_logger(), "pitch_rad=%.3f | err_deg=%.2f", pitch_rad, err_deg);
    return err_deg;
  }
}
```

```cpp
// inference.cpp: runPipelineLoop() 抜粋
const double yaw_err_deg = computeAxisErrorDeg(Axis::YAW, box);
const double pitch_err_deg = computeAxisErrorDeg(Axis::PITCH, box);
const bool yaw_in_dead = std::abs(yaw_err_deg) < kYawDeadZoneDeg;
const bool pitch_in_dead = std::abs(pitch_err_deg) < kPitchDeadZoneDeg;

// dead zone外の軸だけ pos_deg をコミット（＝実送信する角度だけ更新）
if (!yaw_in_dead) {
  servo_x_deg_ = yaw_axis_.commit(yaw_err_deg, effective_dt);
}
if (!pitch_in_dead) {
  servo_y_deg_ = pitch_axis_.commit(pitch_err_deg, effective_dt);
}
if (!yaw_in_dead || !pitch_in_dead) {
  publishState(static_cast<float>(servo_x_deg_), static_cast<float>(servo_y_deg_));
}
```

> 📎 細かい注意点: 旧コードのログは`yaw_axis_.prev_err_deg`（＝関数内で直前に更新された値）を出力していたが、`errorDeg()`は状態を更新しないため、新コードでは代わりにローカル変数`err_deg`をログ出力するよう変更している。この置き換えを忘れると、dead zone内フレームのログが常に「最後にcommitされた時の値」のまま止まって見える。

### 期待される改善動作
- dead zone内で「送信していないのに内部目標角度だけ動く」ことがなくなり、`pos_deg`は常に「実際に最後にサーボへ送った角度」と一致する
- ノイズによるランダムウォーク的なドリフトが蓄積しなくなるため、閾値を超えた瞬間にまとめて大きな補正が飛ぶ現象（＝ユーザー報告の「通り過ぎる」動き）が解消される見込み
- 静止targetに対するhunting（往復振動）の主因が取り除かれる。Point2のロック判定（誤差がなるべく0で安定）も、この修正が前提として効いてくる

---

## 修正② dead zone幅とmax_step_degの再チューニング

### 修正の意味
サーボへの送信角度は最終的に整数degに丸められる（`px3_servo_node.cpp: on_angle()`の`static_cast<int>`）ため、実質的な最小可制御ステップは1°。これに対し現状はdead zone=1.5°、1回の最大補正幅(`max_step_deg`)=3°で、dead zone幅の**2倍**まで一気に動ける設定になっている。dead zoneを整数分解能に対して十分広く取り、`max_step_deg`をdead zone未満に抑えることで、「1回の補正でdead zoneを一発で通り過ぎない」設計にする。

### Before

```cpp
// inference.h
static constexpr double kYawDeadZoneDeg = 1.5;
static constexpr double kPitchDeadZoneDeg = 1.5;
```

```cpp
// inference.cpp（コンストラクタ）
const double max_step_deg = declare_parameter<double>("max_step_deg", 3.0);
```

### After

```cpp
// inference.h
static constexpr double kYawDeadZoneDeg = 2.0;   // 1.5 → 2.0
static constexpr double kPitchDeadZoneDeg = 2.0; // 1.5 → 2.0
```

```cpp
// inference.cpp（コンストラクタ）
const double max_step_deg = declare_parameter<double>("max_step_deg", 1.2); // 3.0 → 1.2（dead zone未満に抑制）
```

### 期待される改善動作
- 1回の補正が最大1.2°に制限されるため、dead zone(2.0°)を一発で通り過ぎることがなくなる
- 反面、収束速度は落ちる（目標到達までの補正回数が増える）ため、Kp/Kd（現状Kp=0.3, Kd=0.2）との再チューニングが必要になる可能性が高い
- どちらもlaunchパラメータ経由で上書き可能な値なので、まず起動引数（`max_step_deg:=1.2`等）で実機検証してから、定数のデフォルト値を確定するのが安全

---

## 修正③ publishState()のACK待ちブロッキング解消

### 修正の意味
現状、サーボコマンド送信のたびに`px3`からのACKが返るまで最大`kAckTimeoutMs`(50ms)、パイプラインループ全体がブロックされる。この間カメラフレームも読めず、検出→追尾ループの実効周期を押し下げている。視覚サーボは「常に最新の実測誤差でコマンドを出し続ける」閉ループであり、逐一到達確認してから次に進む必要は本質的にない。ACKは「診断・ヘルスチェック用途」に限定し、送信をノンブロッキングにする。

### Before

```cpp
// inference.cpp
void OnnxInferenceNode::publishState(float yaw_deg, float pitch_deg) {
  {
    std::lock_guard<std::mutex> lk(ack_mtx_);
    ack_ready_ = false;
  }

  custom_msgs::msg::AbsResult m;
  m.x_angle = yaw_deg;
  m.y_angle = pitch_deg;
  RCLCPP_INFO(get_logger(), "px2: publishState X=%.2f Y=%.2f", m.x_angle, m.y_angle);
  pub_abs_->publish(m);

  if (!wait_for_ack_ms(kAckTimeoutMs))
    RCLCPP_WARN(get_logger(), "px3 ACK timeout; continuing");
}
```

```cpp
// inference.cpp（コンストラクタ内、publisher定義）
{
  rclcpp::QoS qos(10);
  qos.reliable().durability_volatile();
  pub_abs_ = create_publisher<custom_msgs::msg::AbsResult>("inference", qos);
}
```

### After

```cpp
// inference.cpp
void OnnxInferenceNode::publishState(float yaw_deg, float pitch_deg) {
  custom_msgs::msg::AbsResult m;
  m.x_angle = yaw_deg;
  m.y_angle = pitch_deg;
  RCLCPP_INFO(get_logger(), "px2: publishState X=%.2f Y=%.2f", m.x_angle, m.y_angle);
  pub_abs_->publish(m);
  // ACK待ちのブロッキングは撤廃。/px3_ack は last_ack_time_ を更新するだけの
  // ヘルスチェック用途に格下げする（下記コールバック参照）。
}
```

```cpp
// inference.cpp（コンストラクタ内、px3_ack_sub_コールバック）
px3_ack_sub_ = create_subscription<std_msgs::msg::Bool>(
    "px3_ack", rclcpp::QoS(10),
    [this](std_msgs::msg::Bool::ConstSharedPtr m) {
      if (!m->data)
        return;
      last_ack_time_.store(std::chrono::steady_clock::now()); // ヘルスチェック用に記録のみ
    },
    opts);
```

```cpp
// inference.h（★抜けていたので追加。private メンバ、<chrono> のincludeも必要）
std::atomic<std::chrono::steady_clock::time_point> last_ack_time_{
    std::chrono::steady_clock::now()}; // ACKヘルスチェック用
```

```cpp
// inference.cpp（コンストラクタ内、publisher定義）— 推奨（任意）
// 最新のコマンドだけが意味を持つため、古いコマンドをキューに溜めない設定にする
{
  rclcpp::QoS qos(rclcpp::KeepLast(1));
  qos.reliable().durability_volatile();
  pub_abs_ = create_publisher<custom_msgs::msg::AbsResult>("inference", qos);
}
```

（`wait_for_ack_ms()`, `ack_mtx_`, `ack_cv_`, `ack_ready_`, `kAckTimeoutMs`はこの経路で使わなくなるため削除可。ヘルスチェックを使わないなら`px3_ack_sub_`ごと削除してもよい）

### 期待される改善動作
- 現状は毎回最大50ms、ループ全体がブロックされていたが、これが撤廃され次フレーム処理に即座に移れる
- ループの実効周期がYOLO推論時間に近づき、追尾の反応速度が向上する
- ⚠️ 注意点: `px3`側は`on_angle()`内で30ms(`SERVO_ARRIVAL_WAIT_MS`)のsleepを維持したままなので、px2側が待たずに次々コマンドを送ると、px3の処理速度をコマンド送信ペースが上回る可能性がある。QoSを`KeepLast(1)`にしておけば、処理し切れず滞留した古いコマンドは破棄され、常に最新の指令のみが送られる状態を維持できる

---

## 別立てで扱う項目: 高頻度Trackerレイヤーの追加（Point1④）

これは`vision/lightweight_tracker.hpp`の新規追加や`runPipelineLoop()`の状態遷移そのものの再設計を伴う**アーキテクチャレベルの変更**であり、上記のような数十行規模のbefore/afterでは表現しきれない規模。ここでは対象外とし、必要であれば別途設計として詰める。

---

## 修正④ 不要になったACK待ち機構の削除（post process）

### 修正の意味
修正③でpublishState()のACK待ちブロッキングを撤廃した結果、ACK関連の仕組み一式（`wait_for_ack_ms()`, `ack_mtx_`, `ack_cv_`, `ack_ready_`, `ack_group_`, `px3_ack_sub_`, `last_ack_time_`）がどこからも実質的に使われないdead codeになっていた。`last_ack_time_`はコールバック内で書き込まれるだけで読み出し先もなく、write-onlyの状態だった。ヘルスチェック機能として活用する案もあったが、今回は使わない方針とし、関連コードを丸ごと削除した。

### Before

```cpp
// inference.h
rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr px3_ack_sub_;
rclcpp::CallbackGroup::SharedPtr ack_group_;
std::mutex ack_mtx_;
std::condition_variable ack_cv_;
bool ack_ready_ = false;
std::atomic<std::chrono::steady_clock::time_point> last_ack_time_{
    std::chrono::steady_clock::now()}; // ACKヘルスチェック用
```
```cpp
// inference.h
static constexpr int kAckTimeoutMs = 50;
```
```cpp
// inference.h
bool wait_for_ack_ms(int timeout_ms);
```
```cpp
// inference.cpp（コンストラクタ内）
ack_group_ = create_callback_group(rclcpp::CallbackGroupType::Reentrant);
rclcpp::SubscriptionOptions opts;
opts.callback_group = ack_group_;
px3_ack_sub_ = create_subscription<std_msgs::msg::Bool>(
    "px3_ack", rclcpp::QoS(10),
    [this](std_msgs::msg::Bool::ConstSharedPtr m) {
      if (!m->data)
        return;
      last_ack_time_.store(std::chrono::steady_clock::now());
    },
    opts);
```
```cpp
// inference.cpp
bool OnnxInferenceNode::wait_for_ack_ms(int timeout_ms) {
  std::unique_lock<std::mutex> lk(ack_mtx_);
  return ack_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [this] { return ack_ready_; });
}
```

### After

上記6ブロックすべて削除。あわせて未使用になった`<chrono>`, `<mutex>`, `<condition_variable>`のincludeも`inference.h`から削除。

### 連鎖して直す必要があった箇所（CIの契約テスト）
`kAckTimeoutMs`を`inference.h`から削除したことで、以下2箇所が「対応先が見つからない」で連動して壊れる。実際に一度目は`control_params.yaml`側の未更新で、二度目は`test_sim_hil_param_contract.py`側の未適用でCIがFAILした。

```python
# ci_checks/tests/test_sim_hil_param_contract.py: INFERENCE_H_MAPPING
# Before
INFERENCE_H_MAPPING: list[tuple[str, str]] = [
    ("future_params.dead_zone_deg.yaw", "kYawDeadZoneDeg"),
    ("future_params.dead_zone_deg.pitch", "kPitchDeadZoneDeg"),
    ("future_params.ack_timeout_ms", "kAckTimeoutMs"),
]

# After
INFERENCE_H_MAPPING: list[tuple[str, str]] = [
    ("future_params.dead_zone_deg.yaw", "kYawDeadZoneDeg"),
    ("future_params.dead_zone_deg.pitch", "kPitchDeadZoneDeg"),
]
```

```yaml
# simulation/data/params/control_params.yaml
# Before
future_params:
  ...
  ack_timeout_ms: 50           # kAckTimeoutMs (inference.h)
  ...

# After
# ack_timeout_ms の行を削除
```

### 期待される改善動作
- 死んでいたスレッド同期用リソース(mutex/condition_variable/callback_group)が消え、コードの見通しが良くなる
- CIの契約テスト(`test_sim_hil_param_contract.py`)が、実体のない定数を要求し続けることがなくなる
- ⚠️ 教訓: 定数を`inference.h`から削除する際は「①inference.h」「②control_params.yaml」「③test_sim_hil_param_contract.pyのINFERENCE_H_MAPPING」の3点セットを同時に見直す。今回2回に分けてCIが落ちたのはこの3点セットを一度に揃えなかったため

---

## 別立てで扱う項目: run_level3_tracking_tests.pyのdocstring更新（付随修正）

Point1のコード自体ではないが、②(max_step_deg変更)に付随してsimulationテストのdocstringが古い数値を持っていた点も合わせて修正した。

```python
# simulation/scripts/v3/run_level3_tracking_tests.py: Test4ClampBoundaryTracking docstring
# Before
合否基準: PDコマンド出力の連続差分が、実機仕様のmax_step_deg
          (control_params.yaml参照、既定3.0deg)を超えないこと。

# After
合否基準: PDコマンド出力の連続差分が、実機仕様のmax_step_deg
          (control_params.yaml参照。値は下記step_limitとして実行時に動的取得、
          ここでは固定値を記述しない — yaml側の値変更に追従させるため)を超えないこと。
```
判定ロジック自体は`env.params.get(...)`でyamlを動的に読む設計のため実害はなかったが、コメント上の数値だけ実態とズレる状態だったため、以後どの値に変更しても古くならない書き方に変更した。

---

## 適用の推奨順序
1. 修正①（accumulator/dead-zone不整合） — 単独で適用・検証可能。最優先
2. 修正②（dead zone/max_step再チューニング） — ①と同時に実機で挙動確認しながら値を追い込む
3. 修正③（ACK非同期化） — ①②で挙動が安定してから。ループ周期が変わるためKp/Kd/dtへの影響を再確認すること
4. 修正④（dead code削除） — ①②③の効果が実機で確認できた後のpost processとして実施。削除対象の定数がCI契約テスト/yamlから参照されていないか事前に`grep`で確認してから着手すると手戻りが減る
