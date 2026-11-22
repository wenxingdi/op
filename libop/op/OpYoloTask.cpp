// OpYoloTask.cpp: YOLO 任务维度（批 C 2026-11-22）。
//
// 为什么单独一个 .cpp：
//   OpYolo.cpp 里已经全是"检测 + 过滤 + 截断"的门面，再塞进 classify/segment 会到 900 行；
//   而这三种任务的门面形状（入参、返回语义、JSON 结构）互不相同，分开更清楚。
//
// 与检测版共用的东西：
//   * with_captured_region / with_mem_region —— 屏幕区域与图像句柄两条入口
//   * JSON 用 op::json DOM（不手拼字符串，转义交给 Stringify）
//   * 结果条数由 ret 输出，-1 = 任务不支持/参数非法，0 = 没有结果
#include "OpContext.h"
#include "OpCaptureHelpers.h"
#include "OpResult.h"

#include "../json/JsonValue.h"
#include "base/Utils.h"
#include "image/Image.h"
#include "yolo/YoloDetector.h"

#include <libop.h>

#include <string>
#include <vector>

namespace {

using namespace op::json;

Value JsonNum(double d) {
    Value n;
    n.type = Type::Number;
    n.number = d;
    return n;
}

Value JsonStr(const std::wstring &s) {
    Value v;
    v.type = Type::String;
    v.str = s;
    return v;
}

const wchar_t *TaskText(op::yolo::Task t) {
    return op::yolo::TaskName(t);
}

// 统一失败 JSON：带 code 便于调用方区分"任务不支持(-1)"与"没结果(0)"。
std::wstring FailureJson(const wchar_t *task, long code) {
    Value root;
    root.type = Type::Object;
    root.obj.push_back(std::make_pair(std::wstring(L"ok"), JsonNum(0)));
    root.obj.push_back(std::make_pair(std::wstring(L"code"), JsonNum(static_cast<double>(code))));
    root.obj.push_back(std::make_pair(std::wstring(L"task"), JsonStr(task ? task : L"")));
    Value arr;
    arr.type = Type::Array;
    root.obj.push_back(std::make_pair(std::wstring(L"results"), std::move(arr)));
    return op::json::Stringify(root);
}

std::wstring ClassifyJson(const op::yolo::vyolo_cls_t &items, long code) {
    Value root;
    root.type = Type::Object;
    root.obj.push_back(std::make_pair(std::wstring(L"ok"), JsonNum(code >= 0 ? 1 : 0)));
    root.obj.push_back(std::make_pair(std::wstring(L"code"), JsonNum(static_cast<double>(code < 0 ? code : 0))));
    root.obj.push_back(std::make_pair(std::wstring(L"task"), JsonStr(L"classify")));
    root.obj.push_back(std::make_pair(std::wstring(L"returned"), JsonNum(static_cast<double>(items.size()))));
    Value arr;
    arr.type = Type::Array;
    for (const auto &it : items) {
        Value o;
        o.type = Type::Object;
        o.obj.push_back(std::make_pair(std::wstring(L"class_id"), JsonNum(it.class_id)));
        o.obj.push_back(std::make_pair(std::wstring(L"label"), JsonStr(it.label)));
        o.obj.push_back(std::make_pair(std::wstring(L"score"), JsonNum(it.score)));
        arr.arr.push_back(std::move(o));
    }
    root.obj.push_back(std::make_pair(std::wstring(L"results"), std::move(arr)));
    return op::json::Stringify(root);
}

std::wstring SegmentJson(const op::yolo::vyolo_seg_t &items, long code) {
    Value root;
    root.type = Type::Object;
    root.obj.push_back(std::make_pair(std::wstring(L"ok"), JsonNum(code >= 0 ? 1 : 0)));
    root.obj.push_back(std::make_pair(std::wstring(L"code"), JsonNum(static_cast<double>(code < 0 ? code : 0))));
    root.obj.push_back(std::make_pair(std::wstring(L"task"), JsonStr(L"segment")));
    root.obj.push_back(std::make_pair(std::wstring(L"total"), JsonNum(static_cast<double>(items.size()))));
    root.obj.push_back(std::make_pair(std::wstring(L"returned"), JsonNum(static_cast<double>(items.size()))));
    Value arr;
    arr.type = Type::Array;
    for (const auto &it : items) {
        Value o;
        o.type = Type::Object;
        o.obj.push_back(std::make_pair(std::wstring(L"class_id"), JsonNum(it.box.class_id)));
        o.obj.push_back(std::make_pair(std::wstring(L"label"), JsonStr(it.box.label)));
        Value bbox;
        bbox.type = Type::Array;
        bbox.arr.push_back(JsonNum(it.box.left_top.x));
        bbox.arr.push_back(JsonNum(it.box.left_top.y));
        bbox.arr.push_back(JsonNum(it.box.right_bottom.x));
        bbox.arr.push_back(JsonNum(it.box.right_bottom.y));
        o.obj.push_back(std::make_pair(std::wstring(L"bbox"), std::move(bbox)));
        o.obj.push_back(std::make_pair(std::wstring(L"confidence"), JsonNum(it.box.confidence)));
        o.obj.push_back(std::make_pair(std::wstring(L"mask_area"), JsonNum(it.mask_area)));
        Value poly;
        poly.type = Type::Array;
        for (const auto &p : it.polygon) {
            Value pt;
            pt.type = Type::Array;
            pt.arr.push_back(JsonNum(p.x));
            pt.arr.push_back(JsonNum(p.y));
            poly.arr.push_back(std::move(pt));
        }
        o.obj.push_back(std::make_pair(std::wstring(L"polygon"), std::move(poly)));
        arr.arr.push_back(std::move(o));
    }
    root.obj.push_back(std::make_pair(std::wstring(L"results"), std::move(arr)));
    return op::json::Stringify(root);
}

} // namespace

long op::Op::YoloClassify(long x1, long y1, long x2, long y2, long top_k, std::wstring &retjson, long *ret) {
    internal::set_result(ret, 0L);
    retjson = FailureJson(L"classify", -1);
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
        retjson = ClassifyJson(items, 0);
    });
    if (code < 0)
        return -1; // 任务不支持：显式失败
    internal::set_result(ret, found);
    return found;
}

long op::Op::YoloClassifyFromFile(const wchar_t *file_name, long top_k, std::wstring &retjson, long *ret) {
    internal::set_result(ret, 0L);
    retjson = FailureJson(L"classify", -1);
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
    retjson = ClassifyJson(items, 0);
    internal::set_result(ret, n);
    return n;
}

long op::Op::YoloSegment(long x1, long y1, long x2, long y2, double conf, double iou, std::wstring &retjson, long *ret) {
    internal::set_result(ret, 0L);
    retjson = FailureJson(L"segment", -1);
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
        retjson = SegmentJson(items, 0);
    });
    if (unsupported)
        return -1;
    internal::set_result(ret, found);
    return found;
}

long op::Op::YoloSegmentFromFile(const wchar_t *file_name, double conf, double iou, std::wstring &retjson, long *ret) {
    internal::set_result(ret, 0L);
    retjson = FailureJson(L"segment", -1);
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
    retjson = SegmentJson(items, 0);
    internal::set_result(ret, n);
    return n;
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
        retjson = op::json::Stringify(root);
    });
}
