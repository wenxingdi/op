#include "OpContext.h"
#include "OpCaptureHelpers.h"
#include "OpResult.h"

#include "audio/AudioService.h"
#include "base/Utils.h"
#include "json/JsonValue.h"

#include <libop.h>

#include <string>

namespace {

// 输出契约归一口径（2026-11-21 修，见 docs/2026-11/音量模块_方案_20261121.md §八）。
//
// op::audio::Set* 的内部约定是「0 = 成功，负数 = 错误码」，而库内对外契约是
// 「0 = 失败，非 0 = 成功」（宿主侧普遍 `if (ret)` 判定，OPTool/易语言的布尔化都是
// `!= 0`）。直接透传会把失败(-5)判成成功、把成功(0)判成失败 —— **语义整体反了**。
// 这里统一归一为 1/0，具体错误码写 __op.log；要码的调用方走 AudioQueryPid 的 code 字段。
long setter_result(const wchar_t *api_name, int code) {
    if (code != 0) {
        setlog(L"%s: 失败，错误码 %d（-2 COM 不可用 / -4 PID 无效 / -5 设置失败）", api_name, code);
        return 0L;
    }
    return 1L;
}

// 统一构建会话查询 JSON。字段见 docs/2026-11/音量模块_方案_20261121.md §3.1。
void build_session_json(const op::audio::SessionInfo &info, std::wstring &retjson) {
    using namespace op::json;
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
    auto boolv = [](bool b) {
        Value v;
        v.type = Type::Bool;
        v.boolean = b;
        return v;
    };
    auto key = [](const wchar_t *k, Value v) { return std::make_pair(std::wstring(k), std::move(v)); };

    Value root;
    root.type = Type::Object;
    root.obj.push_back(key(L"ok", num(info.ok ? 1 : 0)));
    root.obj.push_back(key(L"code", num(info.code)));
    root.obj.push_back(key(L"found", boolv(info.found)));
    root.obj.push_back(key(L"pid", num(static_cast<double>(info.pid))));
    root.obj.push_back(key(L"muted", boolv(info.muted)));
    root.obj.push_back(key(L"playing", boolv(info.playing)));
    root.obj.push_back(key(L"volume", num(info.volume)));
    root.obj.push_back(key(L"peak", num(info.peak)));
    root.obj.push_back(key(L"error", strv(info.error)));
    retjson = op::json::Stringify(root);
}

void build_system_json(const op::audio::SystemVolumeInfo &info, std::wstring &retjson) {
    using namespace op::json;
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
    auto boolv = [](bool b) {
        Value v;
        v.type = Type::Bool;
        v.boolean = b;
        return v;
    };
    auto key = [](const wchar_t *k, Value v) { return std::make_pair(std::wstring(k), std::move(v)); };

    Value root;
    root.type = Type::Object;
    root.obj.push_back(key(L"ok", num(info.ok ? 1 : 0)));
    root.obj.push_back(key(L"code", num(info.code)));
    root.obj.push_back(key(L"muted", boolv(info.muted)));
    root.obj.push_back(key(L"volume", num(info.volume)));
    root.obj.push_back(key(L"error", strv(info.error)));
    retjson = op::json::Stringify(root);
}

} // namespace

void op::Op::AudioQueryWindow(long long hwnd, std::wstring &retjson) {
    retjson.clear();
    internal::guard_exceptions("AudioQueryWindow", [&]() {
        build_session_json(op::audio::QueryByWindow(reinterpret_cast<void *>(static_cast<intptr_t>(hwnd))), retjson);
    });
}

void op::Op::AudioQueryPid(long pid, std::wstring &retjson) {
    retjson.clear();
    internal::guard_exceptions("AudioQueryPid", [&]() {
        build_session_json(op::audio::QueryByPid(pid > 0 ? static_cast<unsigned long>(pid) : 0), retjson);
    });
}

void op::Op::AudioGetPidFromHwnd(long long hwnd, long *ret) {
    internal::set_result(ret, 0L);
    internal::guard_exceptions("AudioGetPidFromHwnd", [&]() {
        const unsigned long pid =
            op::audio::GetPidFromWindow(reinterpret_cast<void *>(static_cast<intptr_t>(hwnd)));
        internal::set_result(ret, static_cast<long>(pid));
    });
}

void op::Op::AudioSetSessionVolume(long pid, double volume, long *ret) {
    internal::set_result(ret, 0L);
    internal::guard_exceptions("AudioSetSessionVolume", [&]() {
        const int code = pid > 0
                             ? op::audio::SetSessionVolume(static_cast<unsigned long>(pid), volume)
                             : -4;
        internal::set_result(ret, setter_result(L"AudioSetSessionVolume", code));
    });
}

void op::Op::AudioSetSessionMute(long pid, long muted, long *ret) {
    internal::set_result(ret, 0L);
    internal::guard_exceptions("AudioSetSessionMute", [&]() {
        const int code =
            pid > 0 ? op::audio::SetSessionMute(static_cast<unsigned long>(pid), muted != 0) : -4;
        internal::set_result(ret, setter_result(L"AudioSetSessionMute", code));
    });
}

void op::Op::AudioGetSystemVolume(std::wstring &retjson) {
    retjson.clear();
    internal::guard_exceptions("AudioGetSystemVolume",
                               [&]() { build_system_json(op::audio::GetSystemVolume(), retjson); });
}

void op::Op::AudioSetSystemVolume(double volume, long *ret) {
    internal::set_result(ret, 0L);
    internal::guard_exceptions("AudioSetSystemVolume", [&]() {
        internal::set_result(ret, setter_result(L"AudioSetSystemVolume", op::audio::SetSystemVolume(volume)));
    });
}

void op::Op::AudioSetSystemMute(long muted, long *ret) {
    internal::set_result(ret, 0L);
    internal::guard_exceptions("AudioSetSystemMute", [&]() {
        internal::set_result(ret, setter_result(L"AudioSetSystemMute", op::audio::SetSystemMute(muted != 0)));
    });
}
