#pragma once
#include "YoloService.h"
#include <map>
#include <memory>
#include <mutex>

namespace op::yolo {

// 命名模型注册表快照（YoloListModels 的数据源）。
struct NamedModelInfo {
    std::wstring name;
    std::wstring path;   // 文件模型路径；内存模型（加密包解密后）为空
    Task task = Task::Unknown;
    int input_width = 0;
    int input_height = 0;
    int device = -1;
    bool encrypted = false;
    bool active = false;
    size_t bytes = 0;
};

// 引擎选择器（单例，对外接口不变）。
// HTTP 远程后端已隐藏（2026-09-18 决策）：YOLO 依赖自训 ONNX 模型，不走 HTTP 服务。
// 现行为：engine 以 "onnx" 开头 / 空 / 未知 / .onnx 路径（OpYolo 层已归一）-> OnnxYoloEngine；
// http(s):// 及旧远程别名（yolo/yolo11/yolo_http/yolo_server）-> init 拒绝返 0。
// 如需恢复 HTTP：定义 OP_ENABLE_HTTP_YOLO_BACKEND，把 yolo/HttpYoloEngine.cpp 与
// network/HttpClient.cpp 加回 CMake 源列表，并在 YoloDetector.cpp 打开对应分支。
class YoloDetector {
  private:
    YoloDetector();

  public:
    YoloDetector(const YoloDetector &) = delete;
    YoloDetector &operator=(const YoloDetector &) = delete;
    static YoloDetector *getInstance();
    ~YoloDetector();
    int init(const std::wstring &enginePath, const std::wstring &dllName, const vector<string> &argv);
    int release();

    // ---- 命名模型注册表（多模型热切换）----
    // load_* 成功即注册并激活；失败不注册（不留半加载状态）。
    int load_named(const std::wstring &name, const std::wstring &path, const vector<string> &argv, bool encrypted);
    int load_named_memory(const std::wstring &name, const std::vector<uint8_t> &model, const vector<string> &argv,
                          bool encrypted);
    int use_named(const std::wstring &name);
    int release_named(const std::wstring &name);
    std::vector<NamedModelInfo> list_models();
    std::wstring active_model();

    int detect(byte *data, int w, int h, int bpp, double conf, double iou, vyolo_rec_t &result);
    // 批 C（2026-11-22）：任务维度。任务不符 / 未加载 返回 -1，无结果返回 0。
    int classify(byte *data, int w, int h, int bpp, int top_k, vyolo_cls_t &result);
    int segment(byte *data, int w, int h, int bpp, double conf, double iou, vyolo_seg_t &result);
    int pose(byte *data, int w, int h, int bpp, double conf, double iou, vyolo_pose_t &result);
    int obb(byte *data, int w, int h, int bpp, double conf, double iou, vyolo_obb_t &result);
    Task loaded_task();
    // 诊断：模型输入边长（未加载返回 0）。
    int model_input_width();
    int model_input_height();
    // 诊断：实际生效设备（-1 = CPU，>=0 = DirectML 适配器）。
    int device_id();
    bool dml_active();
    // 当前 onnxruntime.dll 是否带 DirectML EP（设备枚举用）。
    bool dml_supported();

  private:
    struct NamedModel {
        std::wstring name;
        std::wstring path;
        std::vector<uint8_t> data; // 非空 = 内存模型（加密包解密后），不落盘
        vector<string> argv;
        bool encrypted = false;
        Task task = Task::Unknown; // 该模型上次成功加载时判定出的任务/输入尺寸/设备
        int input_width = 0;
        int input_height = 0;
        int device = -1;
    };

    int init_locked(const std::wstring &enginePath, const std::wstring &dllName, const vector<string> &argv,
                    const std::vector<uint8_t> *mem);
    void release_locked();
    void snapshot_locked(NamedModelInfo &info) const;

    std::mutex m_mutex;
    std::unique_ptr<YoloEngine> m_engine;
    std::map<std::wstring, NamedModel> m_models;
    std::wstring m_active;
};

} // namespace op::yolo
