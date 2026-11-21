#include "OpContext.h"
#include "OpCaptureHelpers.h"
#include "OpResult.h"

#include "audio/AudioService.h"
#include "json/JsonValue.h"

#include <libop.h>

#include <string>

namespace {

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
    internal::set_result(ret, -4L);
    internal::guard_exceptions("AudioSetSessionVolume", [&]() {
        internal::set_result(ret, pid > 0 ? op::audio::SetSessionVolume(static_cast<unsigned long>(pid), volume)
                                          : -4);
    });
}

void op::Op::AudioSetSessionMute(long pid, long muted, long *ret) {
    internal::set_result(ret, -4L);
    internal::guard_exceptions("AudioSetSessionMute", [&]() {
        internal::set_result(ret, pid > 0 ? op::audio::SetSessionMute(static_cast<unsigned long>(pid), muted != 0)
                                          : -4);
    });
}

void op::Op::AudioGetSystemVolume(std::wstring &retjson) {
    retjson.clear();
    internal::guard_exceptions("AudioGetSystemVolume",
                               [&]() { build_system_json(op::audio::GetSystemVolume(), retjson); });
}

void op::Op::AudioSetSystemVolume(double volume, long *ret) {
    internal::set_result(ret, 0L);
    internal::guard_exceptions("AudioSetSystemVolume",
                               [&]() { internal::set_result(ret, op::audio::SetSystemVolume(volume)); });
}

void op::Op::AudioSetSystemMute(long muted, long *ret) {
    internal::set_result(ret, 0L);
    internal::guard_exceptions("AudioSetSystemMute",
                               [&]() { internal::set_result(ret, op::audio::SetSystemMute(muted != 0)); });
}
