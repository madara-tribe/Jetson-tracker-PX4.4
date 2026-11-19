#pragma once

#include <cstring>
#include <iostream>
#include <memory>
#include <onnxruntime/core/session/onnxruntime_cxx_api.h>
#include <opencv2/opencv.hpp>
#include <string>
#include <vector>

// NOTE: kCocoClassNames の extern 宣言は削除済み。
// 現行モデルは COCO 80-class model だが、postprocess() で person
// チャンネル(kPersonClassId)のみを読み取り、他クラスは無視する設計。
// obj_id は常に kPersonClassId になるため、kClassNames は
// yolo_inference.cpp 側で {"person"} の1要素のみ定義して使用する。

struct Result {
  int x1;
  int x2;
  int y1;
  int y2;
  int obj_id;
  float accuracy;

  Result(int x1_, int x2_, int y1_, int y2_, int obj_id_, float accuracy_) {
    x1 = x1_;
    x2 = x2_;
    y1 = y1_;
    y2 = y2_;
    obj_id = obj_id_;
    accuracy = accuracy_;
  }
};

class TrtEngine; // trt_engine.h（TensorRT 直接実行。ビルド時に TensorRT がある場合のみ有効）

// ------------------------------------------------------------
//  実行プロバイダ（推論デバイス）の設定
//    provider : "trt"      TensorRT を直接使う（ONNX Runtime を通さない。Step 4）
//               "tensorrt" ONNX Runtime の TensorRT 実行プロバイダ（GPU 版 ORT が必要）
//               "cuda"     ONNX Runtime の CUDA 実行プロバイダ（GPU 版 ORT が必要）
//               "cpu"      ONNX Runtime の CPU
//               使えない場合は CPU などへ自動で切り替える
//    trt_fp16 : TensorRT を FP16 で動かす（Orin は FP16 で大きく速くなる）
//    trt_cache_dir : TensorRT エンジンの保存先。2回目以降の起動が速くなる
// ------------------------------------------------------------
struct OrtBackendOptions {
  std::string provider = "tensorrt";
  bool trt_fp16 = true;
  std::string trt_cache_dir = "/tmp/px_trt_cache";
  std::string trt_engine = ""; // "trt" 用 .engine のパス（空なら trt_cache_dir に自動で作る）
  int device_id = 0;
};

class YoloDetect {
public:
  // conf_threshold: 検出採用の信頼度しきい値。これ未満のaccuracyの検出は破棄する。
  YoloDetect(const std::string& modelPath, float conf_threshold = 0.25f,
             const OrtBackendOptions& backend = OrtBackendOptions{});
  const std::string& activeProvider() const {
    return active_provider_;
  }
  double lastInferMs() const {
    return last_infer_ms_;
  }
  // "trt" のときだけ有効: 直近 1 回の CPU→GPU / GPU 計算 / GPU→CPU（ms）。それ以外は false
  bool lastTrtBreakdown(double& h2d_ms, double& gpu_ms, double& d2h_ms) const;
  ~YoloDetect(); // session (Ort::Session*) の解放
  cv::Mat drawBoundingBox(cv::Mat& image, std::vector<Result>& resultVector);
  // image（BGR）は書き換えない。呼び出し側で同じフレームを保存・描画に使える。
  cv::Mat preprocess(const cv::Mat& image, int model_input_width, int model_input_height);
  std::vector<Result> postprocess(cv::Size originalImageSize,
                                  std::vector<Ort::Value>& outputTensors);
  std::vector<Ort::Value> RunInference(cv::Mat& inputImage);
  cv::Rect clipBox(float x1, float y1, float x2, float y2, int imageWidth, int imageHeight);

private:
  // パディング除去 + 元画像サイズへのスケール変換ヘルパー
  static float unpadCoord(float v, int pad, int model_size_after_padding, int original_size);
  // 指定プロバイダでセッションを作る（失敗したら例外）
  void createSession(const std::string& modelPath, const std::string& provider,
                     const OrtBackendOptions& backend);
  void warmup();

  std::unique_ptr<TrtEngine> trt_; // provider "trt" のときだけ使う
  std::vector<float> trt_out_;     // TensorRT の出力（ホスト側のコピー）
  std::string active_provider_ = "cpu";
  double last_infer_ms_ = 0.0;

  float conf_threshold_ = 0.5f; // 検出採用の信頼度しきい値
  Ort::SessionOptions sessionOptions;
  Ort::Env env;
  Ort::Session* session = nullptr;
  int numthreads = 0;
  const char* input_node_name = "images";
  const char* output_node_name = "output0";

  int pad_size_y;
  int pad_size_x;
  int model_width_after_padding;
  int model_height_after_padding;
};
