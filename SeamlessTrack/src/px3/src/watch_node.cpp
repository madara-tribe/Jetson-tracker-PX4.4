// ============================================================
//  watch_node.cpp  — WatchNode 実装
//
//  subscribe → publish のマッピング:
//    /pir/state=true（今 HIGH）+ px3_ready=true → /px_wakeup=true
//    /pir/state=false                   → 値を覚えるだけ
//    /px_wakeup=false (from px2)        → wakeup_sent_ reset。rewake_delay_ms のあと、
//                                         PIR がまだ HIGH なら再び /px_wakeup=true
// ============================================================

#include <cinttypes>

#include "px3/watch_node.h"

namespace px_watch {

WatchNode::WatchNode(const rclcpp::NodeOptions& options) : Node("watch_node", options) {
  // ---- パラメータ ----
  pir_debounce_ms_ = declare_parameter<int>("pir_debounce_ms", 200);
  rewake_delay_ms_ = declare_parameter<int>("rewake_delay_ms", 1000);

  // ---- Publisher ----
  // /px_wakeup: transient_local — px2 が後から起動しても受信できる
  auto wakeup_qos = rclcpp::QoS(rclcpp::KeepLast(1)).reliable().transient_local();
  wakeup_pub_ = create_publisher<std_msgs::msg::Bool>("px_wakeup", wakeup_qos);

  // 初期値 false を publish（px2 が late-join したとき false の状態を受け取れるようにする）
  publish_wakeup(false);

  // ---- /px3_ready subscriber ----
  // transient_local: px3 が先に起動済みでも受信できる
  auto ready_qos = rclcpp::QoS(rclcpp::KeepLast(1)).reliable().transient_local();
  px3_ready_sub_ = create_subscription<std_msgs::msg::Bool>(
      "px3_ready", ready_qos, std::bind(&WatchNode::on_px3_ready, this, std::placeholders::_1));

  // ---- /pir/state subscriber ----
  // transient_local: px3 が先に起動済みでも、最後に送られた値（今の値）を受け取れる
  auto pir_qos = rclcpp::QoS(rclcpp::KeepLast(1)).reliable().transient_local();
  pir_sub_ = create_subscription<std_msgs::msg::Bool>(
      "/pir/state", pir_qos, std::bind(&WatchNode::on_pir_state, this, std::placeholders::_1));

  // ---- /px_wakeup feedback subscriber ----
  // px2 が detect mode を終了して watch mode に戻ったとき false を publish する
  // それを受け取って wakeup_sent_ をリセットする
  wakeup_fb_sub_ = create_subscription<std_msgs::msg::Bool>(
      "px_wakeup", wakeup_qos,
      std::bind(&WatchNode::on_wakeup_feedback, this, std::placeholders::_1));

  RCLCPP_INFO(get_logger(), "watch_node ready. pir_debounce_ms=%d rewake_delay_ms=%d",
              pir_debounce_ms_, rewake_delay_ms_);
}

// px3 初期化完了を受け取る。準備前に PIR が HIGH になっていた場合は、ここで起動する。
void WatchNode::on_px3_ready(const std_msgs::msg::Bool::SharedPtr msg) {
  if (msg->data) {
    px3_ready_.store(true);
    RCLCPP_INFO(get_logger(), "watch_node: px3_ready received → PIR monitoring active");
    try_wakeup();
  }
}

// PIR の今の値を受け取る（変化したとき + 200 ms ごと）。値を覚えて、起動できるか確かめる。
void WatchNode::on_pir_state(const std_msgs::msg::Bool::SharedPtr msg) {
  const bool was = pir_level_.exchange(msg->data);
  if (was != msg->data) {
    RCLCPP_INFO(get_logger(), "watch_node: PIR %s", msg->data ? "HIGH" : "LOW");
  }
  if (msg->data)
    try_wakeup();
}

// 条件がそろっていれば wakeup を送る。そろっていなければ何もしない
// （PIR が HIGH のままなら、次の /pir/state でもう一度ここに来る）。
void WatchNode::try_wakeup() {
  if (!pir_level_.load())
    return;

  // ガード1: px3 が未 ready なら待つ（ready になったとき on_px3_ready から呼ばれる）
  if (!px3_ready_.load()) {
    RCLCPP_DEBUG(get_logger(), "watch_node: PIR is HIGH but px3 not ready yet");
    return;
  }

  // ガード2: wakeup 送信済みなら重複送信しない（px2 から false が来るまでブロック）
  if (wakeup_sent_.load())
    return;

  const auto now = std::chrono::steady_clock::now();
  using std::chrono::duration_cast;
  using std::chrono::milliseconds;

  // ガード3: 前回 wakeup から pir_debounce_ms_ 以内は送らない
  if (duration_cast<milliseconds>(now - last_wakeup_t_).count() <
      static_cast<int64_t>(pir_debounce_ms_))
    return;

  // ガード4: px2 が WATCH に戻ってから rewake_delay_ms_ 以内は送らない
  if (duration_cast<milliseconds>(now - watch_resumed_t_).count() <
      static_cast<int64_t>(rewake_delay_ms_))
    return;

  // wakeup 送信
  last_wakeup_t_ = now;
  wakeup_sent_.store(true);
  publish_wakeup(true);

  RCLCPP_INFO(get_logger(), "watch_node: PIR is HIGH → /px_wakeup = true");
}

// /px_wakeup=false (from px2) を受け取り wakeup_sent_ をリセット。
// 自ノードの true publish は条件 !msg->data で自動的に無視される。
// PIR がまだ HIGH なら、rewake_delay_ms_ のあと、次の /pir/state で再び起動する。
void WatchNode::on_wakeup_feedback(const std_msgs::msg::Bool::SharedPtr msg) {
  if (!msg->data && wakeup_sent_.load()) {
    wakeup_sent_.store(false);
    watch_resumed_t_ = std::chrono::steady_clock::now();
    RCLCPP_INFO(
        get_logger(),
        "watch_node: /px_wakeup=false received from px2 → state reset, PIR monitoring resumed");
  }
}

void WatchNode::publish_wakeup(bool on) {
  std_msgs::msg::Bool msg;
  msg.data = on;
  wakeup_pub_->publish(msg);
}

} // namespace px_watch

// ============================================================
//  main
// ============================================================
int main(int argc, char* argv[]) {
  rclcpp::init(argc, argv);
  try {
    auto node = std::make_shared<px_watch::WatchNode>();
    rclcpp::spin(node);
  } catch (const std::exception& e) {
    RCLCPP_ERROR(rclcpp::get_logger("main"), "Fatal: %s", e.what());
  }
  rclcpp::shutdown();
  return 0;
}
