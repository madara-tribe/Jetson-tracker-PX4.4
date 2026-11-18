// ============================================================
//  watch_node.cpp  — WatchNode 実装
//
//  subscribe → publish のマッピング:
//    /pir/state=true  + px3_ready=true  → /px_wakeup=true
//    /pir/state=false                   → DEBUG ログのみ
//    /px_wakeup=false (from px2)        → wakeup_sent_ reset
// ============================================================

#include <cinttypes>

#include "px3/watch_node.h"

namespace px_watch {

WatchNode::WatchNode(const rclcpp::NodeOptions& options) : Node("watch_node", options) {
  // ---- パラメータ ----
  pir_debounce_ms_ = declare_parameter<int>("pir_debounce_ms", 200);

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
  pir_sub_ = create_subscription<std_msgs::msg::Bool>(
      "/pir/state", 10, std::bind(&WatchNode::on_pir_state, this, std::placeholders::_1));

  // ---- /px_wakeup feedback subscriber ----
  // px2 が detect mode を終了して watch mode に戻ったとき false を publish する
  // それを受け取って wakeup_sent_ をリセットする
  wakeup_fb_sub_ = create_subscription<std_msgs::msg::Bool>(
      "px_wakeup", wakeup_qos,
      std::bind(&WatchNode::on_wakeup_feedback, this, std::placeholders::_1));

  RCLCPP_INFO(get_logger(), "watch_node ready. pir_debounce_ms=%d", pir_debounce_ms_);
}

// px3 初期化完了を受け取る。true になるまで PIR 検知を無視する。
void WatchNode::on_px3_ready(const std_msgs::msg::Bool::SharedPtr msg) {
  if (msg->data) {
    px3_ready_.store(true);
    RCLCPP_INFO(get_logger(), "watch_node: px3_ready received → PIR monitoring active");
  }
}

// PIR 検知(true): px3_ready・未送信・デバウンス通過の3条件を満たせば wakeup 送信。
// PIR 解除(false): DEBUG ログのみ。wakeup リセットは px2 からの /px_wakeup=false で行う。
void WatchNode::on_pir_state(const std_msgs::msg::Bool::SharedPtr msg) {
  if (!msg->data) {
    RCLCPP_DEBUG(get_logger(), "watch_node: PIR cleared");
    return;
  }

  // ガード1: px3 が未 ready なら無視
  if (!px3_ready_.load()) {
    RCLCPP_WARN(get_logger(), "watch_node: PIR detected but px3 not ready, ignoring");
    return;
  }

  // ガード2: wakeup 送信済みなら重複送信しない（px2 から false が来るまでブロック）
  if (wakeup_sent_.load()) {
    RCLCPP_DEBUG(get_logger(), "watch_node: PIR detected but wakeup already sent");
    return;
  }

  // ガード3: 前回 wakeup から pir_debounce_ms_ 以内は無視
  const auto now = std::chrono::steady_clock::now();
  const int64_t elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - last_wakeup_t_).count();
  if (elapsed_ms < static_cast<int64_t>(pir_debounce_ms_)) {
    RCLCPP_DEBUG(get_logger(), "watch_node: PIR debounce (%" PRId64 " ms < %d ms)",
                 static_cast<int64_t>(elapsed_ms), pir_debounce_ms_);
    return;
  }

  // wakeup 送信
  last_wakeup_t_ = now;
  wakeup_sent_.store(true);
  publish_wakeup(true);

  RCLCPP_INFO(get_logger(), "watch_node: PIR detected → /px_wakeup = true");
}

// /px_wakeup=false (from px2) を受け取り wakeup_sent_ をリセット。
// 自ノードの true publish は条件 !msg->data で自動的に無視される。
void WatchNode::on_wakeup_feedback(const std_msgs::msg::Bool::SharedPtr msg) {
  if (!msg->data && wakeup_sent_.load()) {
    wakeup_sent_.store(false);
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
