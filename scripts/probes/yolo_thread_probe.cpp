// YOLO 线程扩展性基准探针（2026-11-21）
//
// 目的：`OnnxYoloEngine::Impl` 里写死 `sopts.SetIntraOpNumThreads(1)`，
// 而 YOLO 推理是这条链路里唯一的重 CPU 活。改之前先量：
// 单线程 vs 2/4/8 线程到底差多少？如果只差 10%，就不值得动。
//
// 用法（先跑 scripts/gen_yolo_bench_model.py 生成模型）：
//   cl /nologo /EHsc /std:c++17 /O2 /I <ort_include> yolo_thread_probe.cpp \
//      /link <ort_lib>/onnxruntime.lib
//   yolo_thread_probe.exe workbench/tmp/yolo_bench_heavy.onnx
//
// 注意：跑之前把 onnxruntime.dll 放到 exe 同目录或 PATH 里。
#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::vector<char> read_file(const char *path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    const std::streamoff sz = f.tellg();
    f.seekg(0, std::ios::beg);
    if (sz <= 0) return {};
    std::vector<char> buf(static_cast<size_t>(sz));
    f.read(buf.data(), sz);
    return buf;
}

double median_ms(std::vector<double> v) {
    if (v.empty()) return -1.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// threads: -1 = 不调用 SetIntraOpNumThreads（ORT 默认）
void bench(Ort::Env &env, const std::vector<char> &model, int threads, int runs) {
    Ort::SessionOptions so;
    if (threads >= 0) so.SetIntraOpNumThreads(threads);
    so.SetGraphOptimizationLevel(ORT_ENABLE_ALL);
    Ort::Session sess(env, model.data(), model.size(), so);

    std::array<int64_t, 4> shape{1, 3, 640, 640};
    std::vector<float> input(size_t(1 * 3 * 640 * 640), 0.0f);
    auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    auto tensor = Ort::Value::CreateTensor<float>(mem, input.data(), input.size(), shape.data(), 4);

    const char *in_names[] = {"images"};
    const char *out_names[] = {"out"};

    // 预热 2 次（含 EP 初始化 / 内存池增长，别算进中位数）
    for (int i = 0; i < 2; ++i)
        sess.Run(Ort::RunOptions{nullptr}, in_names, &tensor, 1, out_names, 1);

    std::vector<double> samples;
    samples.reserve(size_t(runs));
    for (int i = 0; i < runs; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        sess.Run(Ort::RunOptions{nullptr}, in_names, &tensor, 1, out_names, 1);
        const auto t1 = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    const std::string tlabel = threads < 0 ? std::string("auto") : std::to_string(threads);
    std::printf("  intra-op threads=%-4s median=%8.2f ms  min=%8.2f  max=%8.2f\n", tlabel.c_str(),
                median_ms(samples), *std::min_element(samples.begin(), samples.end()),
                *std::max_element(samples.begin(), samples.end()));
}

} // namespace

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "workbench/tmp/yolo_bench_heavy.onnx";
    const int runs = argc > 2 ? std::atoi(argv[2]) : 10;
    auto model = read_file(path);
    if (model.empty()) {
        std::printf("model not found: %s\n", path);
        return 2;
    }
    std::printf("model=%s size=%zu runs=%d\n", path, model.size(), runs);
    Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "yolo-thread-probe");
    for (int t : {1, 2, 4, 8, -1}) bench(env, model, t, runs);
    return 0;
}
