#pragma once

// ============================================================
//  px3_servo_node.h  — TelemetrixSerial / Px3ServoNode
//
//  TelemetrixSerial: サーボ write + PIR DIGITAL_REPORT 受信
//                    （read_loop スレッドで Arduino からの報告を処理）
//  Px3ServoNode    : /inference → サーボ駆動（四捨五入した整数の角度）、PIR → /pir/state 配信
// ============================================================

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "custom_msgs/msg/abs_result.hpp"
#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/bool.hpp"

namespace px3_embedded {

// ============================================================
//  ピン・コマンド定数
// ============================================================
inline constexpr int SERVO_X_PIN = 10;
inline constexpr int SERVO_Y_PIN = 9;
inline constexpr int PIR_PIN = 2; // SB612B OUT → Arduino D2
// PIR の今の値を /pir/state に送り直す間隔（ms）。変化が無くても送る。
// 受け取る側（watch_node）が 1 回取りこぼしても、次の送信で追いつけるようにするため
inline constexpr int PIR_STATE_PERIOD_MS = 200;

// Telemetrix コマンド
inline constexpr uint8_t CMD_SERVO_ATTACH = 7;
inline constexpr uint8_t CMD_SERVO_WRITE = 8;
inline constexpr uint8_t CMD_SERVO_DETACH = 9;
inline constexpr uint8_t CMD_SET_PIN_MODE = 1;

// Telemetrix レポート
inline constexpr uint8_t REPORT_DIGITAL = 2; // DIGITAL_REPORT
inline constexpr uint8_t REPORT_I_AM_HERE = 6;
inline constexpr uint8_t REPORT_FIRMWARE = 5;

// PIN モード
inline constexpr uint8_t PIN_MODE_INPUT = 0;
inline constexpr uint8_t REPORTING_ENABLED = 1;

// ============================================================
//  TelemetrixSerial
//    サーボコマンド送信（write側）と
//    PIR DIGITAL_REPORT 受信（read_loop スレッド）を担う。
// ============================================================
class TelemetrixSerial {
public:
  // PIR 変化通知コールバック型: on_pir_change(pin, value)
  using PirCallback = std::function<void(uint8_t pin, uint8_t value)>;

  TelemetrixSerial(const std::string& port, int baudrate);
  ~TelemetrixSerial();

  // ----- servo -----
  void servo_attach(uint8_t pin, uint16_t min_pulse, uint16_t max_pulse);
  void servo_write(uint8_t pin, int angle_deg);
  void servo_detach(uint8_t pin);

  // ----- PIR setup -----
  // SET_PIN_MODE で指定ピンを digital input + reporting 有効にする
  void set_pir_pin(uint8_t pin, PirCallback cb);
  // PIR reporting を再トリガーする（WATCH復帰時のHIGH保持状態リカバリ用）。
  // set_pir_pin() で設定したピンに対して SET_PIN_MODE を再送する。
  void retrigger_pir_reporting();
  void shutdown();

private:
  void open_port();
  void write_all(const uint8_t* data, size_t len);
  void send_command(const std::vector<uint8_t>& cmd_without_len);
  // SET_PIN_MODE (digital input + reporting有効) を指定ピンに送信する
  void send_set_pin_mode(uint8_t pin);

  // read_loop: DIGITAL_REPORT を待ち受けてコールバックを呼ぶ
  void start_read_loop();
  void stop_read_loop();
  void read_loop();
  void process_report(const uint8_t* frame, uint8_t len);

private:
  std::string port_;
  int baudrate_{115200};
  int fd_{-1};
  uint8_t pir_pin_{0}; // set_pir_pin() で設定したPIRピン（再トリガー用に保持）

  // write 側ロック
  std::mutex write_mtx_;

  // read_loop スレッド
  std::atomic<bool> read_running_{false};
  std::thread read_thread_;
  PirCallback pir_cb_;

  std::atomic<bool> shutdown_done_{false};
};

// ============================================================
//  Px3ServoNode
//    /inference 受信 → サーボ角度書き込み（最新の 1 件だけ。待たない）
//    PIR 変化 → /pir/state publish。さらに PIR_STATE_PERIOD_MS ごとに今の値を送り直す
//    （/pir/state は「立ち上がり」ではなく「今 HIGH かどうか」を表す）
//
//  パラメータ（serial_port / baudrate / x_center_deg / y_center_deg / pir_pin）は、
//  px2 と同居するときは px2 から parameter_overrides で受け取る（inference.cpp の main）。
// ============================================================
class Px3ServoNode : public rclcpp::Node {
public:
  explicit Px3ServoNode(const rclcpp::NodeOptions& options = rclcpp::NodeOptions());
  ~Px3ServoNode() override;

private:
  // /inference 受信 → サーボ角度書き込み
  void on_angle(const custom_msgs::msg::AbsResult::SharedPtr msg);

  // PIR 変化通知（read_loop スレッドから呼ばれる）
  void on_pir_change(uint8_t pin, uint8_t value);
  // px2 がWATCHに戻った通知を受けて、PIR の今の値を送り直す
  void on_watch_resumed(const std_msgs::msg::Bool::SharedPtr msg);

private:
  std::unique_ptr<TelemetrixSerial> board_;
  double x_center_deg_ = 90.0;
  double y_center_deg_ = 90.0;

  rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr ready_pub_;
  rclcpp::Subscription<custom_msgs::msg::AbsResult>::SharedPtr sub_;

  // /pir/state publisher
  rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr pir_state_pub_;
  rclcpp::TimerBase::SharedPtr pir_state_timer_; // 今の値を定期的に送り直す
  void publish_pir_state();

  // PIR ステート（デバウンス用）
  bool init_ok_{false}; // 初期化完了フラグ（起動直後の誤リセット抑制用）
  std::atomic<bool> pir_detected_{false};
  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr px_wakeup_sub_;
};

} // namespace px3_embedded
