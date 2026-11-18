#pragma once
// ============================================================
//  trt_engine.h — TensorRT を直接使う推論（ONNX Runtime を使わない）
//
//  - .engine があれば読み込む。無い / 読めない（TensorRT の版違いなど）場合は
//    ONNX から作って保存する（初回は数分かかる）
//  - 入出力は float32（FP16 エンジンでも入出力は float32 のまま）
//  - CUDA / TensorRT のヘッダはこのファイルの外に出さない（pimpl）
//  ※ CMake で TensorRT が見つかったときだけビルドされる（PX_WITH_TENSORRT）
// ============================================================
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class TrtEngine {
public:
  TrtEngine(const std::string& onnx_path, const std::string& engine_path, bool fp16);
  ~TrtEngine();
  TrtEngine(const TrtEngine&) = delete;
  TrtEngine& operator=(const TrtEngine&) = delete;

  // input: CHW の float 配列（要素数は inputShape の積と一致すること）
  // out  : 出力をホストにコピーして返す（形は outputShape）
  void infer(const float* input, size_t input_count, std::vector<float>& out);

  const std::vector<int64_t>& inputShape() const;
  const std::vector<int64_t>& outputShape() const;
  const std::string& enginePath() const;

  // 直近 1 回の内訳（ms）: CPU→GPU コピー / GPU 計算 / GPU→CPU コピー
  double lastH2dMs() const;
  double lastComputeMs() const;
  double lastD2hMs() const;

  // 既定のエンジン保存名: <dir>/<onnx名>_<fp16|fp32>_<onnxサイズ>.engine
  static std::string defaultEnginePath(const std::string& onnx_path, const std::string& dir,
                                       bool fp16);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
