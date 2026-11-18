cmake_minimum_required(VERSION 3.8)
project(px3)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ========== 依存パッケージ ==========
find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_msgs REQUIRED)
find_package(custom_msgs REQUIRED)

# ========== 共有ライブラリ作成 ==========
# px3_lib: px2 プロセスに組み込まれる px3_servo_node の実体
add_library(px3_lib SHARED
  src/px3_servo_node.cpp
)

target_include_directories(px3_lib PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(px3_lib PUBLIC cxx_std_17)

ament_target_dependencies(px3_lib
  rclcpp
  std_msgs
  custom_msgs
)

# ========== [追加] watch_node 実行ファイル ==========
# 役割: /pir/state(from px3) → /px_wakeup(to px2) の仲介ノード
# px2 プロセスとは別プロセスで起動する
add_executable(watch_node
  src/watch_node.cpp
)

target_include_directories(watch_node PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(watch_node PUBLIC cxx_std_17)

ament_target_dependencies(watch_node
  rclcpp
  std_msgs
)

# ========== ヘッダーのインストール ==========
install(
  DIRECTORY include/
  DESTINATION include
)

# ========== ライブラリのインストール + エクスポート ==========
install(
  TARGETS px3_lib
  EXPORT px3_libTargets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin
  INCLUDES DESTINATION include
)

# ========== [追加] watch_node のインストール ==========
install(
  TARGETS watch_node
  DESTINATION lib/${PROJECT_NAME}
)

# ========== CMake Targets のエクスポート ==========
ament_export_targets(px3_libTargets HAS_LIBRARY_TARGET)
ament_export_dependencies(rclcpp std_msgs custom_msgs)

ament_package()
