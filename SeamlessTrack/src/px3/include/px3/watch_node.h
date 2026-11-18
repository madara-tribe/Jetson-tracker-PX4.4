#pragma once

// ============================================================
//  watch_node.h  — WatchNode
//
//  /pir/state (from px3) を subscribe し、
//  PIR 検知時に /px_wakeup=true を px2 へ publish する仲介ノード。
//  シリアル / Telemetrix 処理は持たない。
//
//  Subscribe:
//    /pir/state   std_msgs/Bool  (from px3)
//    /px3_ready   std_msgs/Bool  transient_local (from px3)
//    /px_wakeup   std_msgs/Bool  transient_local (from px2, false のみ使用)
//
//  Publish:
//    /px_wakeup   std_msgs/Bool  transient_local (to px2)
// ============================================================

#include <atomic>
#include <chrono>

#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/bool.hpp"

namespace px_watch {

class WatchNode : public rclcpp::Node {
public:
  explicit WatchNode(const rclcpp::NodeOptions& options = rclcpp::NodeOptions());
  ~WatchNode() override = default;

private:
  // PIR 状態受信 (from px3)
  void on_pir_state(const std_msgs::msg::Bool::SharedPtr msg);

  // px3_ready 受信 (from px3)
  void on_px3_ready(const std_msgs::msg::Bool::SharedPtr msg);

  // px_wakeup 受信 (from px2 — false が来たらリセット)
  void on_wakeup_feedback(const std_msgs::msg::Bool::SharedPtr msg);

  // wakeup publish ヘルパー
  void publish_wakeup(bool on);

private:
  // ---- Subscribers ----
  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr pir_sub_;
  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr px3_ready_sub_;
  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr wakeup_fb_sub_;

  // ---- Publisher ----
  rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr wakeup_pub_;

  // ---- State ----
  std::atomic<bool> px3_ready_{false};
  std::atomic<bool> wakeup_sent_{false}; // 二重送信防止

  // ---- Params ----
  int pir_debounce_ms_{200}; // 連続 PIR 検知に対するデバウンス (ms)
  // {} 初期化により初回 PIR 検知は必ずデバウンスをpassする（意図的）
  std::chrono::steady_clock::time_point last_wakeup_t_{};
};

} // namespace px_watch
