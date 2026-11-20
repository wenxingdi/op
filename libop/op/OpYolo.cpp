#include "OpContext.h"
#include "OpCaptureHelpers.h"
#include "OpResult.h"

#include "image/Image.h"
#include "base/JsonUtils.h"
#include "../json/JsonValue.h" // YOLO Ex2：过滤选项解析与结果 JSON 构建用 op::json DOM
#include "base/Utils.h"
#include "yolo/YoloDetector.h"

#include <libop.h>

#include <cwctype>
#include <string>
#include <vector>

namespace {

constexpr const wchar_t *kYoloFailureJson = L"{\"ok\":0,\"code\":-1,\"results\":[]}";

// 过滤/截断选项（Ex2 用）。空 = 不过滤。
struct yolo_options_t {
    std::vector<long> class_ids;
    std::vector<std::wstring> labels;
    long max_results = 0; // 0 = 不限
    bool has_class_filter = false;
    bool has_label_filter = false;
};

// 解析 json_options。空串 => 全部取默认（返回 true）。
// 未知字段 / 类型错误 / JSON 语法错误一律返回 false（显式失败，不静默降级）。
bool parse_yolo_options(const std::wstring &json_text, yolo_options_t &opt) {
    opt = yolo_options_t{};
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
bool keep_detection(const op::vyolo_rec_t &items, size_t idx, const yolo_options_t &opt) {
    const auto &it = items[idx];
    if (!opt.has_class_filter && !opt.has_label_filter)
        return true;
    if (opt.has_class_filter) {
        for (long id : opt.class_ids) {
            if (id == it.class_id)
                return true;
        }
    }
    if (opt.has_label_filter) {
        for (const auto &name : opt.labels) {
            if (name == it.label)
                return true;
        }
    }
    return false;
}

// 构建 YOLO 结果 JSON。
//
// 用 op::json DOM 而非手拼字符串（2026-11-21 改）：手拼时每加一个字段
// 就要动一处分号与逗号，结构一变极易漏转义；DOM 由 Stringify 统一负责转义。
//
// extra_fields=true 时额外输出 index/cx/cy/w/h 与 total/returned（Ex2 用）；
// 旧接口保持原结构（只有 class_id/label/bbox/confidence），避免破坏既有调用方。
void build_yolo_json(const op::vyolo_rec_t &items, const yolo_options_t &opt, bool extra_fields,
                     std::wstring &retjson) {
    using namespace op::json;

    // 先选出生效下标（保持原顺序，便于用 index 对照）
    std::vector<size_t> kept;
    kept.reserve(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        if (keep_detection(items, i, opt))
            kept.push_back(i);
    }
    const long total = static_cast<long>(items.size());

    // max_results：按 confidence 降序截断。
    // 注意"降序截断"而不是"按原序取前 N"——调用方要的是"最可信的 N 个"。
    if (opt.max_results > 0 && static_cast<long>(kept.size()) > opt.max_results) {
        std::stable_sort(kept.begin(), kept.end(), [&items](size_t a, size_t b) {
            return items[a].confidence > items[b].confidence;
        });
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
// off_x/off_y 必须是"截图偏移 + 客户区偏移"，与 OCR/找图保持同一口径
// （2026-11-21 修正：原实现只加了截图偏移，窗口模式下坐标会与其他模块错位）。
void offset_detections(op::vyolo_rec_t &res, long off_x, long off_y) {
    for (auto &it : res) {
        it.left_top.x += static_cast<int>(off_x);
        it.left_top.y += static_cast<int>(off_y);
        it.right_bottom.x += static_cast<int>(off_x);
        it.right_bottom.y += static_cast<int>(off_y);
    }
}

// 统一入口判定：path_of_engine 直接给 .onnx 模型文件路径（dll_name 为空）时，
// 自动切进程内 ONNX 引擎并以该路径为模型文件——用户无需了解引擎选择细节。
bool has_onnx_model_extension(const std::wstring &path) {
    const size_t sep = path.find_last_of(L"\\/");
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (sep != std::wstring::npos && dot < sep))
        return false;
    std::wstring ext = path.substr(dot + 1);
    for (auto &c : ext)
        c = static_cast<wchar_t>(towlower(c));
    return ext == L"onnx";
}

} // namespace

long op::Op::SetYoloEngine(const wchar_t *path_of_engine, const wchar_t *dll_name, const wchar_t *argv) {
    string argvs = argv ? _ws2string(argv) : "";
    vector<string> vstr;
    split(argvs, vstr, " ");
    std::wstring engine = path_of_engine ? path_of_engine : L"";
    std::wstring dll = dll_name ? dll_name : L"";
    // 统一入口：SetYoloEngine("D:/xx/best.onnx", "", "--labels=...") 等价于
    // SetYoloEngine("onnx", "D:/xx/best.onnx", ...)。dll_name 非空时保持旧语义（优先作模型路径）。
    if (dll.empty() && has_onnx_model_extension(engine)) {
        dll = engine;
        engine = L"onnx";
    }
    return op::yolo::YoloDetector::getInstance()->init(engine, dll, vstr) == 0 ? 1 : 0;
}

// 旧接口：保持原 JSON 结构（只有 class_id/label/bbox/confidence），不加过滤。
// 坐标口径已修正为"截图偏移 + 客户区偏移"，与 OCR/找图一致（行为变更，CHANGELOG 已标注）。
void op::Op::YoloDetect(long x1, long y1, long x2, long y2, double conf, double iou, std::wstring &retjson, long *ret) {
    retjson = kYoloFailureJson;
    internal::set_result(ret, 0L);
    internal::with_captured_region(m_context.get(), x1, y1, x2, y2, [&]() {
        vyolo_rec_t res;
        const int n = op::yolo::YoloDetector::getInstance()->detect(
            m_context->image_proc._src.pdata, m_context->image_proc._src.width, m_context->image_proc._src.height, 4,
            conf, iou, res);
        if (n < 0)
            return;
        offset_detections(res, x1 + m_context->image_proc._dx, y1 + m_context->image_proc._dy);
        yolo_options_t no_filter;
        build_yolo_json(res, no_filter, /*extra_fields=*/false, retjson);
        internal::set_result(ret, n);
    });
}

void op::Op::YoloDetectFromFile(const wchar_t *file_name, double conf, double iou, std::wstring &retjson, long *ret) {
    retjson = kYoloFailureJson;
    internal::set_result(ret, 0L);
    std::wstring fullpath;
    if (!Path2GlobalPath(file_name ? file_name : L"", m_context->curr_path, fullpath))
        return;
    Image img;
    if (!img.read(fullpath.data()))
        return;
    vyolo_rec_t res;
    const int n = op::yolo::YoloDetector::getInstance()->detect(img.pdata, img.width, img.height, 4, conf, iou, res);
    if (n < 0)
        return;
    yolo_options_t no_filter;
    build_yolo_json(res, no_filter, /*extra_fields=*/false, retjson);
    internal::set_result(ret, n);
}

// Ex2：带过滤/截断的检测（2026-11-21 新增）。
// json_options 为 {"class_ids":[],"labels":[],"max_results":N}，空串等价于不过滤。
// 非法 options 返回 -1 且 retjson 置失败 JSON（调用方据此区分"参数错"与"没检测到"）。
long op::Op::YoloDetectEx2(long x1, long y1, long x2, long y2, double conf, double iou,
                           const wchar_t *json_options, std::wstring &retjson, long *ret) {
    retjson = kYoloFailureJson;
    internal::set_result(ret, 0L);

    yolo_options_t opt;
    const std::wstring options_text = json_options ? json_options : L"";
    if (!parse_yolo_options(options_text, opt))
        return -1; // 参数非法：显式失败，不当作"没检测到"

    long found = 0;
    long total = 0;
    internal::with_captured_region(m_context.get(), x1, y1, x2, y2, [&]() {
        vyolo_rec_t res;
        const int n = op::yolo::YoloDetector::getInstance()->detect(
            m_context->image_proc._src.pdata, m_context->image_proc._src.width, m_context->image_proc._src.height, 4,
            conf, iou, res);
        if (n < 0)
            return;
        offset_detections(res, x1 + m_context->image_proc._dx, y1 + m_context->image_proc._dy);
        total = static_cast<long>(res.size());
        build_yolo_json(res, opt, /*extra_fields=*/true, retjson);
        // 统计过滤后的条数（build_yolo_json 已算过，这里按同口径再算一遍，
        // 保持 ret 与 JSON 的 returned 一致）
        found = 0;
        for (size_t i = 0; i < res.size(); ++i) {
            if (keep_detection(res, i, opt))
                ++found;
        }
        if (opt.max_results > 0 && found > opt.max_results)
            found = opt.max_results;
    });
    internal::set_result(ret, found);
    return found;
}

// Ex2 的文件版：从磁盘图片检测 + 过滤/截断。
// 存在的理由：Ex2 的屏幕版依赖绑定窗口，无窗口环境下无法验证过滤逻辑；
// 文件版让"过滤/截断/JSON 契约"可以用**真实模型**闭环验证（见 yolo_ex2_test.cpp）。
long op::Op::YoloDetectFromFileEx2(const wchar_t *file_name, double conf, double iou, const wchar_t *json_options,
                                   std::wstring &retjson, long *ret) {
    retjson = kYoloFailureJson;
    internal::set_result(ret, 0L);

    yolo_options_t opt;
    const std::wstring options_text = json_options ? json_options : L"";
    if (!parse_yolo_options(options_text, opt))
        return -1; // 参数非法：显式失败

    std::wstring fullpath;
    if (!Path2GlobalPath(file_name ? file_name : L"", m_context->curr_path, fullpath))
        return 0;
    Image img;
    if (!img.read(fullpath.data()))
        return 0;

    vyolo_rec_t res;
    const int n = op::yolo::YoloDetector::getInstance()->detect(img.pdata, img.width, img.height, 4, conf, iou, res);
    if (n < 0)
        return 0;

    build_yolo_json(res, opt, /*extra_fields=*/true, retjson);
    long found = 0;
    for (size_t i = 0; i < res.size(); ++i) {
        if (keep_detection(res, i, opt))
            ++found;
    }
    if (opt.max_results > 0 && found > opt.max_results)
        found = opt.max_results;
    internal::set_result(ret, found);
    return found;
}