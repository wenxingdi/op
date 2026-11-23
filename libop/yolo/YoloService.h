#pragma once
#include "../base/Types.h"
#include <cstdint>
#include <string>
#include <vector>

namespace op::yolo {

// 推理任务类型（批 C 2026-11-22 引入）。
// 背景：原引擎抽象只有 detect()，解码器写死检测输出布局 [1, nattr, nanch]，
// 因此 classify / segment 这类模型根本无法从这个入口进来。
// 现在把"任务"提升为一等公民：init 时按模型输出形状/元数据判定 task，
// 各任务有独立的解码器，但共用同一套预处理（letterbox + CHW/255）与会话管理。
enum class Task {
    Unknown = 0,
    Detect = 1,   // [1, 4+nc, nanch] 或 [1, nanch, 4+nc] / v5 [1, nanch, 5+nc]
    Segment = 2,  // 检测头 + 掩码系数头：[1, 4+nc+nm, nanch] + proto [1, nm, H/4, W/4]
    Classify = 3, // [1, nc] 或 [1, nc, 1, 1]，整图分类
    Pose = 4,     // [1, 4+nc+3*kpt, nanch]，检测框 + 每个关键点 (x,y,conf)
    Obb = 5,      // [1, 4+nc+1, nanch]，旋转框，最后一位是弧度角
};

inline const wchar_t *TaskName(Task task) {
    switch (task) {
    case Task::Detect:
        return L"detect";
    case Task::Segment:
        return L"segment";
    case Task::Classify:
        return L"classify";
    case Task::Pose:
        return L"pose";
    case Task::Obb:
        return L"obb";
    default:
        return L"none";
    }
}

// 分类结果（按 score 降序，最多 topk 条）。
struct yolo_cls_t {
    int class_id = -1;
    std::wstring label;
    float score = 0.0f;
};
using vyolo_cls_t = std::vector<yolo_cls_t>;

// 分割结果：复用检测框 + 掩码。
// 掩码用**多边形**表示（原图像素坐标），而不是 4 位 H/4 x W/4 的原始 proto 张量——
// 调用方要的是"这块区域在哪"，不是张量本身。点数过多时按 Douglas-Peucker 抽稀。
struct yolo_seg_t {
    yolo_rec_t box;              // 检测框（含 class_id / label / confidence，坐标已是原图）
    std::vector<point_t> polygon; // 掩码轮廓（原图像素坐标，可能为空：掩码面积过小）
    float mask_area = 0.0f;      // 掩码像素面积（原图尺度），用于调用方过滤噪声
};
using vyolo_seg_t = std::vector<yolo_seg_t>;

// 关键点。坐标保留 float：姿态点的亚像素精度有意义（绘骨架/算角度都用得到），
// 不要提前取整——四边形角点和关键点的取整统一放到 JSON 边界做。
struct yolo_kpt_t {
    float x = 0.0f;          // 原图像素坐标
    float y = 0.0f;
    float confidence = 0.0f; // 模型给出的该点可见度
};
using vyolo_kpt_t = std::vector<yolo_kpt_t>;

struct yolo_pose_t {
    yolo_rec_t box;        // 人体/物体检测框（原图坐标）
    vyolo_kpt_t keypoints; // 按模型输出的关键点顺序
};
using vyolo_pose_t = std::vector<yolo_pose_t>;

// 旋转框（OBB）。xywhr 中 r 为弧度，与 ultralytics OBB 输出一致。
// corners 是四角点（原图坐标，顺序：左上/右上/右下/左下，按旋转前的矩形顺序）。
struct yolo_obb_t {
    int class_id = -1;
    std::wstring label;
    float confidence = 0.0f;
    float cx = 0.0f, cy = 0.0f;
    float w = 0.0f, h = 0.0f;
    float angle = 0.0f;          // 弧度
    point_t left_top, right_bottom; // 旋转框的轴对齐包围盒（原图坐标）
    std::vector<point_t> corners;   // 4 个角点（原图坐标）
};
using vyolo_obb_t = std::vector<yolo_obb_t>;

// YOLO 引擎抽象。当前仅进程内 ONNX 实现（OnnxYoloEngine）；
// HTTP 远程实现（HttpYoloEngine）已隐藏不入构建，见 YoloDetector.h 顶部说明。
// init 语义与 SetYoloEngine(engine, dllName, argv) 对齐；各 detect/classify/segment
// 输入像素 buffer（bpp=1/3/4），conf/iou <=0 时用引擎默认值。
class YoloEngine {
  public:
    virtual ~YoloEngine() = default;
    virtual int init(const std::wstring &engine, const std::wstring &dllName, const vector<string> &argv) = 0;

    // 从内存字节加载模型：加密模型包解密后直接建会话，明文不落盘。
    // 默认返回 -1（引擎不支持内存加载），语义与 init 失败一致。
    virtual int init_from_memory(const std::vector<uint8_t> &model, const vector<string> &argv) {
        (void)model;
        (void)argv;
        return -1;
    }

    virtual int detect(byte *data, int w, int h, int bpp, double conf, double iou, vyolo_rec_t &result) = 0;

    // 以下两个带默认实现：不支持该任务的引擎返回 -1（"明确不支持"），
    // 而不是返回 0（"没有结果"）——两者对调用方语义完全不同。
    virtual int classify(byte *data, int w, int h, int bpp, int top_k, vyolo_cls_t &result) {
        (void)data;
        (void)w;
        (void)h;
        (void)bpp;
        (void)top_k;
        result.clear();
        return -1;
    }
    virtual int segment(byte *data, int w, int h, int bpp, double conf, double iou, vyolo_seg_t &result) {
        (void)data;
        (void)w;
        (void)h;
        (void)bpp;
        (void)conf;
        (void)iou;
        result.clear();
        return -1;
    }
    virtual int pose(byte *data, int w, int h, int bpp, double conf, double iou, vyolo_pose_t &result) {
        (void)data;
        (void)w;
        (void)h;
        (void)bpp;
        (void)conf;
        (void)iou;
        result.clear();
        return -1;
    }
    virtual int obb(byte *data, int w, int h, int bpp, double conf, double iou, vyolo_obb_t &result) {
        (void)data;
        (void)w;
        (void)h;
        (void)bpp;
        (void)conf;
        (void)iou;
        result.clear();
        return -1;
    }

    // 已加载模型的任务类型（未加载 = Unknown）。
    virtual Task task() const {
        return Task::Unknown;
    }

    // 实际生效的推理设备（-1 = CPU，>=0 = DirectML 适配器）。
    virtual int device_id() const {
        return -1;
    }
    virtual bool dml_active() const {
        return false;
    }
    // 当前 onnxruntime.dll 是否带 DirectML EP（与"本次是否真的用了 GPU"不同）。
    virtual bool dml_supported() const {
        return false;
    }
};

} // namespace op::yolo
