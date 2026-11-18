// ============================================================
//  bench_yolo.cpp — オフライン推論ベンチマーク（ROS 不要）
//
//  px2 と同じ YoloDetect（前処理・推論・後処理）を、同じ入力で N 回まわして
//  1 回ごとの時間を CSV に書く。カメラや人の動きに左右されないので、
//  CPU / CUDA / TensorRT FP32 / FP16 を公平に比べられる。
//
//  使い方:
//    bench_yolo --model weights/yolo11.onnx --provider cpu --iters 300
//               --image sample.jpg --csv out.csv
//  オプション:
//    --provider  cpu | trt | cuda | tensorrt  (既定 cpu)
//                trt = TensorRT を直接使う / cuda・tensorrt = GPU 版 ONNX Runtime が必要
//    --fp16      0 | 1                     (trt / tensorrt のみ有効, 既定 1)
//    --engine    trt 用 .engine のパス（省略時は --cache に自動で作る）
//    --cache     TensorRT エンジンの保存先 (既定 /tmp/px_trt_cache)
//    --iters     計測回数 (既定 300)
//    --warmup    計測前の空回し回数 (既定 20)
//    --image     入力画像。省略時は 1920x1080 の合成画像
//    --width/--height  合成画像のサイズ (既定 1920x1080)
//    --csv       1 回ごとの結果を書く CSV (省略時は書かない)
//    --label     CSV の label 列に入れる名前 (既定: provider 名)
// ============================================================
#include "yolo_inference.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <numeric>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
double msBetween(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}

double percentile(std::vector<double> v, double p) {
  if (v.empty())
    return 0.0;
  std::sort(v.begin(), v.end());
  const double idx = p / 100.0 * (v.size() - 1);
  const size_t lo = static_cast<size_t>(idx);
  const size_t hi = std::min(lo + 1, v.size() - 1);
  return v[lo] + (v[hi] - v[lo]) * (idx - lo);
}

double mean(const std::vector<double>& v) {
  return v.empty() ? 0.0 : std::accumulate(v.begin(), v.end(), 0.0) / v.size();
}

void usage() {
  std::printf(
      "usage: bench_yolo --model M.onnx [--provider cpu|cuda|tensorrt] [--fp16 0|1]\n"
      "                  [--cache DIR] [--engine E] [--iters N] [--warmup N] [--image IMG]\n"
      "                  [--width W --height H] [--csv OUT.csv] [--label NAME]\n");
}

} // namespace

int main(int argc, char** argv) {
  std::map<std::string, std::string> a = {{"--model", ""},      {"--provider", "cpu"},
                                          {"--fp16", "1"},      {"--cache", "/tmp/px_trt_cache"},
                                          {"--iters", "300"},   {"--warmup", "20"},
                                          {"--image", ""},      {"--width", "1920"},
                                          {"--height", "1080"}, {"--csv", ""},
                                          {"--label", ""},      {"--engine", ""}};
  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    if (k == "-h" || k == "--help") {
      usage();
      return 0;
    }
    if (!a.count(k) || i + 1 >= argc) {
      std::fprintf(stderr, "unknown or incomplete option: %s\n", k.c_str());
      usage();
      return 2;
    }
    a[k] = argv[++i];
  }
  if (a["--model"].empty()) {
    usage();
    return 2;
  }

  OrtBackendOptions backend;
  backend.provider = a["--provider"];
  backend.trt_fp16 = (a["--fp16"] == "1");
  backend.trt_cache_dir = a["--cache"];
  backend.trt_engine = a["--engine"];
  const int iters = std::stoi(a["--iters"]);
  const int warmup = std::stoi(a["--warmup"]);

  // ---- 入力画像 ----
  cv::Mat src;
  if (!a["--image"].empty()) {
    src = cv::imread(a["--image"], cv::IMREAD_COLOR);
    if (src.empty()) {
      std::fprintf(stderr, "cannot read image: %s\n", a["--image"].c_str());
      return 1;
    }
  } else {
    src = cv::Mat(std::stoi(a["--height"]), std::stoi(a["--width"]), CV_8UC3);
    cv::randu(src, cv::Scalar::all(0), cv::Scalar::all(255));
  }

  // ---- セッション作成（TensorRT は初回だけエンジン作成で数分かかる）----
  const auto ts0 = Clock::now();
  YoloDetect yolo(a["--model"], 0.25f, backend);
  const double session_s = msBetween(ts0, Clock::now()) / 1000.0;
  const std::string provider = yolo.activeProvider();
  const bool fp16_used = ((provider == "tensorrt" || provider == "trt") && backend.trt_fp16);
  const std::string label = a["--label"].empty() ? provider : a["--label"];
  if (provider != backend.provider) {
    std::fprintf(stderr, "WARNING: requested '%s' but running on '%s'\n", backend.provider.c_str(),
                 provider.c_str());
  }

  std::ofstream csv;
  if (!a["--csv"].empty()) {
    const auto parent = std::filesystem::path(a["--csv"]).parent_path();
    if (!parent.empty())
      std::filesystem::create_directories(parent);
    csv.open(a["--csv"]);
    csv << "label,provider,fp16,iter,pre_ms,infer_ms,post_ms,total_ms,n_det,h2d_ms,gpu_ms,d2h_ms\n";
  }

  std::vector<double> pre, inf, post, total, h2d, gpu, d2h;
  for (int i = -warmup; i < iters; ++i) {
    cv::Mat frame = src.clone(); // preprocess は画像を書き換えるので毎回コピー
    const auto t0 = Clock::now();
    cv::Mat blob = yolo.preprocess(frame, 640, 640);
    const auto t1 = Clock::now();
    std::vector<Ort::Value> out = yolo.RunInference(blob);
    const auto t2 = Clock::now();
    std::vector<Result> res = yolo.postprocess(src.size(), out);
    const auto t3 = Clock::now();
    if (i < 0)
      continue; // ウォームアップは記録しない
    pre.push_back(msBetween(t0, t1));
    inf.push_back(msBetween(t1, t2));
    post.push_back(msBetween(t2, t3));
    total.push_back(msBetween(t0, t3));
    double a1 = 0, a2 = 0, a3 = 0;
    const bool has_trt = yolo.lastTrtBreakdown(a1, a2, a3);
    if (has_trt) {
      h2d.push_back(a1);
      gpu.push_back(a2);
      d2h.push_back(a3);
    }
    if (csv.is_open()) {
      csv << label << ',' << provider << ',' << (fp16_used ? 1 : 0) << ',' << i << ',' << pre.back()
          << ',' << inf.back() << ',' << post.back() << ',' << total.back() << ',' << res.size()
          << ',' << a1 << ',' << a2 << ',' << a3 << '\n';
    }
  }

  std::printf("\n=== bench_yolo: %s (provider=%s fp16=%d) ===\n", label.c_str(), provider.c_str(),
              fp16_used ? 1 : 0);
  std::printf("session create : %.1f s\n", session_s);
  std::printf("iters          : %d (warmup %d)\n", iters, warmup);
  std::printf("%-8s %8s %8s %8s %8s\n", "stage", "mean", "p50", "p95", "max");
  auto row = [](const char* n, const std::vector<double>& v) {
    std::printf("%-8s %8.2f %8.2f %8.2f %8.2f  ms\n", n, mean(v), percentile(v, 50),
                percentile(v, 95), v.empty() ? 0.0 : *std::max_element(v.begin(), v.end()));
  };
  row("pre", pre);
  row("infer", inf);
  row("post", post);
  row("total", total);
  if (!gpu.empty()) {
    std::printf("-- inside infer (TensorRT) --\n");
    row("h2d", h2d);
    row("gpu", gpu);
    row("d2h", d2h);
  }
  std::printf("throughput     : %.1f FPS (1000 / mean total)\n", 1000.0 / mean(total));
  return 0;
}
