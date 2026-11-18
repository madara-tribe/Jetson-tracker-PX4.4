// ============================================================
//  trt_engine.cpp — TensorRT 10 (JetPack 6) の推論ランナー
// ============================================================
#include "trt_engine.h"

#include <NvInfer.h>
#include <NvOnnxParser.h>
#include <cuda_runtime_api.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {

void checkCuda(cudaError_t e, const char* what) {
  if (e != cudaSuccess) {
    throw std::runtime_error(std::string("[trt] CUDA error at ") + what + ": " +
                             cudaGetErrorString(e));
  }
}

class Logger : public nvinfer1::ILogger {
public:
  // メモリ不足で候補（tactic）を飛ばしたメッセージは大量に出るので、数えるだけにする
  int skipped_tactics = 0;
  void log(Severity severity, const char* msg) noexcept override {
    if (severity > Severity::kWARNING)
      return;
    const std::string m(msg);
    if (m.find("insufficient") != std::string::npos ||
        m.find("Skipping tactic") != std::string::npos) {
      ++skipped_tactics;
      return;
    }
    std::cerr << "[trt] " << m << std::endl;
  }
};
Logger gLogger;

std::vector<int64_t> toVec(const nvinfer1::Dims& d) {
  return std::vector<int64_t>(d.d, d.d + d.nbDims);
}

size_t volume(const std::vector<int64_t>& s) {
  size_t v = 1;
  for (auto x : s)
    v *= static_cast<size_t>(x);
  return v;
}

std::string shapeStr(const std::vector<int64_t>& s) {
  std::ostringstream os;
  os << "[";
  for (size_t i = 0; i < s.size(); ++i)
    os << (i ? "," : "") << s[i];
  os << "]";
  return os.str();
}

} // namespace

struct TrtEngine::Impl {
  std::unique_ptr<nvinfer1::IRuntime> runtime;
  std::unique_ptr<nvinfer1::ICudaEngine> engine;
  std::unique_ptr<nvinfer1::IExecutionContext> context;
  cudaStream_t stream = nullptr;
  cudaEvent_t ev[4] = {nullptr, nullptr, nullptr, nullptr};

  std::string input_name, output_name, engine_path;
  std::vector<int64_t> in_shape, out_shape;
  void* d_in = nullptr;
  void* d_out = nullptr;
  size_t in_count = 0, out_count = 0;
  double h2d_ms = 0, compute_ms = 0, d2h_ms = 0;

  ~Impl() {
    if (d_in)
      cudaFree(d_in);
    if (d_out)
      cudaFree(d_out);
    for (auto& e : ev)
      if (e)
        cudaEventDestroy(e);
    if (stream)
      cudaStreamDestroy(stream);
    context.reset();
    engine.reset();
    runtime.reset();
  }

  // ONNX → シリアライズ済みエンジンを作ってファイルに保存
  static void build(const std::string& onnx, const std::string& out_path, bool fp16) {
    std::cout << "[trt] building engine from " << onnx << " (" << (fp16 ? "FP16" : "FP32")
              << "). This takes a few minutes the first time (camera starts after this)..."
              << std::endl;
    gLogger.skipped_tactics = 0;
    const auto t0 = std::chrono::steady_clock::now();

    std::unique_ptr<nvinfer1::IBuilder> builder{nvinfer1::createInferBuilder(gLogger)};
    if (!builder)
      throw std::runtime_error("[trt] createInferBuilder failed");
    std::unique_ptr<nvinfer1::INetworkDefinition> network{builder->createNetworkV2(0U)};
    std::unique_ptr<nvonnxparser::IParser> parser{nvonnxparser::createParser(*network, gLogger)};
    if (!parser->parseFromFile(onnx.c_str(),
                               static_cast<int>(nvinfer1::ILogger::Severity::kWARNING))) {
      std::ostringstream os;
      os << "[trt] ONNX parse failed:";
      for (int i = 0; i < parser->getNbErrors(); ++i)
        os << "\n  " << parser->getError(i)->desc();
      throw std::runtime_error(os.str());
    }

    std::unique_ptr<nvinfer1::IBuilderConfig> config{builder->createBuilderConfig()};
    config->setMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE, 512ULL << 20); // 512 MiB
    if (fp16)
      config->setFlag(nvinfer1::BuilderFlag::kFP16);

    // 入力サイズが可変 (-1) のモデルは 1x3x640x640 に固定する
    nvinfer1::ITensor* in = network->getInput(0);
    nvinfer1::Dims d = in->getDimensions();
    bool dynamic = false;
    for (int i = 0; i < d.nbDims; ++i)
      dynamic |= (d.d[i] < 0);
    if (dynamic) {
      nvinfer1::Dims4 fixed{1, 3, 640, 640};
      auto* profile = builder->createOptimizationProfile();
      profile->setDimensions(in->getName(), nvinfer1::OptProfileSelector::kMIN, fixed);
      profile->setDimensions(in->getName(), nvinfer1::OptProfileSelector::kOPT, fixed);
      profile->setDimensions(in->getName(), nvinfer1::OptProfileSelector::kMAX, fixed);
      config->addOptimizationProfile(profile);
    }

    std::unique_ptr<nvinfer1::IHostMemory> plan{builder->buildSerializedNetwork(*network, *config)};
    if (!plan || plan->size() == 0)
      throw std::runtime_error("[trt] buildSerializedNetwork failed");

    const auto parent = std::filesystem::path(out_path).parent_path();
    if (!parent.empty())
      std::filesystem::create_directories(parent);
    std::ofstream f(out_path, std::ios::binary);
    f.write(static_cast<const char*>(plan->data()), static_cast<std::streamsize>(plan->size()));
    if (!f)
      throw std::runtime_error("[trt] cannot write engine: " + out_path);

    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "[trt] engine saved: " << out_path << " (" << plan->size() / (1024 * 1024)
              << " MB, " << sec << " s)" << std::endl;
    if (gLogger.skipped_tactics > 0) {
      std::cout << "[trt] NOTE: " << gLogger.skipped_tactics
                << " candidate kernels were skipped because GPU memory was low. The engine works,"
                   " but may be slower. Free memory (close other programs) and delete the"
                   " .engine to rebuild."
                << std::endl;
    }
  }

  // ファイルから読み込み。失敗したら false（版違いなど）
  bool load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
      return false;
    std::vector<char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.empty())
      return false;
    if (!runtime)
      runtime.reset(nvinfer1::createInferRuntime(gLogger));
    engine.reset(runtime->deserializeCudaEngine(data.data(), data.size()));
    return static_cast<bool>(engine);
  }

  void setupIO() {
    context.reset(engine->createExecutionContext());
    if (!context)
      throw std::runtime_error("[trt] createExecutionContext failed");

    for (int i = 0; i < engine->getNbIOTensors(); ++i) {
      const char* name = engine->getIOTensorName(i);
      if (engine->getTensorDataType(name) != nvinfer1::DataType::kFLOAT) {
        throw std::runtime_error(std::string("[trt] tensor is not float32: ") + name);
      }
      if (engine->getTensorIOMode(name) == nvinfer1::TensorIOMode::kINPUT) {
        if (!input_name.empty())
          throw std::runtime_error("[trt] model has more than one input");
        input_name = name;
        nvinfer1::Dims d = engine->getTensorShape(name);
        bool dynamic = false;
        for (int k = 0; k < d.nbDims; ++k)
          dynamic |= (d.d[k] < 0);
        if (dynamic)
          context->setInputShape(name, nvinfer1::Dims4{1, 3, 640, 640});
      } else if (output_name.empty()) {
        output_name = name; // YOLO は出力 1 つ（output0）
      }
    }
    if (input_name.empty() || output_name.empty())
      throw std::runtime_error("[trt] input/output tensor not found");

    in_shape = toVec(context->getTensorShape(input_name.c_str()));
    out_shape = toVec(context->getTensorShape(output_name.c_str()));
    in_count = volume(in_shape);
    out_count = volume(out_shape);

    checkCuda(cudaStreamCreate(&stream), "cudaStreamCreate");
    for (auto& e : ev)
      checkCuda(cudaEventCreate(&e), "cudaEventCreate");
    checkCuda(cudaMalloc(&d_in, in_count * sizeof(float)), "cudaMalloc(in)");
    checkCuda(cudaMalloc(&d_out, out_count * sizeof(float)), "cudaMalloc(out)");
    if (!context->setTensorAddress(input_name.c_str(), d_in) ||
        !context->setTensorAddress(output_name.c_str(), d_out)) {
      throw std::runtime_error("[trt] setTensorAddress failed");
    }
    std::cout << "[trt] engine ready: " << engine_path << "  in " << input_name
              << shapeStr(in_shape) << "  out " << output_name << shapeStr(out_shape) << std::endl;
  }
};

TrtEngine::TrtEngine(const std::string& onnx_path, const std::string& engine_path, bool fp16)
    : impl_(std::make_unique<Impl>()) {
  impl_->engine_path = engine_path;
  if (!impl_->load(engine_path)) {
    if (std::filesystem::exists(engine_path)) {
      std::cerr << "[trt] cannot load " << engine_path
                << " (built with another TensorRT?) -> rebuilding" << std::endl;
    }
    Impl::build(onnx_path, engine_path, fp16);
    if (!impl_->load(engine_path))
      throw std::runtime_error("[trt] failed to load freshly built engine: " + engine_path);
  }
  impl_->setupIO();
}

TrtEngine::~TrtEngine() = default;

void TrtEngine::infer(const float* input, size_t input_count, std::vector<float>& out) {
  Impl& m = *impl_;
  if (input_count != m.in_count) {
    throw std::runtime_error("[trt] input size mismatch: got " + std::to_string(input_count) +
                             ", engine expects " + std::to_string(m.in_count));
  }
  out.resize(m.out_count);
  checkCuda(cudaEventRecord(m.ev[0], m.stream), "event0");
  checkCuda(
      cudaMemcpyAsync(m.d_in, input, m.in_count * sizeof(float), cudaMemcpyHostToDevice, m.stream),
      "H2D");
  checkCuda(cudaEventRecord(m.ev[1], m.stream), "event1");
  if (!m.context->enqueueV3(m.stream))
    throw std::runtime_error("[trt] enqueueV3 failed");
  checkCuda(cudaEventRecord(m.ev[2], m.stream), "event2");
  checkCuda(cudaMemcpyAsync(out.data(), m.d_out, m.out_count * sizeof(float),
                            cudaMemcpyDeviceToHost, m.stream),
            "D2H");
  checkCuda(cudaEventRecord(m.ev[3], m.stream), "event3");
  checkCuda(cudaStreamSynchronize(m.stream), "sync");

  float a = 0, b = 0, c = 0;
  cudaEventElapsedTime(&a, m.ev[0], m.ev[1]);
  cudaEventElapsedTime(&b, m.ev[1], m.ev[2]);
  cudaEventElapsedTime(&c, m.ev[2], m.ev[3]);
  m.h2d_ms = a;
  m.compute_ms = b;
  m.d2h_ms = c;
}

const std::vector<int64_t>& TrtEngine::inputShape() const {
  return impl_->in_shape;
}
const std::vector<int64_t>& TrtEngine::outputShape() const {
  return impl_->out_shape;
}
const std::string& TrtEngine::enginePath() const {
  return impl_->engine_path;
}
double TrtEngine::lastH2dMs() const {
  return impl_->h2d_ms;
}
double TrtEngine::lastComputeMs() const {
  return impl_->compute_ms;
}
double TrtEngine::lastD2hMs() const {
  return impl_->d2h_ms;
}

std::string TrtEngine::defaultEnginePath(const std::string& onnx_path, const std::string& dir,
                                         bool fp16) {
  namespace fs = std::filesystem;
  const fs::path p(onnx_path);
  const auto size = fs::exists(p) ? fs::file_size(p) : 0;
  return (fs::path(dir) /
          (p.stem().string() + (fp16 ? "_fp16_" : "_fp32_") + std::to_string(size) + ".engine"))
      .string();
}
