// ============================================================
//  px3_servo_node.cpp  — TelemetrixSerial / Px3ServoNode 実装
// ============================================================

#include <px3/px3_servo_node.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

using namespace std::chrono_literals;

namespace px3_embedded {

// ------------------------------------------------------------
// ユーティリティ
// ------------------------------------------------------------
static speed_t to_speed_t(int baudrate) {
  switch (baudrate) {
  case 9600:
    return B9600;
  case 19200:
    return B19200;
  case 38400:
    return B38400;
  case 57600:
    return B57600;
  case 115200:
    return B115200;
  case 230400:
    return B230400;
  default:
    throw std::runtime_error("Unsupported baudrate (try 115200)");
  }
}

// ============================================================
//  TelemetrixSerial
// ============================================================

TelemetrixSerial::TelemetrixSerial(const std::string& port, int baudrate)
    : port_(port), baudrate_(baudrate) {
  open_port();
}

TelemetrixSerial::~TelemetrixSerial() {
  try {
    shutdown();
  } catch (...) {
  }
}

// ------------------------------------------------------------
//  servo
// ------------------------------------------------------------
void TelemetrixSerial::servo_attach(uint8_t pin, uint16_t min_pulse, uint16_t max_pulse) {
  std::vector<uint8_t> cmd = {
      CMD_SERVO_ATTACH,
      pin,
      static_cast<uint8_t>((min_pulse >> 8) & 0xFF),
      static_cast<uint8_t>(min_pulse & 0xFF),
      static_cast<uint8_t>((max_pulse >> 8) & 0xFF),
      static_cast<uint8_t>(max_pulse & 0xFF),
  };
  send_command(cmd);
}

void TelemetrixSerial::servo_write(uint8_t pin, int angle_deg) {
  angle_deg = std::clamp(angle_deg, 0, 180);
  std::vector<uint8_t> cmd = {CMD_SERVO_WRITE, pin, static_cast<uint8_t>(angle_deg)};
  send_command(cmd);
}

void TelemetrixSerial::servo_detach(uint8_t pin) {
  std::vector<uint8_t> cmd = {CMD_SERVO_DETACH, pin};
  send_command(cmd);
}

// ------------------------------------------------------------
//  PIR setup
//    SET_PIN_MODE コマンド送信後、read_loop を起動する
// ------------------------------------------------------------
void TelemetrixSerial::set_pir_pin(uint8_t pin, PirCallback cb) {
  pir_cb_ = std::move(cb);
  pir_pin_ = pin; // 再トリガー用に保持

  send_set_pin_mode(pin);

  // read_loop 起動
  start_read_loop();
}

// SET_PIN_MODE (digital input + reporting有効) を指定ピンに送信する。
// パケット長フィールド(4) + cmd(1) + pin + mode + reporting_enabled
void TelemetrixSerial::send_set_pin_mode(uint8_t pin) {
  uint8_t cmd[5] = {4, CMD_SET_PIN_MODE, pin, PIN_MODE_INPUT, REPORTING_ENABLED};
  std::lock_guard<std::mutex> lk(write_mtx_);
  ::write(fd_, cmd, sizeof(cmd));
  // [診断ログ] SET_PIN_MODE を送ったピンを出力する。
  // retrigger_pir_reporting() 経由時に pin が正しい値（PIR_PIN=2）かを確認するため。
  fprintf(stderr, "[px3/serial] SET_PIN_MODE sent for pin=%d\n", static_cast<int>(pin));
}

// PIR reporting を再トリガー: set_pir_pin() で設定したピンに SET_PIN_MODE を再送する。
// SB612Bが現在HIGHなら直後にDIGITAL_REPORTが届き、HIGH保持中のエッジ消失を解消する。
void TelemetrixSerial::retrigger_pir_reporting() {
  send_set_pin_mode(pir_pin_);
}

// ------------------------------------------------------------
//  shutdown
// ------------------------------------------------------------
void TelemetrixSerial::shutdown() {
  if (shutdown_done_.exchange(true))
    return;

  // read_loop 停止を先に行う（fd_ を close する前に）
  stop_read_loop();

  try {
    servo_detach(static_cast<uint8_t>(SERVO_X_PIN));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    servo_detach(static_cast<uint8_t>(SERVO_Y_PIN));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  } catch (...) {
  }

  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

// ------------------------------------------------------------
//  open_port
// ------------------------------------------------------------
void TelemetrixSerial::open_port() {
  fd_ = ::open(port_.c_str(), O_RDWR | O_NOCTTY | O_SYNC);
  if (fd_ < 0) {
    throw std::runtime_error("Failed to open serial port: " + port_ + " (" + std::strerror(errno) +
                             ")");
  }

  termios tty{};
  if (tcgetattr(fd_, &tty) != 0)
    throw std::runtime_error("tcgetattr failed: " + std::string(std::strerror(errno)));

  cfsetospeed(&tty, to_speed_t(baudrate_));
  cfsetispeed(&tty, to_speed_t(baudrate_));

  tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
  tty.c_iflag &= ~IGNBRK;
  tty.c_lflag = 0;
  tty.c_oflag = 0;
  tty.c_cc[VMIN] = 0;
  tty.c_cc[VTIME] = 10; // 1s read タイムアウト（read_loop の polling 間隔）

  tty.c_iflag &= ~(IXON | IXOFF | IXANY);
  tty.c_cflag |= (CLOCAL | CREAD);
  tty.c_cflag &= ~(PARENB | PARODD);
  tty.c_cflag &= ~CSTOPB;
  tty.c_cflag &= ~CRTSCTS;

  if (tcsetattr(fd_, TCSANOW, &tty) != 0)
    throw std::runtime_error("tcsetattr failed: " + std::string(std::strerror(errno)));

  tcflush(fd_, TCIOFLUSH);
}

// ------------------------------------------------------------
//  write_all / send_command
// ------------------------------------------------------------
void TelemetrixSerial::write_all(const uint8_t* data, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = ::write(fd_, data + off, len - off);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      throw std::runtime_error("Serial write failed: " + std::string(std::strerror(errno)));
    }
    off += static_cast<size_t>(n);
  }
}

void TelemetrixSerial::send_command(const std::vector<uint8_t>& cmd_without_len) {
  if (cmd_without_len.empty())
    throw std::runtime_error("Empty command");
  if (cmd_without_len.size() > 255)
    throw std::runtime_error("Command too long");

  std::vector<uint8_t> pkt;
  pkt.reserve(cmd_without_len.size() + 1);
  pkt.push_back(static_cast<uint8_t>(cmd_without_len.size()));
  pkt.insert(pkt.end(), cmd_without_len.begin(), cmd_without_len.end());

  std::lock_guard<std::mutex> lk(write_mtx_);
  write_all(pkt.data(), pkt.size());
}

// ------------------------------------------------------------
//  read_loop 管理
// ------------------------------------------------------------
void TelemetrixSerial::start_read_loop() {
  read_running_.store(true);
  read_thread_ = std::thread(&TelemetrixSerial::read_loop, this);
}

void TelemetrixSerial::stop_read_loop() {
  read_running_.store(false);
  if (read_thread_.joinable())
    read_thread_.join();
}

void TelemetrixSerial::read_loop() {
  while (read_running_.load() && fd_ >= 0) {
    uint8_t len_byte = 0;
    ssize_t n = ::read(fd_, &len_byte, 1);
    if (n <= 0)
      continue; // タイムアウト or エラー → polling 継続

    const uint8_t len = len_byte;
    if (len == 0 || len > 32)
      continue; // sanity check

    uint8_t frame[32];
    size_t got = 0;
    while (got < len && read_running_.load()) {
      ssize_t r = ::read(fd_, frame + got, len - got);
      if (r > 0)
        got += static_cast<size_t>(r);
    }
    if (got < len)
      break;

    process_report(frame, len);
  }
}

void TelemetrixSerial::process_report(const uint8_t* frame, uint8_t len) {
  if (len < 1)
    return;
  const uint8_t report_type = frame[0];

  // [診断ログ] 受信したレポート種別を標準エラーに出力する（TelemetrixSerialはROS
  // loggerを持たない）。 再入場時に DIGITAL(=2)
  // が届いているか、別種別ばかりか、何も来ないかを切り分けるため。
  fprintf(stderr, "[px3/serial] report_type=%d len=%d\n", static_cast<int>(report_type),
          static_cast<int>(len));

  switch (report_type) {
  case REPORT_DIGITAL:
    // [DIGITAL_REPORT, pin, value]
    if (len >= 3 && pir_cb_) {
      pir_cb_(frame[1], frame[2]);
    }
    break;

  case REPORT_I_AM_HERE:
  case REPORT_FIRMWARE:
    break; // 起動確認用レポートは無視

  default:
    break;
  }
}

// ============================================================
//  Px3ServoNode
// ============================================================

Px3ServoNode::Px3ServoNode(const rclcpp::NodeOptions& options) : Node("px3", options) {
  const auto port = declare_parameter<std::string>("serial_port", "/dev/ttyACM0");
  const auto baud = declare_parameter<int>("baudrate", 115200);
  x_center_deg_ = declare_parameter<double>("x_center_deg", 90.0);
  y_center_deg_ = declare_parameter<double>("y_center_deg", 90.0);
  const int pir_pin = declare_parameter<int>("pir_pin", PIR_PIN);
  RCLCPP_INFO(
      get_logger(),
      "px3: params serial_port=%s baudrate=%d x_center_deg=%.1f y_center_deg=%.1f pir_pin=%d",
      port.c_str(), static_cast<int>(baud), x_center_deg_, y_center_deg_, pir_pin);

  const uint16_t x_pulse_min = 544;
  const uint16_t x_pulse_max = 2400;
  const uint16_t y_pulse_min = 544;
  const uint16_t y_pulse_max = 2400;
  bool init_ok = false;

  // /pir/state publisher (watch_node が subscribe)
  //   transient_local: あとから起動したノードも、最後に送った値（今の値）を受け取れる
  auto pir_qos = rclcpp::QoS(rclcpp::KeepLast(1)).reliable().transient_local();
  pir_state_pub_ = create_publisher<std_msgs::msg::Bool>("/pir/state", pir_qos);

  try {
    board_ = std::make_unique<TelemetrixSerial>(port, baud);

    // Arduino リセット完了まで待機（Telemetrix初期化に必要）
    std::this_thread::sleep_for(std::chrono::seconds(4));

    // servo 初期化
    board_->servo_detach(static_cast<uint8_t>(SERVO_X_PIN));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    board_->servo_detach(static_cast<uint8_t>(SERVO_Y_PIN));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    board_->servo_attach(static_cast<uint8_t>(SERVO_X_PIN), x_pulse_min, x_pulse_max);
    board_->servo_write(static_cast<uint8_t>(SERVO_X_PIN),
                        static_cast<int>(std::lround(x_center_deg_)));
    RCLCPP_INFO(get_logger(), "x_center_deg=%.3f was sent", x_center_deg_);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    board_->servo_attach(static_cast<uint8_t>(SERVO_Y_PIN), y_pulse_min, y_pulse_max);
    board_->servo_write(static_cast<uint8_t>(SERVO_Y_PIN),
                        static_cast<int>(std::lround(y_center_deg_)));
    RCLCPP_INFO(get_logger(), "y_center_deg=%.3f was sent", y_center_deg_);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // PIR ピン設定: SET_PIN_MODE D2 + read_loop 起動
    board_->set_pir_pin(static_cast<uint8_t>(pir_pin),
                        [this](uint8_t pin, uint8_t value) { on_pir_change(pin, value); });
    RCLCPP_INFO(get_logger(), "px3: PIR D%d configured (reporting ON)", pir_pin);

    init_ok = true;
    init_ok_ = true; // メンバフラグもセット（on_watch_resumedのガード用）
    RCLCPP_INFO(get_logger(), "px3: serial OK (servo + PIR)");

  } catch (const std::exception& e) {
    RCLCPP_ERROR(get_logger(), "px3: serial init failed: %s", e.what());
  }

  // /px3_ready publish: transient_local で遅れて起動したノードも受信可能
  auto ready_qos = rclcpp::QoS(rclcpp::KeepLast(1)).reliable().transient_local();
  ready_pub_ = create_publisher<std_msgs::msg::Bool>("px3_ready", ready_qos);

  std_msgs::msg::Bool ready_msg;
  ready_msg.data = init_ok;
  ready_pub_->publish(ready_msg);

  // PIR の今の値を定期的に送り直す（変化が無くても送る）。
  // 以前は変化したときだけ送っていたので、受け取る側が 1 回取りこぼすと、
  // PIR がいったん LOW に戻るまで起動できなかった。
  if (init_ok) {
    pir_state_timer_ = create_wall_timer(std::chrono::milliseconds(PIR_STATE_PERIOD_MS),
                                         std::bind(&Px3ServoNode::publish_pir_state, this));
  }

  // /inference subscriber
  //   キューは 1 件（最新の指令だけ使う）。以前は 10 件 + 1 件ごとに 30 ms 待っていたため、
  //   px2 が速く送ると古い角度が溜まり、遅れてサーボに届いていた。
  //   /px3_ack は受け取るノードが無かったので廃止した。
  auto angle_qos = rclcpp::QoS(rclcpp::KeepLast(1)).reliable();
  sub_ = create_subscription<custom_msgs::msg::AbsResult>(
      "inference", angle_qos, std::bind(&Px3ServoNode::on_angle, this, std::placeholders::_1));

  // /px_wakeup=false 受信 → pir_detected_ リセット
  // px2 が WATCH 復帰時に false を publish するタイミングで呼ばれる
  auto wakeup_qos = rclcpp::QoS(rclcpp::KeepLast(1)).reliable().transient_local();
  px_wakeup_sub_ = create_subscription<std_msgs::msg::Bool>(
      "px_wakeup", wakeup_qos,
      std::bind(&Px3ServoNode::on_watch_resumed, this, std::placeholders::_1));
}

Px3ServoNode::~Px3ServoNode() {
  try {
    if (board_)
      board_->shutdown();
  } catch (...) {
  }
}

// サーボは整数の角度しか受け取らない。切り捨てではなく四捨五入する
// （切り捨てだと最大 1°、四捨五入なら最大 0.5° のずれ）。待ち時間は入れない。
void Px3ServoNode::on_angle(const custom_msgs::msg::AbsResult::SharedPtr msg) {
  if (!board_)
    return;
  if (!std::isfinite(msg->x_angle) || !std::isfinite(msg->y_angle))
    return; // NaN / inf は送らない
  board_->servo_write(static_cast<uint8_t>(SERVO_X_PIN),
                      static_cast<int>(std::lround(msg->x_angle)));
  board_->servo_write(static_cast<uint8_t>(SERVO_Y_PIN),
                      static_cast<int>(std::lround(msg->y_angle)));
}

void Px3ServoNode::publish_pir_state() {
  std_msgs::msg::Bool msg;
  msg.data = pir_detected_.load();
  pir_state_pub_->publish(msg);
}

// PIR 変化通知。read_loop スレッドから呼ばれる。
// pir_detected_ は「Arduino が最後に報告した値」＝ PIR の今の値を持つ。
void Px3ServoNode::on_pir_change(uint8_t /*pin*/, uint8_t value) {
  const bool new_state = (value != 0);
  const bool old_state = pir_detected_.exchange(new_state);
  // [診断ログ] Arduinoから届いた生の値と、エッジ判定結果を毎回出力する。
  RCLCPP_INFO(get_logger(), "px3: on_pir_change value=%d (old=%d new=%d, edge=%s)", value,
              static_cast<int>(old_state), static_cast<int>(new_state),
              (new_state != old_state) ? "YES" : "no");
  if (new_state == old_state)
    return; // 変化なし（定期送信に任せる）

  // 変化したときは、定期送信を待たずにすぐ送る。
  // read_loop スレッドからの呼び出しだが、rclcpp publisher は
  // マルチスレッドから呼び出し可能（内部でロック済み）
  publish_pir_state();
}

// px2 がWATCHモードに復帰したとき /px_wakeup=false を受信する。
// 以前はここで pir_detected_ を false に戻していたが、PIR が HIGH のままだと
// 実際の値と食い違い、次の立ち上がりが来ないまま起動できなくなっていた。
// 今は値を書き換えず、今の値をそのまま送り直す。
void Px3ServoNode::on_watch_resumed(const std_msgs::msg::Bool::SharedPtr msg) {
  if (!msg->data && init_ok_) {
    // Arduino に今の値を報告させる（報告が来れば on_pir_change で値が更新される）。
    // 報告が来ない実装でも、pir_detected_ は最後の報告のままなので困らない。
    if (board_) {
      board_->retrigger_pir_reporting();
    }
    publish_pir_state();
    RCLCPP_INFO(get_logger(), "px3: WATCH resumed → PIR state re-published (level=%d)",
                static_cast<int>(pir_detected_.load()));
  }
}

} // namespace px3_embedded
