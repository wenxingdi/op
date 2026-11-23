#include "OpContext.h"
#include "OpCaptureHelpers.h"
#include "OpResult.h"
#include "YoloDetectCore.h"

#include "image/Image.h"
#include "base/CryptoBox.h"
#include "base/JsonUtils.h"
#include "base/Utils.h"
#include "json/JsonValue.h"
#include "yolo/YoloDetector.h"

#include <libop.h>

#include <Windows.h>
#include <dxgi.h>

// 设备枚举走 DXGI；项目默认不链接 dxgi.lib，这里按 OpCrypto 的先例 pragma 补链。
#pragma comment(lib, "dxgi.lib")

#include <cwctype>
#include <cstring>
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

// ---------------------------------------------------------------- 模型平台
//
// 注册表本体在 YoloDetector：这里只负责"路径解析 / 加密包解封 / JSON 契约"。
// 加密包在内存里解密后直接建 ONNX 会话，明文模型不落临时文件。

namespace {

using namespace op::json;

// 与 libop.h 注释一致：OPYMOD01（8 字节，无 NUL）
constexpr char kYoloSealMagic[8] = {'O', 'P', 'Y', 'M', 'O', 'D', '0', '1'};

Value YNum(double v) {
    Value n;
    n.type = Type::Number;
    n.number = v;
    return n;
}

Value YStr(const std::wstring &s) {
    Value v;
    v.type = Type::String;
    v.str = s;
    return v;
}

void Add(Value &obj, const wchar_t *key, Value v) {
    obj.obj.push_back(std::make_pair(std::wstring(key), std::move(v)));
}

std::wstring YoloErrorJson(const wchar_t *error, const wchar_t *message) {
    Value root;
    root.type = Type::Object;
    Add(root, L"ok", YNum(0));
    Add(root, L"error", YStr(error ? error : L"unknown"));
    if (message && *message)
        Add(root, L"message", YStr(message));
    return Stringify(root);
}

Value ModelValue(const op::yolo::NamedModelInfo &m) {
    Value v;
    v.type = Type::Object;
    Add(v, L"name", YStr(m.name));
    Add(v, L"task", YStr(op::yolo::TaskName(m.task)));
    Add(v, L"task_code", YNum(static_cast<double>(m.task)));
    Add(v, L"encrypted", YNum(m.encrypted ? 1 : 0));
    Add(v, L"active", YNum(m.active ? 1 : 0));
    Add(v, L"input_width", YNum(m.input_width));
    Add(v, L"input_height", YNum(m.input_height));
    Add(v, L"device", YNum(m.device));
    Add(v, L"dml", YNum(m.device >= 0 ? 1 : 0));
    Add(v, L"memory_loaded", YNum(m.path.empty() ? 1 : 0));
    if (!m.path.empty())
        Add(v, L"path", YStr(m.path));
    return v;
}

std::wstring ModelInfoJson(const op::yolo::NamedModelInfo &m) {
    Value root = ModelValue(m);
    // 单模型查询与列表项同构，只多一个顶层 ok
    Value out;
    out.type = Type::Object;
    Add(out, L"ok", YNum(1));
    for (auto &kv : root.obj)
        out.obj.push_back(kv);
    return Stringify(out);
}

bool FindModel(const std::wstring &name, op::yolo::NamedModelInfo &out) {
    const auto models = op::yolo::YoloDetector::getInstance()->list_models();
    for (const auto &m : models) {
        if (m.name == name) {
            out = m;
            return true;
        }
    }
    return false;
}

std::vector<std::string> SplitEngineArgv(const wchar_t *argv) {
    std::vector<std::string> out;
    if (!argv)
        return out;
    split(_ws2string(argv), out, " ");
    return out;
}

// 密码从 argv 里摘掉：引擎不认识 --password=，留在里面只会误导排障。
std::string TakePassword(std::vector<std::string> &argv) {
    std::string password;
    for (auto it = argv.begin(); it != argv.end();) {
        if (it->rfind("--password=", 0) == 0) {
            password = it->substr(11);
            it = argv.erase(it);
        } else {
            ++it;
        }
    }
    return password;
}

bool IsSealedFile(const std::wstring &path) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    char magic[8] = {};
    DWORD got = 0;
    const bool ok = ::ReadFile(h, magic, 8, &got, nullptr) && got == 8 &&
                    std::memcmp(magic, kYoloSealMagic, 8) == 0;
    ::CloseHandle(h);
    return ok;
}

std::wstring ResolveOutputPath(const std::wstring &path, const std::wstring &curr_path) {
    if (path.size() >= 2 && path[1] == L':')
        return path;
    if (path.rfind(L"\\\\", 0) == 0)
        return path;
    return curr_path + L"\\" + path;
}

} // namespace

void op::Op::YoloLoadModel(const wchar_t *name, const wchar_t *path, const wchar_t *argv, std::wstring &retjson,
                           long *ret) {
    internal::set_result(ret, 0L);
    retjson = YoloErrorJson(L"invalid_argument", L"");
    const std::wstring model_name = (name && *name) ? std::wstring(name) : std::wstring(L"default");

    std::wstring fullpath;
    if (!path || !Path2GlobalPath(path, m_context->curr_path, fullpath)) {
        retjson = YoloErrorJson(L"file_not_found", L"");
        return;
    }

    std::vector<std::string> av = SplitEngineArgv(argv);
    const std::string password = TakePassword(av);

    int rc = -1;
    if (IsSealedFile(fullpath)) {
        if (password.empty()) {
            retjson = YoloErrorJson(L"password_required", L"encrypted package needs --password=");
            return;
        }
        std::vector<uint8_t> blob;
        std::vector<uint8_t> plain;
        if (!op::cryptobox::read_file(fullpath, blob) ||
            !op::cryptobox::unseal(blob.data(), blob.size(), kYoloSealMagic, password, plain)) {
            retjson = YoloErrorJson(L"decrypt_failed", L"wrong password or corrupted package");
            return;
        }
        rc = op::yolo::YoloDetector::getInstance()->load_named_memory(model_name, plain, av, true);
        op::cryptobox::secure_zero(plain.data(), plain.size());
    } else {
        rc = op::yolo::YoloDetector::getInstance()->load_named(model_name, fullpath, av, false);
    }
    if (rc != 0) {
        retjson = YoloErrorJson(L"load_failed", L"model rejected by inference engine");
        return;
    }

    op::yolo::NamedModelInfo info;
    if (FindModel(model_name, info))
        retjson = ModelInfoJson(info);
    internal::set_result(ret, 1L);
}

void op::Op::YoloUseModel(const wchar_t *name, long *ret) {
    internal::set_result(ret, 0L);
    if (!name || !*name)
        return;
    op::yolo::NamedModelInfo info;
    if (!FindModel(name, info))
        return; // 未注册：先 YoloLoadModel
    if (op::yolo::YoloDetector::getInstance()->use_named(name) != 0)
        return;
    internal::set_result(ret, 1L);
}

void op::Op::YoloReleaseModel(const wchar_t *name, long *ret) {
    internal::set_result(ret, 0L);
    auto *detector = op::yolo::YoloDetector::getInstance();
    if (name && *name) {
        internal::set_result(ret, detector->release_named(name) ? 1L : 0L);
        return;
    }
    // 空名 = 全部释放；操作本身成功即可（可能本来就没有模型）
    const auto models = detector->list_models();
    for (const auto &m : models)
        detector->release_named(m.name);
    internal::set_result(ret, 1L);
}

void op::Op::YoloListModels(std::wstring &retjson) {
    retjson.clear();
    internal::guard_exceptions("YoloListModels", [&]() {
        auto *detector = op::yolo::YoloDetector::getInstance();
        const auto models = detector->list_models();
        Value root;
        root.type = Type::Object;
        Add(root, L"ok", YNum(1));
        Add(root, L"active", YStr(detector->active_model()));
        Add(root, L"count", YNum(static_cast<double>(models.size())));
        Value arr;
        arr.type = Type::Array;
        for (const auto &m : models)
            arr.arr.push_back(ModelValue(m));
        Add(root, L"models", std::move(arr));
        retjson = Stringify(root);
    });
}

void op::Op::YoloListDevices(std::wstring &retjson) {
    retjson.clear();
    internal::guard_exceptions("YoloListDevices", [&]() {
        auto *detector = op::yolo::YoloDetector::getInstance();
        const int active = detector->device_id();

        Value root;
        root.type = Type::Object;
        Add(root, L"ok", YNum(1));
        Add(root, L"dml_available", YNum(detector->dml_supported() ? 1 : 0));
        Add(root, L"active", YNum(active));

        Value arr;
        arr.type = Type::Array;
        Value cpu;
        cpu.type = Type::Object;
        Add(cpu, L"index", YNum(-1));
        Add(cpu, L"name", YStr(L"CPU"));
        Add(cpu, L"type", YStr(L"cpu"));
        Add(cpu, L"vram_mb", YNum(0));
        Add(cpu, L"active", YNum(active == -1 ? 1 : 0));
        arr.arr.push_back(std::move(cpu));

        IDXGIFactory1 *factory = nullptr;
        if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory) {
            Add(root, L"devices", std::move(arr));
            retjson = Stringify(root);
            return;
        }
        for (UINT i = 0;; ++i) {
            IDXGIAdapter1 *adapter = nullptr;
            if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
                break;
            if (!adapter)
                break;
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc))) {
                const bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
                Value d;
                d.type = Type::Object;
                Add(d, L"index", YNum(static_cast<double>(i)));
                Add(d, L"name", YStr(desc.Description));
                Add(d, L"type", YStr(software ? L"software" : L"gpu"));
                Add(d, L"vendor_id", YNum(desc.VendorId));
                Add(d, L"vram_mb", YNum(static_cast<double>(desc.DedicatedVideoMemory / (1024ull * 1024ull))));
                Add(d, L"active", YNum(active == static_cast<int>(i) ? 1 : 0));
                arr.arr.push_back(std::move(d));
            }
            adapter->Release();
        }
        factory->Release();
        Add(root, L"devices", std::move(arr));
        retjson = Stringify(root);
    });
}

void op::Op::YoloEncryptModel(const wchar_t *src_path, const wchar_t *dst_path, const wchar_t *password, long *ret) {
    internal::set_result(ret, 0L);
    if (!src_path || !dst_path || !password || !*password)
        return;
    std::wstring src;
    if (!Path2GlobalPath(src_path, m_context->curr_path, src))
        return;
    std::vector<uint8_t> plain;
    if (!op::cryptobox::read_file(src, plain) || plain.empty())
        return;
    const std::string pw = _ws2string(password);
    std::vector<uint8_t> sealed =
        op::cryptobox::seal(kYoloSealMagic, pw, plain.data(), plain.size());
    op::cryptobox::secure_zero(plain.data(), plain.size());
    if (sealed.empty())
        return;
    const std::wstring dst = ResolveOutputPath(dst_path, m_context->curr_path);
    if (!op::cryptobox::write_file(dst, sealed.data(), sealed.size()))
        return;
    internal::set_result(ret, 1L);
}

void op::Op::YoloDecryptModel(const wchar_t *src_path, const wchar_t *dst_path, const wchar_t *password, long *ret) {
    internal::set_result(ret, 0L);
    if (!src_path || !dst_path || !password || !*password)
        return;
    std::wstring src;
    if (!Path2GlobalPath(src_path, m_context->curr_path, src))
        return;
    std::vector<uint8_t> blob;
    std::vector<uint8_t> plain;
    if (!op::cryptobox::read_file(src, blob) ||
        !op::cryptobox::unseal(blob.data(), blob.size(), kYoloSealMagic, _ws2string(password), plain))
        return;
    const std::wstring dst = ResolveOutputPath(dst_path, m_context->curr_path);
    const bool ok = op::cryptobox::write_file(dst, plain.data(), plain.size());
    op::cryptobox::secure_zero(plain.data(), plain.size());
    if (ok)
        internal::set_result(ret, 1L);
}
