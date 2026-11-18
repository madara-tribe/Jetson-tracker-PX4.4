cmake_minimum_required(VERSION 3.8)
project(px2)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_msgs REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(cv_bridge REQUIRED)
find_package(image_transport REQUIRED)
find_package(ament_index_cpp REQUIRED)
find_package(custom_msgs REQUIRED)
find_package(px3 REQUIRED)

find_package(OpenCV REQUIRED COMPONENTS
  core
  imgproc
  imgcodecs
  videoio
  highgui
  dnn
  video
  tracking
)

# The CI workflow installs the ONNX Runtime binary SDK under /usr/local.
# yolo_inference.h includes:
#   <onnxruntime/core/session/onnxruntime_cxx_api.h>
# Therefore the include root must contain the "onnxruntime/core/session" tree.
find_path(
  ONNX_RUNTIME_INCLUDE_DIR
  NAMES onnxruntime/core/session/onnxruntime_cxx_api.h
  HINTS
    /usr/local/include
    /usr/include
)

find_library(
  ONNX_RUNTIME_LIB
  NAMES onnxruntime
  HINTS
    /usr/local/lib
    /usr/local/lib64
    /usr/lib
    /usr/lib/x86_64-linux-gnu
)

if(NOT ONNX_RUNTIME_INCLUDE_DIR)
  message(FATAL_ERROR "ONNX Runtime header not found: onnxruntime/core/session/onnxruntime_cxx_api.h")
endif()

if(NOT ONNX_RUNTIME_LIB)
  message(FATAL_ERROR "ONNX Runtime library not found: libonnxruntime.so")
endif()

# ---- TensorRT（JetPack に同梱）。見つかったときだけ provider "trt" を有効にする ----
option(PX_ENABLE_TENSORRT "Build native TensorRT backend (provider trt)" ON)
set(PX_TRT_SOURCES "")
set(PX_TRT_LIBS "")
set(PX_TRT_INCLUDES "")
if(PX_ENABLE_TENSORRT)
  find_package(CUDAToolkit QUIET)
  find_path(TENSORRT_INCLUDE_DIR NvInfer.h
    HINTS /usr/include/aarch64-linux-gnu /usr/include/x86_64-linux-gnu
          /usr/local/TensorRT/include /usr/include)
  find_library(TENSORRT_NVINFER nvinfer
    HINTS /usr/lib/aarch64-linux-gnu /usr/lib/x86_64-linux-gnu /usr/local/TensorRT/lib)
  find_library(TENSORRT_ONNXPARSER nvonnxparser
    HINTS /usr/lib/aarch64-linux-gnu /usr/lib/x86_64-linux-gnu /usr/local/TensorRT/lib)
  if(CUDAToolkit_FOUND AND TENSORRT_INCLUDE_DIR AND TENSORRT_NVINFER AND TENSORRT_ONNXPARSER)
    message(STATUS "px2: TensorRT enabled (${TENSORRT_NVINFER})")
    set(PX_TRT_SOURCES src/trt_engine.cpp)
    set(PX_TRT_LIBS ${TENSORRT_NVINFER} ${TENSORRT_ONNXPARSER} CUDA::cudart)
    set(PX_TRT_INCLUDES ${TENSORRT_INCLUDE_DIR} ${CUDAToolkit_INCLUDE_DIRS})
    add_compile_definitions(PX_WITH_TENSORRT)
  else()
    message(WARNING "px2: TensorRT/CUDA not found -> provider 'trt' disabled")
  endif()
endif()

add_executable(${PROJECT_NAME}
  src/inference.cpp
  src/yolo_inference.cpp
  ${PX_TRT_SOURCES}
)

target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_17)

target_include_directories(${PROJECT_NAME} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${ONNX_RUNTIME_INCLUDE_DIR}
  ${OpenCV_INCLUDE_DIRS}
)
if(PX_TRT_INCLUDES)
  target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE ${PX_TRT_INCLUDES})
endif()

target_link_libraries(${PROJECT_NAME}
  ${ONNX_RUNTIME_LIB}
  ${OpenCV_LIBS}
  ${PX_TRT_LIBS}
  ament_index_cpp::ament_index_cpp
)

ament_target_dependencies(${PROJECT_NAME}
  rclcpp
  std_msgs
  sensor_msgs
  cv_bridge
  image_transport
  ament_index_cpp
  custom_msgs
  px3
)

# ---- オフライン計測ツール（ROS 不要。px2 と同じ YoloDetect を使う）----
add_executable(bench_yolo
  src/tools/bench_yolo.cpp
  src/yolo_inference.cpp
  ${PX_TRT_SOURCES}
)
target_compile_features(bench_yolo PUBLIC cxx_std_17)
target_include_directories(bench_yolo PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${ONNX_RUNTIME_INCLUDE_DIR}
  ${OpenCV_INCLUDE_DIRS}
)
if(PX_TRT_INCLUDES)
  target_include_directories(bench_yolo SYSTEM PRIVATE ${PX_TRT_INCLUDES})
endif()
target_link_libraries(bench_yolo
  ${ONNX_RUNTIME_LIB}
  ${OpenCV_LIBS}
  ${PX_TRT_LIBS}
)

install(
  TARGETS ${PROJECT_NAME} bench_yolo
  DESTINATION lib/${PROJECT_NAME}
)

install(
  DIRECTORY data/
  DESTINATION share/${PROJECT_NAME}/data
)

install(
  DIRECTORY weights/
  DESTINATION share/${PROJECT_NAME}/weights
)

ament_package()
