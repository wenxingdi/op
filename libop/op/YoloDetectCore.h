#pragma once
// YOLO 检测核心（2026-11-21 抽出，供 OpYolo.cpp 与 OpMemImage.cpp 共用）
//
// 背景：二期要加"在图像句柄上做检测"，逻辑与屏幕版**完全相同**（同一套
// 检测调用 + 过滤 + JSON 构建），差别只有坐标偏移。
// 原先这些函数在 OpYolo.cpp 的匿名命名空间里，跨 TU 不可见；
// 抽出到本头文件后两个入口共用一份实现 —— "行为一致"是结构保证。
//
// 约定：本头文件只做 header-only 的工具函数，不引入新的编译单元。

#include "yolo/YoloDetector.h"

#include "../json/JsonValue.h"

#include <algorithm>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace op::yolo::core {

// 本命名空间内的 JSON 类型速记（JsonValue.h 里定义在 op::json）。
using op::json::Stringify;
using op::json::Type;
using op::json::Value;

inline constexpr const wchar_t *kFailureJson = L"{\"ok\":0,\"code\":-1,\"results\":[]}";

// 过滤/截断选项（Ex2 用）。空 = 不过滤。
struct Options {
    std::vector<long> class_ids;
    std::vector<std::wstring> labels;
    long max_results = 0; // 0 = 不限
    bool has_class_filter = false;
    bool has_label_filter = false;
};

// 解析 json_options。空串 => 全部取默认（返回 true）。
// 未知字段 / 类型错误 / JSON 语法错误一律返回 false（显式失败，不静默降级）。
inline bool ParseOptions(const std::wstring &json_text, Options &opt) {
    opt = Options{};
    if (json_text.empty())
        return true;

    op::json::Value root;
    if (!op::json::Parse(json_text, root))
        return false;
    if (root.type == op::json::Type::Null)
        return true; // 空 JSON 等价于空 options
    if (root.type != op::json::Type::Object)
        return false;

    for (const auto &kv : root.obj) {
        const std::wstring &k = kv.first;
        const op::json::Value &v = kv.second;
        if (k == L"class_ids") {
            if (v.type != op::json::Type::Array)
                return false;
            for (const auto &item : v.arr) {
                if (item.type != op::json::Type::Number)
                    return false;
                opt.class_ids.push_back(static_cast<long>(item.number));
            }
            opt.has_class_filter = !opt.class_ids.empty();
        } else if (k == L"labels") {
            if (v.type != op::json::Type::Array)
                return false;
            for (const auto &item : v.arr) {
                if (item.type != op::json::Type::String)
                    return false;
                opt.labels.push_back(item.str);
            }
            opt.has_label_filter = !opt.labels.empty();
        } else if (k == L"max_results") {
            if (v.type != op::json::Type::Number)
                return false;
            opt.max_results = static_cast<long>(v.number);
            if (opt.max_results < 0)
                return false;
        } else {
            return false; // 未知字段：拼写错误必须报错
        }
    }
    return true;
}

// 是否保留该检测结果。
// class_ids 与 labels 同时给时取**并集**：满足任一即保留。
// 理由：调用方可能只知道 id 或只知道名字，取交集会让"我以为能命中"变成空结果。
inline bool KeepByClassLabel(long class_id, const std::wstring &label, const Options &opt) {
    if (!opt.has_class_filter && !opt.has_label_filter)
        return true;
    if (opt.has_class_filter) {
        for (long id : opt.class_ids) {
            if (id == class_id)
                return true;
        }
    }
    if (opt.has_label_filter) {
        for (const auto &name : opt.labels) {
            if (name == label)
                return true;
        }
    }
    return false;
}

// Detect 族入口：委托通用判定，行为与 2026-11-21 版逐字节一致。
inline bool KeepDetection(const vyolo_rec_t &items, size_t idx, const Options &opt) {
    return KeepByClassLabel(items[idx].class_id, items[idx].label, opt);
}

// 选出生效下标（2026-11-26，五族共用）：
//   1) 按 class/label 过滤，保持原顺序（index 字段可对照原数组）；
//   2) max_results>0 时按 score 降序截断到前 N（"最可信的 N 个"）。
// class_label_of(item) -> pair<long, const wstring&>；score_of(item) -> double。
template <typename Items, typename ClassLabelOf, typename ScoreOf>
inline std::vector<size_t> SelectKept(const Items &items, const Options &opt, ClassLabelOf class_label_of,
                                      ScoreOf score_of) {
    std::vector<size_t> kept;
    kept.reserve(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        const auto cl = class_label_of(items[i]);
        if (KeepByClassLabel(cl.first, cl.second, opt))
            kept.push_back(i);
    }
    if (opt.max_results > 0 && static_cast<long>(kept.size()) > opt.max_results) {
        std::stable_sort(kept.begin(), kept.end(),
                         [&items, &score_of](size_t a, size_t b) { return score_of(items[a]) > score_of(items[b]); });
        kept.resize(static_cast<size_t>(opt.max_results));
    }
    return kept;
}

// 过滤后条数（Ex2 的返回值/ret 口径，与 JSON 的 returned 一致）。
template <typename Items, typename ClassLabelOf, typename ScoreOf>
inline long CountKept(const Items &items, const Options &opt, ClassLabelOf class_label_of, ScoreOf score_of) {
    return static_cast<long>(SelectKept(items, opt, class_label_of, score_of).size());
}

// 构建 YOLO 结果 JSON（用 op::json DOM，转义由 Stringify 统一负责）。
//
// extra_fields=true 时额外输出 index/cx/cy/w/h 与 total/returned（Ex2 用）；
// 旧接口保持原结构（只有 class_id/label/bbox/confidence），避免破坏既有调用方。
inline void BuildJson(const vyolo_rec_t &items, const Options &opt, bool extra_fields, std::wstring &retjson) {
    using namespace op::json;

    // 先选出生效下标（保持原顺序，便于用 index 对照）
    std::vector<size_t> kept;
    kept.reserve(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        if (KeepDetection(items, i, opt))
            kept.push_back(i);
    }
    const long total = static_cast<long>(items.size());

    // max_results：按 confidence 降序截断。
    // 注意"降序截断"而不是"按原序取前 N"——调用方要的是"最可信的 N 个"。
    if (opt.max_results > 0 && static_cast<long>(kept.size()) > opt.max_results) {
        std::stable_sort(kept.begin(), kept.end(),
                         [&items](size_t a, size_t b) { return items[a].confidence > items[b].confidence; });
        kept.resize(static_cast<size_t>(opt.max_results));
    }

    auto num = [](double d) {
        Value n;
        n.type = Type::Number;
        n.number = d;
        return n;
    };
    auto strv = [](const std::wstring &s) {
        Value v;
        v.type = Type::String;
        v.str = s;
        return v;
    };
    auto key = [](const wchar_t *k, Value v) { return std::make_pair(std::wstring(k), std::move(v)); };

    Value root;
    root.type = Type::Object;
    root.obj.push_back(key(L"ok", num(1)));
    root.obj.push_back(key(L"code", num(0)));
    if (extra_fields) {
        root.obj.push_back(key(L"total", num(static_cast<double>(total))));
        root.obj.push_back(key(L"returned", num(static_cast<double>(kept.size()))));
    }

    Value arr;
    arr.type = Type::Array;
    for (size_t i = 0; i < kept.size(); ++i) {
        const auto &it = items[kept[i]];
        Value item;
        item.type = Type::Object;
        if (extra_fields)
            item.obj.push_back(key(L"index", num(static_cast<double>(kept[i]))));
        item.obj.push_back(key(L"class_id", num(static_cast<double>(it.class_id))));
        item.obj.push_back(key(L"label", strv(it.label)));

        Value bbox;
        bbox.type = Type::Array;
        bbox.arr.push_back(num(it.left_top.x));
        bbox.arr.push_back(num(it.left_top.y));
        bbox.arr.push_back(num(it.right_bottom.x));
        bbox.arr.push_back(num(it.right_bottom.y));
        item.obj.push_back(key(L"bbox", std::move(bbox)));

        item.obj.push_back(key(L"confidence", num(it.confidence)));

        if (extra_fields) {
            // cx/cy/w/h：游戏里"点哪里"最常用，省得调用方自己算
            const long w = it.right_bottom.x - it.left_top.x;
            const long h = it.right_bottom.y - it.left_top.y;
            item.obj.push_back(key(L"cx", num(it.left_top.x + w / 2.0)));
            item.obj.push_back(key(L"cy", num(it.left_top.y + h / 2.0)));
            item.obj.push_back(key(L"w", num(static_cast<double>(w))));
            item.obj.push_back(key(L"h", num(static_cast<double>(h))));
        }
        arr.arr.push_back(std::move(item));
    }
    root.obj.push_back(key(L"results", std::move(arr)));
    retjson = op::json::Stringify(root);
}

// 把检测框从图像本地坐标平移成输出坐标。
// off_x/off_y 必须是"截图偏移 + 客户区偏移"，与 OCR/找图保持同一口径。
inline void OffsetDetections(vyolo_rec_t &res, long off_x, long off_y) {
    for (auto &it : res) {
        it.left_top.x += static_cast<int>(off_x);
        it.left_top.y += static_cast<int>(off_y);
        it.right_bottom.x += static_cast<int>(off_x);
        it.right_bottom.y += static_cast<int>(off_y);
    }
}

// ---------------------------------------------------------------------------
// 任务族（classify/segment/pose/obb）的过滤 + JSON 构建（2026-11-26 从
// OpYoloTask.cpp 匿名命名空间抽出）。Ex2 变体与旧入口共用同一份序列化：
// extra_fields=false + 空 Options 时输出与 2026-11-22 版逐字节一致；
// Ex2 传 extra_fields=true，新增 index（原始下标），total=过滤前/returned=过滤后。
// 分段/姿态的 Ex2 条目顺带输出 cx/cy/w/h（对齐 Detect Ex2 的"点哪里"字段）；
// OBB 本身已有 center/size/angle，不重复。

inline Value YNum(double d) {
    Value n;
    n.type = Type::Number;
    n.number = d;
    return n;
}

inline Value YStr(const std::wstring &s) {
    Value v;
    v.type = Type::String;
    v.str = s;
    return v;
}

inline std::pair<std::wstring, Value> YKey(const wchar_t *k, Value v) {
    return std::make_pair(std::wstring(k), std::move(v));
}

// 统一失败 JSON：带 code 便于调用方区分"任务不支持(-1)"与"没结果(0)"。
inline std::wstring TaskFailureJson(const wchar_t *task, long code) {
    Value root;
    root.type = Type::Object;
    root.obj.push_back(YKey(L"ok", YNum(0)));
    root.obj.push_back(YKey(L"code", YNum(static_cast<double>(code))));
    root.obj.push_back(YKey(L"task", YStr(task ? task : L"")));
    Value arr;
    arr.type = Type::Array;
    root.obj.push_back(YKey(L"results", std::move(arr)));
    return Stringify(root);
}

inline Value YBBox(const point_t &lt, const point_t &rb) {
    Value bbox;
    bbox.type = Type::Array;
    bbox.arr.push_back(YNum(lt.x));
    bbox.arr.push_back(YNum(lt.y));
    bbox.arr.push_back(YNum(rb.x));
    bbox.arr.push_back(YNum(rb.y));
    return bbox;
}

// 分类条目投影：扁平 class_id/label，score 即排序键。
inline auto ClassifyClassLabel(const yolo_cls_t &it) { return std::make_pair(static_cast<long>(it.class_id), std::cref(it.label)); }
inline double ClassifyScore(const yolo_cls_t &it) { return static_cast<double>(it.score); }

inline std::wstring BuildClassifyJson(const vyolo_cls_t &items, const Options &opt, bool extra_fields, long code) {
    const auto kept = SelectKept(items, opt, ClassifyClassLabel, ClassifyScore);
    Value root;
    root.type = Type::Object;
    root.obj.push_back(YKey(L"ok", YNum(code >= 0 ? 1 : 0)));
    root.obj.push_back(YKey(L"code", YNum(static_cast<double>(code < 0 ? code : 0))));
    root.obj.push_back(YKey(L"task", YStr(L"classify")));
    if (extra_fields)
        root.obj.push_back(YKey(L"total", YNum(static_cast<double>(items.size()))));
    root.obj.push_back(YKey(L"returned", YNum(static_cast<double>(kept.size()))));
    Value arr;
    arr.type = Type::Array;
    for (const size_t idx : kept) {
        const auto &it = items[idx];
        Value o;
        o.type = Type::Object;
        if (extra_fields)
            o.obj.push_back(YKey(L"index", YNum(static_cast<double>(idx))));
        o.obj.push_back(YKey(L"class_id", YNum(it.class_id)));
        o.obj.push_back(YKey(L"label", YStr(it.label)));
        o.obj.push_back(YKey(L"score", YNum(it.score)));
        arr.arr.push_back(std::move(o));
    }
    root.obj.push_back(YKey(L"results", std::move(arr)));
    return Stringify(root);
}

// 分割/姿态条目投影：class/label/confidence 都在内嵌 box 上。
inline auto BoxClassLabel(const yolo_rec_t &box) { return std::make_pair(static_cast<long>(box.class_id), std::cref(box.label)); }
inline double BoxScore(const yolo_rec_t &box) { return static_cast<double>(box.confidence); }
inline auto SegClassLabel(const yolo_seg_t &it) { return BoxClassLabel(it.box); }
inline double SegScore(const yolo_seg_t &it) { return BoxScore(it.box); }
inline auto PoseClassLabel(const yolo_pose_t &it) { return BoxClassLabel(it.box); }
inline double PoseScore(const yolo_pose_t &it) { return BoxScore(it.box); }

inline std::wstring BuildSegmentJson(const vyolo_seg_t &items, const Options &opt, bool extra_fields, long code) {
    const auto kept = SelectKept(items, opt, SegClassLabel, SegScore);
    Value root;
    root.type = Type::Object;
    root.obj.push_back(YKey(L"ok", YNum(code >= 0 ? 1 : 0)));
    root.obj.push_back(YKey(L"code", YNum(static_cast<double>(code < 0 ? code : 0))));
    root.obj.push_back(YKey(L"task", YStr(L"segment")));
    root.obj.push_back(YKey(L"total", YNum(static_cast<double>(items.size()))));
    root.obj.push_back(YKey(L"returned", YNum(static_cast<double>(kept.size()))));
    Value arr;
    arr.type = Type::Array;
    for (const size_t idx : kept) {
        const auto &it = items[idx];
        Value o;
        o.type = Type::Object;
        if (extra_fields) {
            o.obj.push_back(YKey(L"index", YNum(static_cast<double>(idx))));
            const long w = it.box.right_bottom.x - it.box.left_top.x;
            const long h = it.box.right_bottom.y - it.box.left_top.y;
            o.obj.push_back(YKey(L"cx", YNum(it.box.left_top.x + w / 2.0)));
            o.obj.push_back(YKey(L"cy", YNum(it.box.left_top.y + h / 2.0)));
            o.obj.push_back(YKey(L"w", YNum(static_cast<double>(w))));
            o.obj.push_back(YKey(L"h", YNum(static_cast<double>(h))));
        }
        o.obj.push_back(YKey(L"class_id", YNum(it.box.class_id)));
        o.obj.push_back(YKey(L"label", YStr(it.box.label)));
        o.obj.push_back(YKey(L"bbox", YBBox(it.box.left_top, it.box.right_bottom)));
        o.obj.push_back(YKey(L"confidence", YNum(it.box.confidence)));
        o.obj.push_back(YKey(L"mask_area", YNum(it.mask_area)));
        Value poly;
        poly.type = Type::Array;
        for (const auto &p : it.polygon) {
            Value pt;
            pt.type = Type::Array;
            pt.arr.push_back(YNum(p.x));
            pt.arr.push_back(YNum(p.y));
            poly.arr.push_back(std::move(pt));
        }
        o.obj.push_back(YKey(L"polygon", std::move(poly)));
        arr.arr.push_back(std::move(o));
    }
    root.obj.push_back(YKey(L"results", std::move(arr)));
    return Stringify(root);
}

inline std::wstring BuildPoseJson(const vyolo_pose_t &items, const Options &opt, bool extra_fields, long code) {
    const auto kept = SelectKept(items, opt, PoseClassLabel, PoseScore);
    Value root;
    root.type = Type::Object;
    root.obj.push_back(YKey(L"ok", YNum(code >= 0 ? 1 : 0)));
    root.obj.push_back(YKey(L"code", YNum(static_cast<double>(code < 0 ? code : 0))));
    root.obj.push_back(YKey(L"task", YStr(L"pose")));
    root.obj.push_back(YKey(L"total", YNum(static_cast<double>(items.size()))));
    root.obj.push_back(YKey(L"returned", YNum(static_cast<double>(kept.size()))));
    Value arr;
    arr.type = Type::Array;
    for (const size_t idx : kept) {
        const auto &it = items[idx];
        Value o;
        o.type = Type::Object;
        if (extra_fields) {
            o.obj.push_back(YKey(L"index", YNum(static_cast<double>(idx))));
            const long w = it.box.right_bottom.x - it.box.left_top.x;
            const long h = it.box.right_bottom.y - it.box.left_top.y;
            o.obj.push_back(YKey(L"cx", YNum(it.box.left_top.x + w / 2.0)));
            o.obj.push_back(YKey(L"cy", YNum(it.box.left_top.y + h / 2.0)));
            o.obj.push_back(YKey(L"w", YNum(static_cast<double>(w))));
            o.obj.push_back(YKey(L"h", YNum(static_cast<double>(h))));
        }
        o.obj.push_back(YKey(L"class_id", YNum(it.box.class_id)));
        o.obj.push_back(YKey(L"label", YStr(it.box.label)));
        o.obj.push_back(YKey(L"bbox", YBBox(it.box.left_top, it.box.right_bottom)));
        o.obj.push_back(YKey(L"confidence", YNum(it.box.confidence)));
        o.obj.push_back(YKey(L"keypoint_count", YNum(static_cast<double>(it.keypoints.size()))));
        Value kpts;
        kpts.type = Type::Array;
        for (const auto &kp : it.keypoints) {
            Value ko;
            ko.type = Type::Object;
            ko.obj.push_back(YKey(L"x", YNum(kp.x)));
            ko.obj.push_back(YKey(L"y", YNum(kp.y)));
            ko.obj.push_back(YKey(L"confidence", YNum(kp.confidence)));
            kpts.arr.push_back(std::move(ko));
        }
        o.obj.push_back(YKey(L"keypoints", std::move(kpts)));
        arr.arr.push_back(std::move(o));
    }
    root.obj.push_back(YKey(L"results", std::move(arr)));
    return Stringify(root);
}

// OBB 条目投影：扁平 class_id/label/confidence。
inline auto ObBClassLabel(const yolo_obb_t &it) { return std::make_pair(static_cast<long>(it.class_id), std::cref(it.label)); }
inline double ObBScore(const yolo_obb_t &it) { return static_cast<double>(it.confidence); }

inline std::wstring BuildObbJson(const vyolo_obb_t &items, const Options &opt, bool extra_fields, long code) {
    const auto kept = SelectKept(items, opt, ObBClassLabel, ObBScore);
    Value root;
    root.type = Type::Object;
    root.obj.push_back(YKey(L"ok", YNum(code >= 0 ? 1 : 0)));
    root.obj.push_back(YKey(L"code", YNum(static_cast<double>(code < 0 ? code : 0))));
    root.obj.push_back(YKey(L"task", YStr(L"obb")));
    root.obj.push_back(YKey(L"total", YNum(static_cast<double>(items.size()))));
    root.obj.push_back(YKey(L"returned", YNum(static_cast<double>(kept.size()))));
    Value arr;
    arr.type = Type::Array;
    for (const size_t idx : kept) {
        const auto &it = items[idx];
        Value o;
        o.type = Type::Object;
        if (extra_fields)
            o.obj.push_back(YKey(L"index", YNum(static_cast<double>(idx))));
        o.obj.push_back(YKey(L"class_id", YNum(it.class_id)));
        o.obj.push_back(YKey(L"label", YStr(it.label)));
        o.obj.push_back(YKey(L"confidence", YNum(it.confidence)));
        Value center;
        center.type = Type::Array;
        center.arr.push_back(YNum(it.cx));
        center.arr.push_back(YNum(it.cy));
        o.obj.push_back(YKey(L"center", std::move(center)));
        Value size;
        size.type = Type::Array;
        size.arr.push_back(YNum(it.w));
        size.arr.push_back(YNum(it.h));
        o.obj.push_back(YKey(L"size", std::move(size)));
        o.obj.push_back(YKey(L"angle", YNum(it.angle)));
        o.obj.push_back(YKey(L"angle_deg", YNum(it.angle * 180.0 / 3.14159265358979323846)));
        o.obj.push_back(YKey(L"bbox", YBBox(it.left_top, it.right_bottom)));
        Value corners;
        corners.type = Type::Array;
        for (const auto &p : it.corners) {
            Value pt;
            pt.type = Type::Array;
            pt.arr.push_back(YNum(p.x));
            pt.arr.push_back(YNum(p.y));
            corners.arr.push_back(std::move(pt));
        }
        o.obj.push_back(YKey(L"corners", std::move(corners)));
        arr.arr.push_back(std::move(o));
    }
    root.obj.push_back(YKey(L"results", std::move(arr)));
    return Stringify(root);
}

} // namespace op::yolo::core
