# YOLO 核心外提（2026-11-21）：让 OpYolo.cpp 与 OpMemImage.cpp 共用一份检测逻辑
# 用法：python scripts\probes\refactor_yolo_core.py

import io
import os
import sys

REPO = r"D:\AutoPro\op-master\op"
OP_YOLO = os.path.join(REPO, "libop", "op", "OpYolo.cpp")
OP_MEM = os.path.join(REPO, "libop", "op", "OpMemImage.cpp")


def read(p):
    with io.open(p, "r", encoding="utf-8", newline="") as f:
        return f.read()


def safe_write(path, new_text, must_contain, min_lines=1):
    old = read(path)
    if old.count("\n") < min_lines:
        print("  FAIL truncated:", os.path.basename(path))
        return False
    with io.open(path + ".bak", "w", encoding="utf-8", newline="") as f:
        f.write(old)
    with io.open(path, "w", encoding="utf-8", newline="") as f:
        f.write(new_text)
    got = read(path)
    missing = [k for k in must_contain if k not in got]
    if missing:
        with io.open(path, "w", encoding="utf-8", newline="") as f:
            f.write(old)
        print("  ROLLBACK:", os.path.basename(path), "missing", missing)
        return False
    os.remove(path + ".bak")
    print("  OK:", os.path.basename(path))
    return True


NEW_OP_YOLO = r'''#include "OpContext.h"
#include "OpCaptureHelpers.h"
#include "OpResult.h"
#include "YoloDetectCore.h"

#include "image/Image.h"
#include "base/JsonUtils.h"
#include "base/Utils.h"
#include "yolo/YoloDetector.h"

#include <libop.h>

#include <cwctype>
#include <string>
#include <vector>

namespace {

// 检测核心（过滤/截断/JSON 构建）已抽到 YoloDetectCore.h，
// 与内存版 OpYoloDetectFromMem* 共用同一份实现。
using op::yolo::core::BuildJson;
using op::yolo::core::kFailureJson;
using op::yolo::core::KeepDetection;
using op::yolo::core::OffsetDetections;
using op::yolo::core::Options;
using op::yolo::core::ParseOptions;

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
// 坐标口径为"截图偏移 + 客户区偏移"，与 OCR/找图一致。
void op::Op::YoloDetect(long x1, long y1, long x2, long y2, double conf, double iou, std::wstring &retjson, long *ret) {
    retjson = kFailureJson;
    internal::set_result(ret, 0L);
    internal::with_captured_region(m_context.get(), x1, y1, x2, y2, [&]() {
        vyolo_rec_t res;
        const int n = op::yolo::YoloDetector::getInstance()->detect(
            m_context->image_proc._src.pdata, m_context->image_proc._src.width, m_context->image_proc._src.height, 4,
            conf, iou, res);
        if (n < 0)
            return;
        OffsetDetections(res, x1 + m_context->image_proc._dx, y1 + m_context->image_proc._dy);
        Options no_filter;
        BuildJson(res, no_filter, /*extra_fields=*/false, retjson);
        internal::set_result(ret, n);
    });
}

void op::Op::YoloDetectFromFile(const wchar_t *file_name, double conf, double iou, std::wstring &retjson, long *ret) {
    retjson = kFailureJson;
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
    Options no_filter;
    BuildJson(res, no_filter, /*extra_fields=*/false, retjson);
    internal::set_result(ret, n);
}

// Ex2：带过滤/截断的检测。
// json_options 为 {"class_ids":[],"labels":[],"max_results":N}，空串等价于不过滤。
// 非法 options 返回 -1 且 retjson 置失败 JSON（调用方据此区分"参数错"与"没检测到"）。
long op::Op::YoloDetectEx2(long x1, long y1, long x2, long y2, double conf, double iou,
                           const wchar_t *json_options, std::wstring &retjson, long *ret) {
    retjson = kFailureJson;
    internal::set_result(ret, 0L);

    Options opt;
    const std::wstring options_text = json_options ? json_options : L"";
    if (!ParseOptions(options_text, opt))
        return -1; // 参数非法：显式失败，不当作"没检测到"

    long found = 0;
    internal::with_captured_region(m_context.get(), x1, y1, x2, y2, [&]() {
        vyolo_rec_t res;
        const int n = op::yolo::YoloDetector::getInstance()->detect(
            m_context->image_proc._src.pdata, m_context->image_proc._src.width, m_context->image_proc._src.height, 4,
            conf, iou, res);
        if (n < 0)
            return;
        OffsetDetections(res, x1 + m_context->image_proc._dx, y1 + m_context->image_proc._dy);
        BuildJson(res, opt, /*extra_fields=*/true, retjson);
        // 统计过滤后的条数（与 BuildJson 的 returned 同口径）
        found = 0;
        for (size_t i = 0; i < res.size(); ++i) {
            if (KeepDetection(res, i, opt))
                ++found;
        }
        if (opt.max_results > 0 && found > opt.max_results)
            found = opt.max_results;
    });
    internal::set_result(ret, found);
    return found;
}

// Ex2 的文件版：从磁盘图片检测 + 过滤/截断。
// 存在的理由：屏幕版 Ex2 依赖绑定窗口，无窗口环境下无法验证过滤逻辑；
// 文件版让"过滤/截断/JSON 契约"可以用**真实模型**闭环验证（见 yolo_ex2_test.cpp）。
long op::Op::YoloDetectFromFileEx2(const wchar_t *file_name, double conf, double iou, const wchar_t *json_options,
                                   std::wstring &retjson, long *ret) {
    retjson = kFailureJson;
    internal::set_result(ret, 0L);

    Options opt;
    const std::wstring options_text = json_options ? json_options : L"";
    if (!ParseOptions(options_text, opt))
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

    BuildJson(res, opt, /*extra_fields=*/true, retjson);
    long found = 0;
    for (size_t i = 0; i < res.size(); ++i) {
        if (KeepDetection(res, i, opt))
            ++found;
    }
    if (opt.max_results > 0 && found > opt.max_results)
        found = opt.max_results;
    internal::set_result(ret, found);
    return found;
}
'''

# OpMemImage.cpp 的 YOLO 部分改成调用共享核心
YOLO_MEM = r'''
// ---- YOLO ----

void op::Op::YoloDetectFromMem(const wchar_t *image_handle, long x1, long y1, long x2, long y2, double conf,
                               double iou, std::wstring &retjson, long *ret) {
    retjson = op::yolo::core::kFailureJson;
    internal::set_result(ret, 0L);
    const std::wstring handle = SafeHandle(image_handle);
    internal::with_mem_region(m_context.get(), handle, x1, y1, x2, y2, [&]() {
        // 与屏幕版 YoloDetect 同一套检测 + JSON 构建（共享 YoloDetectCore.h）；
        // with_mem_region 已把偏移设为 0，故**不加任何平移**，结果就是图像内坐标。
        vyolo_rec_t res;
        const int n = op::yolo::YoloDetector::getInstance()->detect(
            m_context->image_proc._src.pdata, m_context->image_proc._src.width, m_context->image_proc._src.height, 4,
            conf, iou, res);
        if (n < 0)
            return;
        op::yolo::core::Options no_filter;
        op::yolo::core::BuildJson(res, no_filter, /*extra_fields=*/false, retjson);
        internal::set_result(ret, n);
    });
}

long op::Op::YoloDetectFromMemEx2(const wchar_t *image_handle, long x1, long y1, long x2, long y2, double conf,
                                  double iou, const wchar_t *json_options, std::wstring &retjson, long *ret) {
    retjson = op::yolo::core::kFailureJson;
    internal::set_result(ret, 0L);

    op::yolo::core::Options opt;
    const std::wstring options_text = json_options ? json_options : L"";
    if (!op::yolo::core::ParseOptions(options_text, opt))
        return -1; // 参数非法：显式失败，与屏幕版 Ex2 一致

    const std::wstring handle = SafeHandle(image_handle);
    long found = 0;
    internal::with_mem_region(m_context.get(), handle, x1, y1, x2, y2, [&]() {
        vyolo_rec_t res;
        const int n = op::yolo::YoloDetector::getInstance()->detect(
            m_context->image_proc._src.pdata, m_context->image_proc._src.width, m_context->image_proc._src.height, 4,
            conf, iou, res);
        if (n < 0)
            return;
        op::yolo::core::BuildJson(res, opt, /*extra_fields=*/true, retjson);
        found = 0;
        for (size_t i = 0; i < res.size(); ++i) {
            if (op::yolo::core::KeepDetection(res, i, opt))
                ++found;
        }
        if (opt.max_results > 0 && found > opt.max_results)
            found = opt.max_results;
    });
    internal::set_result(ret, found);
    return found;
}
'''


def main():
    ok = True

    print("[1/2] rewrite OpYolo.cpp to use shared core")
    old = read(OP_YOLO)
    nl = "\r\n" if "\r\n" in old else "\n"
    body = NEW_OP_YOLO.replace("\r\n", "\n").replace("\n", nl)
    ok &= safe_write(OP_YOLO, body, ["YoloDetectCore.h", "using op::yolo::core::BuildJson", "YoloDetectFromFileEx2"], 100)

    print("[2/2] replace YOLO tail of OpMemImage.cpp")
    mem = read(OP_MEM)
    nl2 = "\r\n" if "\r\n" in mem else "\n"
    marker = "// ---- YOLO ----"
    if marker not in mem:
        print("  FAIL: YOLO marker missing in OpMemImage.cpp")
        return 1
    head = mem[: mem.index(marker)]
    newmem = head + YOLO_MEM.replace("\r\n", "\n").replace("\n", nl2)
    ok &= safe_write(OP_MEM, newmem, ["YoloDetectFromMemEx2", "op::yolo::core::BuildJson"], 50)

    print("\nRESULT:", "OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
