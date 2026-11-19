#include "yolo_inference.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>

#include "trt_engine.h"
#ifndef PX_WITH_TENSORRT
// TensorRT 無しでビルドする場合（x86 の PC など）: trt_ は常に空なので、
// unique_ptr<TrtEngine> の後片付けに必要な最小限の定義だけ置く
struct TrtEngine::Impl {};
TrtEngine::~TrtEngine() = default;
#endif

#define ACCURACY 0.25
#define NMS_THRESHOLD 0.45f // NMS の IoU しきい値
#define WARNING false

// ============================================================
//  YOLO11 COCO 80-class model（personに限らず全80クラスを出力）
//    output: [1, 84, 8400]   84 = cx,cy,w,h + 80 class scores
//    列優先 (column-major): rawOutput[f * num_detections + i]
//    raw 出力は NMS 未適用 → ここで cv::dnn::NMSBoxes をかける
//
//    本システムは人物検知専用のため、postprocess() では
//    person チャンネル(kPersonClassId)のスコアのみを読み、
//    他79クラス（bottle等）は評価対象から完全に除外する。
// ============================================================

// Ultralytics COCO標準クラス順で person = index 0
// ※学習/エクスポート時にクラス順を変えていないか要確認
constexpr int kPersonClassId = 0;
std::vector<std::string> kClassNames = {"person"}; // obj_idは常にkPersonClassId

YoloDetect::YoloDetect(const std::string& modelPath, float conf_threshold,
                       const OrtBackendOptions& backend)
    : conf_threshold_(conf_threshold) {
  // ---- TensorRT を直接使う（provider "trt"）----
  if (backend.provider == "trt") {
#ifdef PX_WITH_TENSORRT
    try {
      const std::string engine_path =
          backend.trt_engine.empty()
              ? TrtEngine::defaultEnginePath(modelPath, backend.trt_cache_dir, backend.trt_fp16)
              : backend.trt_engine;
      trt_ = std::make_unique<TrtEngine>(modelPath, engine_path, backend.trt_fp16);
      active_provider_ = "trt";
      std::cout << "[yolo] using provider: trt (native TensorRT, "
                << (backend.trt_fp16 ? "FP16" : "FP32") << ")" << std::endl;
      warmup();
      return;
    } catch (const std::exception& e) {
      std::cerr << "[yolo] WARNING: native TensorRT failed: " << e.what()
                << " -> falling back to ONNX Runtime CPU (much slower)" << std::endl;
      trt_.reset();
    }
#else
    std::cerr
        << "[yolo] WARNING: px2 was built without TensorRT -> falling back to ONNX Runtime CPU"
           " (much slower)"
        << std::endl;
#endif
  }

  // この ONNX Runtime ビルドで使えるプロバイダを確認
  const std::vector<std::string> available = Ort::GetAvailableProviders();
  auto has = [&](const char* name) {
    return std::find(available.begin(), available.end(), name) != available.end();
  };
  std::cout << "[yolo] available providers:";
  for (const auto& p : available)
    std::cout << " " << p;
  std::cout << std::endl;

  // 希望 → 下位の順に試す: tensorrt → cuda → cpu
  std::vector<std::string> order;
  if (backend.provider == "tensorrt")
    order = {"tensorrt", "cuda", "cpu"};
  else if (backend.provider == "cuda")
    order = {"cuda", "cpu"};
  else
    order = {"cpu"};

  for (const auto& p : order) {
    if (p == "tensorrt" && !has("TensorrtExecutionProvider"))
      continue;
    if (p == "cuda" && !has("CUDAExecutionProvider"))
      continue;
    try {
      createSession(modelPath, p, backend);
      active_provider_ = p;
      break;
    } catch (const std::exception& e) {
      std::cerr << "[yolo] provider '" << p << "' failed: " << e.what() << " -> trying next"
                << std::endl;
      delete session;
      session = nullptr;
    }
  }
  if (!session)
    throw std::runtime_error("yolo_inference: could not create ONNX Runtime session");

  std::cout << "[yolo] using provider: " << active_provider_ << std::endl;
  warmup();
}

void YoloDetect::createSession(const std::string& modelPath, const std::string& provider,
                               const OrtBackendOptions& backend) {
  sessionOptions = Ort::SessionOptions{};
  sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
  sessionOptions.SetIntraOpNumThreads(numthreads);
  if (!WARNING) {
    sessionOptions.SetLogSeverityLevel(4); // suppress warning messages
  }

  const OrtApi& api = Ort::GetApi();
  const std::string dev = std::to_string(backend.device_id);

  if (provider == "tensorrt") {
    std::filesystem::create_directories(backend.trt_cache_dir);
    OrtTensorRTProviderOptionsV2* trt = nullptr;
    Ort::ThrowOnError(api.CreateTensorRTProviderOptions(&trt));
    const std::string fp16 = backend.trt_fp16 ? "1" : "0";
    const char* keys[] = {"device_id",
                          "trt_fp16_enable",
                          "trt_engine_cache_enable",
                          "trt_engine_cache_path",
                          "trt_timing_cache_enable",
                          "trt_timing_cache_path",
                          "trt_max_workspace_size"};
    const char* vals[] = {dev.c_str(), fp16.c_str(),
                          "1",         backend.trt_cache_dir.c_str(),
                          "1",         backend.trt_cache_dir.c_str(),
                          "1073741824"}; // 1 GiB
    try {
      Ort::ThrowOnError(api.UpdateTensorRTProviderOptions(trt, keys, vals, 7));
      sessionOptions.AppendExecutionProvider_TensorRT_V2(*trt);
    } catch (...) {
      api.ReleaseTensorRTProviderOptions(trt);
      throw;
    }
    api.ReleaseTensorRTProviderOptions(trt);
  }

  if (provider == "tensorrt" || provider == "cuda") {
    // TensorRT が扱えない層は CUDA で、それも無理なら CPU で実行される
    OrtCUDAProviderOptionsV2* cuda = nullptr;
    Ort::ThrowOnError(api.CreateCUDAProviderOptions(&cuda));
    const char* keys[] = {"device_id"};
    const char* vals[] = {dev.c_str()};
    try {
      Ort::ThrowOnError(api.UpdateCUDAProviderOptions(cuda, keys, vals, 1));
      sessionOptions.AppendExecutionProvider_CUDA_V2(*cuda);
    } catch (...) {
      api.ReleaseCUDAProviderOptions(cuda);
      throw;
    }
    api.ReleaseCUDAProviderOptions(cuda);
  }

  // TensorRT は初回にエンジンを作るため数分かかることがある（2回目以降はキャッシュ）
  const auto t0 = std::chrono::steady_clock::now();
  session = new Ort::Session(env, modelPath.c_str(), sessionOptions);
  const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::cout << "[yolo] session created with '" << provider << "' in " << sec << " s" << std::endl;
}

// 最初の1回は遅い（GPU の初期化・エンジン読み込み）ので、追尾開始前に済ませておく
void YoloDetect::warmup() {
  std::vector<int64_t> dims;
#ifdef PX_WITH_TENSORRT
  if (trt_)
    dims = trt_->inputShape();
  else
#endif
    dims = session->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
  for (auto& d : dims)
    if (d <= 0)
      d = (&d == &dims[0]) ? 1 : 640;
  size_t n = 1;
  for (auto d : dims)
    n *= static_cast<size_t>(d);
  cv::Mat dummy(1, static_cast<int>(n), CV_32F, cv::Scalar(0));
  for (int i = 0; i < 3; ++i)
    RunInference(dummy);
  std::cout << "[yolo] warmup done, infer=" << last_infer_ms_ << " ms" << std::endl;
}

YoloDetect::~YoloDetect() {
  delete session;
  session = nullptr;
}

std::vector<Result> YoloDetect::postprocess(cv::Size originalImageSize,
                                            std::vector<Ort::Value>& outputTensors) {
  auto* rawOutput = outputTensors[0].GetTensorData<float>();
  std::vector<int64_t> shape = outputTensors[0].GetTensorTypeAndShapeInfo().GetShape();

  // shape: [1, 84, 8400]  (COCO 80class + box 4)
  const int num_features = static_cast<int>(shape[1]);   // 84
  const int num_detections = static_cast<int>(shape[2]); // 8400
  const int num_classes = num_features - 4;              // 80 (personチャンネルのみ使用)

  if (kPersonClassId >= num_classes) {
    throw std::runtime_error(
        "yolo_inference: kPersonClassId is out of range for this model output");
  }

  std::vector<cv::Rect> boxes;
  std::vector<float> confidences;
  std::vector<int> class_ids;

  for (int i = 0; i < num_detections; ++i) {
    // 列優先レイアウト: feature f, detection i → rawOutput[f * num_detections + i]
    float cx = rawOutput[0 * num_detections + i];
    float cy = rawOutput[1 * num_detections + i];
    float w = rawOutput[2 * num_detections + i];
    float h = rawOutput[3 * num_detections + i];

    // COCO 80class中、personチャンネルのスコアのみを見る。
    // 他クラス(bottle等)が高スコアでもここでは一切拾わない → 誤追尾防止。
    const float score = rawOutput[(4 + kPersonClassId) * num_detections + i];

    // 信頼度しきい値未満は誤検出として破棄
    // （これがないと背景の低信頼度な誤検出を追い続けてDETECTから抜けられなくなる）
    if (score < conf_threshold_)
      continue;

    // center 形式 (cx,cy,w,h) → corner 形式 (model/letterbox 640x640 座標)
    float x1 = cx - w * 0.5f;
    float y1 = cy - h * 0.5f;
    float x2 = cx + w * 0.5f;
    float y2 = cy + h * 0.5f;

    // letterbox 座標 → 元画像座標へスケール
    x1 = unpadCoord(x1, pad_size_x, model_width_after_padding, originalImageSize.width);
    x2 = unpadCoord(x2, pad_size_x, model_width_after_padding, originalImageSize.width);
    y1 = unpadCoord(y1, pad_size_y, model_height_after_padding, originalImageSize.height);
    y2 = unpadCoord(y2, pad_size_y, model_height_after_padding, originalImageSize.height);

    cv::Rect clipped = clipBox(x1, y1, x2, y2, originalImageSize.width, originalImageSize.height);
    if (clipped.width <= 0 || clipped.height <= 0)
      continue;

    boxes.push_back(clipped);
    confidences.push_back(score);
    class_ids.push_back(kPersonClassId);
  }

  // NMS（YOLO11 raw 出力は NMS 未適用のため必須）
  std::vector<int> indices;
  cv::dnn::NMSBoxes(boxes, confidences, conf_threshold_, NMS_THRESHOLD, indices);

  std::vector<Result> resultVector;
  for (int idx : indices) {
    const cv::Rect& b = boxes[idx];
    // 検出ごとの標準出力はやめた（毎フレーム出て遅延の原因になる）。
    // 信頼度は Result::accuracy に入り、px2 の timing CSV の best_conf 列で確認できる。
    resultVector.emplace_back(b.x, b.x + b.width, b.y, b.y + b.height, class_ids[idx],
                              confidences[idx]);
  }

  return resultVector;
}

// パディング除去 + 元画像サイズへのスケール変換。
// model座標 → (pad除去) / padding後サイズ * 元画像サイズ。
float YoloDetect::unpadCoord(float v, int pad, int model_size_after_padding, int original_size) {
  return ((v - pad) / static_cast<float>(model_size_after_padding)) * original_size;
}

cv::Mat YoloDetect::preprocess(const cv::Mat& bgr, int model_input_width, int model_input_height) {

  // Channels order: BGR to RGB（元のフレームは書き換えず、別の Mat に変換する）
  cv::Mat image;
  cv::cvtColor(bgr, image, cv::COLOR_BGR2RGB);

  // Calculate the scaling factor for resizing without distortion
  double scale;
  if (image.cols / static_cast<double>(image.rows) >
      model_input_width / static_cast<double>(model_input_height)) {
    scale = model_input_width / static_cast<double>(image.cols);
  } else {
    scale = model_input_height / static_cast<double>(image.rows);
  }

  // Resize the image with keeping the aspect ratio
  cv::Mat resizedImage;
  cv::resize(image, resizedImage, cv::Size(), scale, scale);

  model_height_after_padding = resizedImage.size[0];
  model_width_after_padding = resizedImage.size[1];
  // Create a blank canvas with the desired model input size
  cv::Mat paddedImage = cv::Mat::zeros(model_input_height, model_input_width, resizedImage.type());

  // Calculate the position to paste the resized image
  int x_offset = (paddedImage.cols - resizedImage.cols) / 2;
  int y_offset = (paddedImage.rows - resizedImage.rows) / 2;
  pad_size_y = y_offset;
  pad_size_x = x_offset;

  // Copy the resized image to the center of the canvas
  resizedImage.copyTo(
      paddedImage(cv::Rect(x_offset, y_offset, resizedImage.cols, resizedImage.rows)));
  // Convert image to float32 and normalize
  cv::Mat floatImage;
  paddedImage.convertTo(floatImage, CV_32F, 1.0 / 255.0);

  // Create a 4-dimensional blob from the image
  cv::Mat blobImage = cv::dnn::blobFromImage(floatImage);

  return blobImage;
}

cv::Mat YoloDetect::drawBoundingBox(cv::Mat& image, std::vector<Result>& resultVector) {
  for (auto result : resultVector) {

    if (result.accuracy > ACCURACY) { // Threshold, can be made function parameter

      cv::rectangle(image, cv::Point(result.x1, result.y1), cv::Point(result.x2, result.y2),
                    cv::Scalar(0, 255, 0), 2);

      cv::putText(image, kClassNames.at(result.obj_id), cv::Point(result.x1, result.y1 - 3),
                  cv::FONT_ITALIC, 0.8, cv::Scalar(255, 255, 255), 2);

      cv::putText(image, std::to_string(result.accuracy), cv::Point(result.x1, result.y1 + 30),
                  cv::FONT_ITALIC, 0.8, cv::Scalar(255, 255, 0), 2);
    }
  }
  return image;
}

cv::Rect YoloDetect::clipBox(float x1, float y1, float x2, float y2, int imageWidth,
                             int imageHeight) {
  int x1_clipped = std::max(0, std::min(static_cast<int>(x1), imageWidth - 1));
  int y1_clipped = std::max(0, std::min(static_cast<int>(y1), imageHeight - 1));
  int x2_clipped = std::max(0, std::min(static_cast<int>(x2), imageWidth - 1));
  int y2_clipped = std::max(0, std::min(static_cast<int>(y2), imageHeight - 1));
  return cv::Rect(cv::Point(x1_clipped, y1_clipped), cv::Point(x2_clipped, y2_clipped));
}

bool YoloDetect::lastTrtBreakdown(double& h2d_ms, double& gpu_ms, double& d2h_ms) const {
#ifdef PX_WITH_TENSORRT
  if (trt_) {
    h2d_ms = trt_->lastH2dMs();
    gpu_ms = trt_->lastComputeMs();
    d2h_ms = trt_->lastD2hMs();
    return true;
  }
#endif
  h2d_ms = gpu_ms = d2h_ms = 0.0;
  return false;
}

std::vector<Ort::Value> YoloDetect::RunInference(cv::Mat& inputImage) {
#ifdef PX_WITH_TENSORRT
  // ---- TensorRT 直接実行: 結果をホストの配列に受け取り、Ort::Value で包んで返す ----
  //      （postprocess() をそのまま使うため。コピーは発生しない）
  if (trt_) {
    const auto t0 = std::chrono::steady_clock::now();
    trt_->infer(inputImage.ptr<float>(), inputImage.total(), trt_out_);
    last_infer_ms_ =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const std::vector<int64_t>& shape = trt_->outputShape();
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtAllocatorType::OrtArenaAllocator,
                                                     OrtMemType::OrtMemTypeDefault);
    std::vector<Ort::Value> out;
    out.emplace_back(Ort::Value::CreateTensor<float>(mem, trt_out_.data(), trt_out_.size(),
                                                     shape.data(), shape.size()));
    return out;
  }
#endif

  size_t num_input_nodes = session->GetInputCount();
  size_t num_output_nodes = session->GetOutputCount();
  std::vector<int64_t> inputDims =
      session->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();

  Ort::MemoryInfo memoryInfo = Ort::MemoryInfo::CreateCpu(OrtAllocatorType::OrtArenaAllocator,
                                                          OrtMemType::OrtMemTypeDefault);

  Ort::Value inputTensor = Ort::Value::CreateTensor<float>(memoryInfo, inputImage.ptr<float>(),
                                                           inputImage.total() * sizeof(float),
                                                           inputDims.data(), inputDims.size());

  const auto t0 = std::chrono::steady_clock::now();
  std::vector<Ort::Value> outputTensors =
      session->Run(Ort::RunOptions{nullptr}, &input_node_name, &inputTensor, num_input_nodes,
                   &output_node_name, num_output_nodes);
  last_infer_ms_ =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return outputTensors;
}
