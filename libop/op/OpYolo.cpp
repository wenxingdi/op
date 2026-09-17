#include "OpContext.h"
#include "OpCaptureHelpers.h"
#include "OpResult.h"

#include "image/Image.h"
#include "base/JsonUtils.h"
#include "base/Utils.h"
#include "yolo/YoloDetector.h"

#include <libop.h>

#include <cwctype>
#include <string>
#include <vector>

namespace {

constexpr const wchar_t *kYoloFailureJson = L"{\"ok\":0,\"code\":-1,\"results\":[]}";

static void build_yolo_json(const op::vyolo_rec_t &items, std::wstring &retjson) {
    retjson = L"{\"ok\":1,\"code\":0,\"results\":[";
    bool first = true;
    for (const auto &it : items) {
        if (!first)
            retjson += L",";
        first = false;
        retjson += L"{\"class_id\":";
        retjson += std::to_wstring(it.class_id);
        retjson += L",\"label\":\"";
        retjson += op::internal::json::EscapeString(it.label);
        retjson += L"\",\"bbox\":[";
        retjson += std::to_wstring(it.left_top.x);
        retjson += L",";
        retjson += std::to_wstring(it.left_top.y);
        retjson += L",";
        retjson += std::to_wstring(it.right_bottom.x);
        retjson += L",";
        retjson += std::to_wstring(it.right_bottom.y);
        retjson += L"],\"confidence\":";
        retjson += op::internal::json::FormatDouble(it.confidence);
        retjson += L"}";
    }
        retjson += L"]}";
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
void op::Op::YoloDetect(long x1, long y1, long x2, long y2, double conf, double iou, std::wstring &retjson, long *ret) {
    retjson = kYoloFailureJson;
    internal::set_result(ret, 0L);
    internal::with_captured_region(m_context.get(), x1, y1, x2, y2, [&]() {
        vyolo_rec_t res;
        const int n =
            op::yolo::YoloDetector::getInstance()->detect(m_context->image_proc._src.pdata, m_context->image_proc._src.width,
                                               m_context->image_proc._src.height, 4, conf, iou, res);
        if (n < 0)
            return;
        for (auto &it : res) {
            it.left_top.x += static_cast<int>(x1);
            it.left_top.y += static_cast<int>(y1);
            it.right_bottom.x += static_cast<int>(x1);
            it.right_bottom.y += static_cast<int>(y1);
        }
        build_yolo_json(res, retjson);
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
    build_yolo_json(res, retjson);
    internal::set_result(ret, n);
}
