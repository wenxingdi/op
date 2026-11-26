// OpYoloTask.cpp: YOLO 任务维度（批 C 2026-11-22；Ex2 过滤变体 2026-11-26）。
//
// 为什么单独一个 .cpp：
//   OpYolo.cpp 里已经全是"检测 + 过滤 + 截断"的门面，再塞进 classify/segment 会到 900 行；
//   而这几种任务的门面形状（入参、返回语义、JSON 结构）互不相同，分开更清楚。
//
// 与检测版共用的东西：
//   * with_captured_region —— 屏幕区域入口
//   * 过滤/截断/JSON 构建 —— 全部在 YoloDetectCore.h（header-only，可单测）
//   * 结果条数由 ret 输出，-1 = 任务不支持/参数非法，0 = 没有结果
//
// Ex2 变体（2026-11-26，对称 Detect 族）：
//   * YoloClassifyEx2 / YoloClassifyFromFileEx2
//   * YoloSegmentEx2  / YoloSegmentFromFileEx2
//   * YoloPoseEx2     / YoloPoseFromFileEx2
//   * YoloObbEx2      / YoloObbFromFileEx2
//   json_options 语义与 Detect Ex2 完全一致：{"class_ids":[],"labels":[],"max_results":N}，
//   class_ids 与 labels 取并集；空串 = 不过滤；非法 options 返回 -1（显式失败）。
//   JSON 在旧结构上增加 index/total（total 恒为过滤前条数，returned 为过滤后）。
#include "OpContext.h"
#include "OpCaptureHelpers.h"
#include "OpResult.h"
#include "YoloDetectCore.h"

#include "../json/JsonValue.h"
#include "base/Utils.h"
#include "image/Image.h"
#include "yolo/YoloDetector.h"

#include <libop.h>

#include <string>
#include <vector>

namespace {

using namespace op::json;

using op::yolo::core::BuildClassifyJson;
using op::yolo::core::BuildObbJson;
using op::yolo::core::BuildPoseJson;
using op::yolo::core::BuildSegmentJson;
using op::yolo::core::ClassifyClassLabel;
using op::yolo::core::ClassifyScore;
using op::yolo::core::CountKept;
using op::yolo::core::ObBClassLabel;
using op::yolo::core::ObBScore;
using op::yolo::core::Options;
using op::yolo::core::ParseOptions;
using op::yolo::core::PoseClassLabel;
using op::yolo::core::PoseScore;
using op::yolo::core::SegClassLabel;
using op::yolo::core::SegScore;
using op::yolo::core::TaskFailureJson;

// YoloGetModelInfo 仍在用旧 helper 名：薄封装到核心 YNum/YStr。
Value JsonNum(double d) { return op::yolo::core::YNum(d); }
Value JsonStr(const std::wstring &s) { return op::yolo::core::YStr(s); }

const wchar_t *TaskText(op::yolo::Task t) {
    return op::yolo::TaskName(t);
}

} // namespace

long op::Op::YoloClassify(long x1, long y1, long x2, long y2, long top_k, std::wstring &retjson, long *ret) {
    internal::set_result(ret, 0L);
    retjson = TaskFailureJson(L"classify", -1);
    long found = 0;
    long code = 0;
    internal::with_captured_region(m_context.get(), x1, y1, x2, y2, [&]() {
        op::yolo::vyolo_cls_t items;
        const int n = op::yolo::YoloDetector::getInstance()->classify(
            m_context->image_proc._src.pdata, m_context->image_proc._src.width, m_context->image_proc._src.height, 4,
            static_cast<int>(top_k), items);
        if (n < 0) {
            code = -1;
            return;
        }
        found = n;
        retjson = BuildClassifyJson(items, Options{}, /*extra_fields=*/false, 0);
    });
    if (code < 0)
        return -1; // 任务不支持：显式失败
    internal::set_result(ret, found);
    return found;
}

long op::Op::YoloClassifyFromFile(const wchar_t *file_name, long top_k, std::wstring &retjson, long *ret) {
    internal::set_result(ret, 0L);
    retjson = TaskFailureJson(L"classify", -1);
    std::wstring fullpath;
    if (!Path2GlobalPath(file_name ? file_name : L"", m_context->curr_path, fullpath))
        return 0;
    Image img;
    if (!img.read(fullpath.data()))
        return 0;
    op::yolo::vyolo_cls_t items;
    const int n = op::yolo::YoloDetector::getInstance()->classify(img.pdata, img.width, img.height, 4,
                                                                  static_cast<int>(top_k), items);
    if (n < 0)
        return -1;
    retjson = BuildClassifyJson(items, Options{}, /*extra_fields=*/false, 0);
    internal::set_result(ret, n);
    return n;
}

// Ex2：分类 + class/label 过滤 + max_results（按 score 降序截断）。
long op::Op::YoloClassifyEx2(long x1, long y1, long x2, long y2, long top_k, const wchar_t *json_options,
                             std::wstring &retjson, long *ret) {
    internal::set_result(ret, 0L);
    retjson = TaskFailureJson(L"classify", -1);

    Options opt;
    if (!ParseOptions(json_options ? json_options : L"", opt))
        return -1; // 参数非法：显式失败，不当作"没检测到"

    long found = 0;
    bool unsupported = false;
    internal::with_captured_region(m_context.get(), x1, y1, x2, y2, [&]() {
        op::yolo::vyolo_cls_t items;
        const int n = op::yolo::YoloDetector::getInstance()->classify(
            m_context->image_proc._src.pdata, m_context->image_proc._src.width, m_context->image_proc._src.height, 4,
            static_cast<int>(top_k), items);
        if (n < 0) {
            unsupported = true;
            return;
        }
        retjson = BuildClassifyJson(items, opt, /*extra_fields=*/true, 0);
        found = CountKept(items, opt, ClassifyClassLabel, ClassifyScore);
    });
    if (unsupported)
        return -1;
    internal::set_result(ret, found);
    return found;
}

long op::Op::YoloClassifyFromFileEx2(const wchar_t *file_name, long top_k, const wchar_t *json_options,
                                     std::wstring &retjson, long *ret) {
    internal::set_result(ret, 0L);
    retjson = TaskFailureJson(L"classify", -1);

    Options opt;
    if (!ParseOptions(json_options ? json_options : L"", opt))
        return -1;

    std::wstring fullpath;
    if (!Path2GlobalPath(file_name ? file_name : L"", m_context->curr_path, fullpath))
        return 0;
    Image img;
    if (!img.read(fullpath.data()))
        return 0;
    op::yolo::vyolo_cls_t items;
    const int n = op::yolo::YoloDetector::getInstance()->classify(img.pdata, img.width, img.height, 4,
                                                                  static_cast<int>(top_k), items);
    if (n < 0)
        return -1;
    retjson = BuildClassifyJson(items, opt, /*extra_fields=*/true, 0);
    const long found = CountKept(items, opt, ClassifyClassLabel, ClassifyScore);
    internal::set_result(ret, found);
    return found;
}

long op::Op::YoloSegment(long x1, long y1, long x2, long y2, double conf, double iou, std::wstring &retjson, long *ret) {
    internal::set_result(ret, 0L);
    retjson = TaskFailureJson(L"segment", -1);
    long found = 0;
    bool unsupported = false;
    internal::with_captured_region(m_context.get(), x1, y1, x2, y2, [&]() {
        op::yolo::vyolo_seg_t items;
        const int n = op::yolo::YoloDetector::getInstance()->segment(
            m_context->image_proc._src.pdata, m_context->image_proc._src.width, m_context->image_proc._src.height, 4,
            conf, iou, items);
        if (n < 0) {
            unsupported = true;
            return;
        }
        // 坐标口径与检测一致：截图偏移 + 客户区偏移
        const int off_x = static_cast<int>(x1 + m_context->image_proc._dx);
        const int off_y = static_cast<int>(y1 + m_context->image_proc._dy);
        for (auto &it : items) {
            it.box.left_top.x += off_x;
            it.box.left_top.y += off_y;
            it.box.right_bottom.x += off_x;
            it.box.right_bottom.y += off_y;
            for (auto &p : it.polygon) {
                p.x += off_x;
                p.y += off_y;
            }
        }
        found = n;
        retjson = BuildSegmentJson(items, Options{}, /*extra_fields=*/false, 0);
    });
    if (unsupported)
        return -1;
    internal::set_result(ret, found);
    return found;
}

long op::Op::YoloSegmentFromFile(const wchar_t *file_name, double conf, double iou, std::wstring &retjson, long *ret) {
    internal::set_result(ret, 0L);
    retjson = TaskFailureJson(L"segment", -1);
    std::wstring fullpath;
    if (!Path2GlobalPath(file_name ? file_name : L"", m_context->curr_path, fullpath))
        return 0;
    Image img;
    if (!img.read(fullpath.data()))
        return 0;
    op::yolo::vyolo_seg_t items;
    const int n = op::yolo::YoloDetector::getInstance()->segment(img.pdata, img.width, img.height, 4, conf, iou, items);
    if (n < 0)
        return -1;
    retjson = BuildSegmentJson(items, Options{}, /*extra_fields=*/false, 0);
    internal::set_result(ret, n);
    return n;
}

// Ex2：分割 + class/label 过滤 + max_results（按框 confidence 降序截断）。
long op::Op::YoloSegmentEx2(long x1, long y1, long x2, long y2, double conf, double iou, const wchar_t *json_options,
                            std::wstring &retjson, long *ret) {
    internal::set_result(ret, 0L);
    retjson = TaskFailureJson(L"segment", -1);

    Options opt;
    if (!ParseOptions(json_options ? json_options : L"", opt))
        return -1;

    long found = 0;
    bool unsupported = false;
    internal::with_captured_region(m_context.get(), x1, y1, x2, y2, [&]() {
        op::yolo::vyolo_seg_t items;
        const int n = op::yolo::YoloDetector::getInstance()->segment(
            m_context->image_proc._src.pdata, m_context->image_proc._src.width, m_context->image_proc._src.height, 4,
            conf, iou, items);
        if (n < 0) {
            unsupported = true;
            return;
        }
        const int off_x = static_cast<int>(x1 + m_context->image_proc._dx);
        const int off_y = static_cast<int>(y1 + m_context->image_proc._dy);
        for (auto &it : items) {
            it.box.left_top.x += off_x;
            it.box.left_top.y += off_y;
            it.box.right_bottom.x += off_x;
            it.box.right_bottom.y += off_y;
            for (auto &p : it.polygon) {
                p.x += off_x;
                p.y += off_y;
            }
        }
        retjson = BuildSegmentJson(items, opt, /*extra_fields=*/true, 0);
        found = CountKept(items, opt, SegClassLabel, SegScore);
    });
    if (unsupported)
        return -1;
    internal::set_result(ret, found);
    return found;
}

long op::Op::YoloSegmentFromFileEx2(const wchar_t *file_name, double conf, double iou, const wchar_t *json_options,
                                    std::wstring &retjson, long *ret) {
    internal::set_result(ret, 0L);
    retjson = TaskFailureJson(L"segment", -1);

    Options opt;
    if (!ParseOptions(json_options ? json_options : L"", opt))
        return -1;

    std::wstring fullpath;
    if (!Path2GlobalPath(file_name ? file_name : L"", m_context->curr_path, fullpath))
        return 0;
    Image img;
    if (!img.read(fullpath.data()))
        return 0;
    op::yolo::vyolo_seg_t items;
    const int n = op::yolo::YoloDetector::getInstance()->segment(img.pdata, img.width, img.height, 4, conf, iou, items);
    if (n < 0)
        return -1;
    retjson = BuildSegmentJson(items, opt, /*extra_fields=*/true, 0);
    const long found = CountKept(items, opt, SegClassLabel, SegScore);
    internal::set_result(ret, found);
    return found;
}

long op::Op::YoloPose(long x1, long y1, long x2, long y2, double conf, double iou, std::wstring &retjson, long *ret) {
    internal::set_result(ret, 0L);
    retjson = TaskFailureJson(L"pose", -1);
    long found = 0;
    bool unsupported = false;
    internal::with_captured_region(m_context.get(), x1, y1, x2, y2, [&]() {
        op::yolo::vyolo_pose_t items;
        const int n = op::yolo::YoloDetector::getInstance()->pose(
            m_context->image_proc._src.pdata, m_context->image_proc._src.width, m_context->image_proc._src.height, 4,
            conf, iou, items);
        if (n < 0) {
            unsupported = true;
            return;
        }
        const int off_x = static_cast<int>(x1 + m_context->image_proc._dx);
        const int off_y = static_cast<int>(y1 + m_context->image_proc._dy);
        for (auto &it : items) {
            it.box.left_top.x += off_x;
            it.box.left_top.y += off_y;
            it.box.right_bottom.x += off_x;
            it.box.right_bottom.y += off_y;
            for (auto &kp : it.keypoints) {
                kp.x += off_x;
                kp.y += off_y;
            }
        }
        found = n;
        retjson = BuildPoseJson(items, Options{}, /*extra_fields=*/false, 0);
    });
    if (unsupported)
        return -1;
    internal::set_result(ret, found);
    return found;
}

long op::Op::YoloPoseFromFile(const wchar_t *file_name, double conf, double iou, std::wstring &retjson, long *ret) {
    internal::set_result(ret, 0L);
    retjson = TaskFailureJson(L"pose", -1);
    std::wstring fullpath;
    if (!Path2GlobalPath(file_name ? file_name : L"", m_context->curr_path, fullpath))
        return 0;
    Image img;
    if (!img.read(fullpath.data()))
        return 0;
    op::yolo::vyolo_pose_t items;
    const int n = op::yolo::YoloDetector::getInstance()->pose(img.pdata, img.width, img.height, 4, conf, iou, items);
    if (n < 0)
        return -1;
    retjson = BuildPoseJson(items, Options{}, /*extra_fields=*/false, 0);
    internal::set_result(ret, n);
    return n;
}

// Ex2：姿态 + class/label 过滤 + max_results（按框 confidence 降序截断）。
long op::Op::YoloPoseEx2(long x1, long y1, long x2, long y2, double conf, double iou, const wchar_t *json_options,
                         std::wstring &retjson, long *ret) {
    internal::set_result(ret, 0L);
    retjson = TaskFailureJson(L"pose", -1);

    Options opt;
    if (!ParseOptions(json_options ? json_options : L"", opt))
        return -1;

    long found = 0;
    bool unsupported = false;
    internal::with_captured_region(m_context.get(), x1, y1, x2, y2, [&]() {
        op::yolo::vyolo_pose_t items;
        const int n = op::yolo::YoloDetector::getInstance()->pose(
            m_context->image_proc._src.pdata, m_context->image_proc._src.width, m_context->image_proc._src.height, 4,
            conf, iou, items);
        if (n < 0) {
            unsupported = true;
            return;
        }
        const int off_x = static_cast<int>(x1 + m_context->image_proc._dx);
        const int off_y = static_cast<int>(y1 + m_context->image_proc._dy);
        for (auto &it : items) {
            it.box.left_top.x += off_x;
            it.box.left_top.y += off_y;
            it.box.right_bottom.x += off_x;
            it.box.right_bottom.y += off_y;
            for (auto &kp : it.keypoints) {
                kp.x += off_x;
                kp.y += off_y;
            }
        }
        retjson = BuildPoseJson(items, opt, /*extra_fields=*/true, 0);
        found = CountKept(items, opt, PoseClassLabel, PoseScore);
    });
    if (unsupported)
        return -1;
    internal::set_result(ret, found);
    return found;
}

long op::Op::YoloPoseFromFileEx2(const wchar_t *file_name, double conf, double iou, const wchar_t *json_options,
                                 std::wstring &retjson, long *ret) {
    internal::set_result(ret, 0L);
    retjson = TaskFailureJson(L"pose", -1);

    Options opt;
    if (!ParseOptions(json_options ? json_options : L"", opt))
        return -1;

    std::wstring fullpath;
    if (!Path2GlobalPath(file_name ? file_name : L"", m_context->curr_path, fullpath))
        return 0;
    Image img;
    if (!img.read(fullpath.data()))
        return 0;
    op::yolo::vyolo_pose_t items;
    const int n = op::yolo::YoloDetector::getInstance()->pose(img.pdata, img.width, img.height, 4, conf, iou, items);
    if (n < 0)
        return -1;
    retjson = BuildPoseJson(items, opt, /*extra_fields=*/true, 0);
    const long found = CountKept(items, opt, PoseClassLabel, PoseScore);
    internal::set_result(ret, found);
    return found;
}

long op::Op::YoloObb(long x1, long y1, long x2, long y2, double conf, double iou, std::wstring &retjson, long *ret) {
    internal::set_result(ret, 0L);
    retjson = TaskFailureJson(L"obb", -1);
    long found = 0;
    bool unsupported = false;
    internal::with_captured_region(m_context.get(), x1, y1, x2, y2, [&]() {
        op::yolo::vyolo_obb_t items;
        const int n = op::yolo::YoloDetector::getInstance()->obb(
            m_context->image_proc._src.pdata, m_context->image_proc._src.width, m_context->image_proc._src.height, 4,
            conf, iou, items);
        if (n < 0) {
            unsupported = true;
            return;
        }
        const int off_x = static_cast<int>(x1 + m_context->image_proc._dx);
        const int off_y = static_cast<int>(y1 + m_context->image_proc._dy);
        for (auto &it : items) {
            it.cx += off_x;
            it.cy += off_y;
            it.left_top.x += off_x;
            it.left_top.y += off_y;
            it.right_bottom.x += off_x;
            it.right_bottom.y += off_y;
            for (auto &p : it.corners) {
                p.x += off_x;
                p.y += off_y;
            }
        }
        found = n;
        retjson = BuildObbJson(items, Options{}, /*extra_fields=*/false, 0);
    });
    if (unsupported)
        return -1;
    internal::set_result(ret, found);
    return found;
}

long op::Op::YoloObbFromFile(const wchar_t *file_name, double conf, double iou, std::wstring &retjson, long *ret) {
    internal::set_result(ret, 0L);
    retjson = TaskFailureJson(L"obb", -1);
    std::wstring fullpath;
    if (!Path2GlobalPath(file_name ? file_name : L"", m_context->curr_path, fullpath))
        return 0;
    Image img;
    if (!img.read(fullpath.data()))
        return 0;
    op::yolo::vyolo_obb_t items;
    const int n = op::yolo::YoloDetector::getInstance()->obb(img.pdata, img.width, img.height, 4, conf, iou, items);
    if (n < 0)
        return -1;
    retjson = BuildObbJson(items, Options{}, /*extra_fields=*/false, 0);
    internal::set_result(ret, n);
    return n;
}

// Ex2：旋转框 + class/label 过滤 + max_results（按 confidence 降序截断）。
long op::Op::YoloObbEx2(long x1, long y1, long x2, long y2, double conf, double iou, const wchar_t *json_options,
                        std::wstring &retjson, long *ret) {
    internal::set_result(ret, 0L);
    retjson = TaskFailureJson(L"obb", -1);

    Options opt;
    if (!ParseOptions(json_options ? json_options : L"", opt))
        return -1;

    long found = 0;
    bool unsupported = false;
    internal::with_captured_region(m_context.get(), x1, y1, x2, y2, [&]() {
        op::yolo::vyolo_obb_t items;
        const int n = op::yolo::YoloDetector::getInstance()->obb(
            m_context->image_proc._src.pdata, m_context->image_proc._src.width, m_context->image_proc._src.height, 4,
            conf, iou, items);
        if (n < 0) {
            unsupported = true;
            return;
        }
        const int off_x = static_cast<int>(x1 + m_context->image_proc._dx);
        const int off_y = static_cast<int>(y1 + m_context->image_proc._dy);
        for (auto &it : items) {
            it.cx += off_x;
            it.cy += off_y;
            it.left_top.x += off_x;
            it.left_top.y += off_y;
            it.right_bottom.x += off_x;
            it.right_bottom.y += off_y;
            for (auto &p : it.corners) {
                p.x += off_x;
                p.y += off_y;
            }
        }
        retjson = BuildObbJson(items, opt, /*extra_fields=*/true, 0);
        found = CountKept(items, opt, ObBClassLabel, ObBScore);
    });
    if (unsupported)
        return -1;
    internal::set_result(ret, found);
    return found;
}

long op::Op::YoloObbFromFileEx2(const wchar_t *file_name, double conf, double iou, const wchar_t *json_options,
                                std::wstring &retjson, long *ret) {
    internal::set_result(ret, 0L);
    retjson = TaskFailureJson(L"obb", -1);

    Options opt;
    if (!ParseOptions(json_options ? json_options : L"", opt))
        return -1;

    std::wstring fullpath;
    if (!Path2GlobalPath(file_name ? file_name : L"", m_context->curr_path, fullpath))
        return 0;
    Image img;
    if (!img.read(fullpath.data()))
        return 0;
    op::yolo::vyolo_obb_t items;
    const int n = op::yolo::YoloDetector::getInstance()->obb(img.pdata, img.width, img.height, 4, conf, iou, items);
    if (n < 0)
        return -1;
    retjson = BuildObbJson(items, opt, /*extra_fields=*/true, 0);
    const long found = CountKept(items, opt, ObBClassLabel, ObBScore);
    internal::set_result(ret, found);
    return found;
}

void op::Op::YoloGetModelInfo(std::wstring &retjson) {
    retjson.clear();
    internal::guard_exceptions("YoloGetModelInfo", [&]() {
        const op::yolo::Task task = op::yolo::YoloDetector::getInstance()->loaded_task();
        Value root;
        root.type = Type::Object;
        root.obj.push_back(std::make_pair(std::wstring(L"ok"), JsonNum(task == op::yolo::Task::Unknown ? 0 : 1)));
        root.obj.push_back(std::make_pair(std::wstring(L"task"), JsonStr(TaskText(task))));
        root.obj.push_back(std::make_pair(std::wstring(L"task_code"), JsonNum(static_cast<double>(task))));
        root.obj.push_back(std::make_pair(
            std::wstring(L"input_width"), JsonNum(op::yolo::YoloDetector::getInstance()->model_input_width())));
        root.obj.push_back(std::make_pair(
            std::wstring(L"input_height"), JsonNum(op::yolo::YoloDetector::getInstance()->model_input_height())));
        root.obj.push_back(std::make_pair(
            std::wstring(L"device"), JsonNum(op::yolo::YoloDetector::getInstance()->device_id())));
        root.obj.push_back(std::make_pair(
            std::wstring(L"dml"), JsonNum(op::yolo::YoloDetector::getInstance()->dml_active() ? 1 : 0)));
        retjson = op::json::Stringify(root);
    });
}
