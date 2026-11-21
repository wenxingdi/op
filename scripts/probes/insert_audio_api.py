# 音量模块三层导出插入脚本（2026-11-21）
# 用法：python scripts\probes\insert_audio_api.py
#
# 关键教训（本轮 YOLO 阶段踩过）：
#   C API 声明**必须插进 op_c_api.h 的 extern "C" 块内**，否则 x64 下变成 C++ 修饰名，
#   C++ 测试照常全绿但 Python/易语言调不到。本脚本显式在 `#ifdef __cplusplus } #endif`
#   **之前**插入，并在写入后校验位置。

import io
import os
import sys

REPO = r"D:\AutoPro\op-master\op"

LIBOP_H = os.path.join(REPO, "include", "libop.h")
OP_C_API_H = os.path.join(REPO, "include", "op_c_api.h")
OP_IDL = os.path.join(REPO, "libop", "com", "op.idl")
OP_AUDIO_CPP = os.path.join(REPO, "libop", "op", "OpAudio.cpp")
OP_C_API_CPP = os.path.join(REPO, "libop", "c_api", "op_c_api.cpp")
AUTOMATION_H = os.path.join(REPO, "libop", "com", "OpAutomation.h")
AUTOMATION_CPP = os.path.join(REPO, "libop", "com", "OpAutomation.cpp")
CMAKELISTS = os.path.join(REPO, "libop", "CMakeLists.txt")


def read(p):
    with io.open(p, "r", encoding="utf-8", newline="") as f:
        return f.read()


def nl_of(t):
    return "\r\n" if "\r\n" in t else "\n"


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


# ---------------------------------------------------------------------------
LIBOP_H_BLOCK = r'''
    // ===== 音频音量（2026-11-21，WASAPI 会话级 + 系统级）=====
    // 设计见 docs/2026-11/音量模块_方案_20261121.md。
    //
    // 语义区分（误用率最高，务必看清）：
    //   volume（滑块值）**与出声大小无直接关系**：滑块 1.0 也可能当前无声。
    //   判断"现在有没有声音"要看 **peak**。
    //   found=false = 该 PID 没有音频会话，**不等于没声音**（可能在放系统音，
    //   或进程刚启动还没建会话）。
    //
    // 返回 JSON：
    //   {"ok":1,"code":0,"found":true,"pid":12345,"muted":false,"playing":true,
    //    "volume":0.75,"peak":0.3421,"error":""}
    //   ok=1 表示查询链路走通（不代表有声音）；ok=0 看 error。
    //   code: 0 成功 / -1 无音频设备 / -2 COM 或激活失败 / -3 枚举失败 / -4 参数非法 / -5 设置失败
    //
    // 无状态：峰值保持/衰减/阈值判定留给调用方（每次调用重新枚举，不缓存 COM 指针）。
    void AudioQueryWindow(_In_ long long hwnd, _Out_ std::wstring &retjson);
    void AudioQueryPid(_In_ long pid, _Out_ std::wstring &retjson);
    // 纯取窗口所属 PID（不碰 COM）。ret: 失败返回 0。
    void AudioGetPidFromHwnd(_In_ long long hwnd, _Out_ long *ret);
    // 设置会话滑块音量（0.0~1.0，越界 clamp）。ret: 0 成功，负数为错误码。
    void AudioSetSessionVolume(_In_ long pid, _In_ double volume, _Out_ long *ret);
    void AudioSetSessionMute(_In_ long pid, _In_ long muted, _Out_ long *ret);
    // 系统主音量（同一套 enumerator 上多取 IAudioEndpointVolume，成本很低）。
    void AudioGetSystemVolume(_Out_ std::wstring &retjson);
    void AudioSetSystemVolume(_In_ double volume, _Out_ long *ret);
    void AudioSetSystemMute(_In_ long muted, _Out_ long *ret);
'''

OP_C_API_H_BLOCK = r'''
/* ===== 音频音量（2026-11-21，WASAPI 会话级 + 系统级）=====
 * 设计见 docs/2026-11/音量模块_方案_20261121.md。
 *
 * 语义区分（误用率最高）：
 *   volume（滑块值）与出声大小无直接关系；判断"有没有声音"要看 peak。
 *   found=false = 该 PID 无音频会话，不等于没声音。
 *
 * 返回 JSON：{"ok":1,"code":0,"found":true,"pid":123,"muted":false,"playing":true,
 *            "volume":0.75,"peak":0.34,"error":""}
 * code: 0 成功 / -1 无音频设备 / -2 COM 或激活失败 / -3 枚举失败 / -4 参数非法 / -5 设置失败
 *
 * 无状态（DLL 不缓存 COM 指针，每次重新枚举），峰值保持留给调用方。
 */
OP_C_API const wchar_t *OP_CALL OpAudioQueryWindow(op_handle handle, long long hwnd);
OP_C_API const wchar_t *OP_CALL OpAudioQueryPid(op_handle handle, int pid);
/* 纯取窗口所属 PID（不碰 COM）。失败返回 0。 */
OP_C_API int OP_CALL OpAudioGetPidFromHwnd(op_handle handle, long long hwnd);
/* 设置会话音量/静音。返回 0 成功，负数为错误码（见上）。 */
OP_C_API int OP_CALL OpAudioSetSessionVolume(op_handle handle, int pid, double volume);
OP_C_API int OP_CALL OpAudioSetSessionMute(op_handle handle, int pid, int muted);
/* 系统主音量 / 静音。 */
OP_C_API const wchar_t *OP_CALL OpAudioGetSystemVolume(op_handle handle);
OP_C_API int OP_CALL OpAudioSetSystemVolume(op_handle handle, double volume);
OP_C_API int OP_CALL OpAudioSetSystemMute(op_handle handle, int muted);
'''

OP_IDL_BLOCK = r'''
	// ---- 音频音量（2026-11-21，WASAPI 会话级 + 系统级）----
	// 设计见 docs/2026-11/音量模块_方案_20261121.md。
	// 语义区分：volume（滑块）与出声大小无直接关系；"有没有声音"看 peak。
	//           found=false = 该 PID 无音频会话，不等于没声音。
	// code: 0 成功 / -1 无音频设备 / -2 COM 或激活失败 / -3 枚举失败 / -4 参数非法 / -5 设置失败
	[id(456)] HRESULT AudioQueryWindow([in] LONG_PTR hwnd, [out, retval] BSTR* retstr);
	[id(457)] HRESULT AudioQueryPid([in] LONG pid, [out, retval] BSTR* retstr);
	[id(458)] HRESULT AudioGetPidFromHwnd([in] LONG_PTR hwnd, [out, retval] LONG* ret);
	[id(459)] HRESULT AudioSetSessionVolume([in] LONG pid, [in] DOUBLE volume, [out, retval] LONG* ret);
	[id(460)] HRESULT AudioSetSessionMute([in] LONG pid, [in] LONG muted, [out, retval] LONG* ret);
	[id(461)] HRESULT AudioGetSystemVolume([out, retval] BSTR* retstr);
	[id(462)] HRESULT AudioSetSystemVolume([in] DOUBLE volume, [out, retval] LONG* ret);
	[id(463)] HRESULT AudioSetSystemMute([in] LONG muted, [out, retval] LONG* ret);
'''

OP_AUDIO_CPP_BLOCK = r'''#include "OpContext.h"
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
'''

OP_C_API_CPP_BLOCK = r'''
/* ---- 音频音量（2026-11-21，WASAPI 会话级 + 系统级） ---- */

const wchar_t *OP_CALL OpAudioQueryWindow(op_handle handle, long long hwnd) {
    return call_string(handle, [&](op::Op &op, std::wstring &ret) { op.AudioQueryWindow(hwnd, ret); });
}

const wchar_t *OP_CALL OpAudioQueryPid(op_handle handle, int pid) {
    return call_string(handle, [&](op::Op &op, std::wstring &ret) { op.AudioQueryPid(pid, ret); });
}

int OP_CALL OpAudioGetPidFromHwnd(op_handle handle, long long hwnd) {
    return call_ret(handle, [&](op::Op &op, long *ret) { op.AudioGetPidFromHwnd(hwnd, ret); });
}

int OP_CALL OpAudioSetSessionVolume(op_handle handle, int pid, double volume) {
    return call_ret(handle, [&](op::Op &op, long *ret) { op.AudioSetSessionVolume(pid, volume, ret); });
}

int OP_CALL OpAudioSetSessionMute(op_handle handle, int pid, int muted) {
    return call_ret(handle, [&](op::Op &op, long *ret) { op.AudioSetSessionMute(pid, muted, ret); });
}

const wchar_t *OP_CALL OpAudioGetSystemVolume(op_handle handle) {
    return call_string(handle, [](op::Op &op, std::wstring &ret) { op.AudioGetSystemVolume(ret); });
}

int OP_CALL OpAudioSetSystemVolume(op_handle handle, double volume) {
    return call_ret(handle, [&](op::Op &op, long *ret) { op.AudioSetSystemVolume(volume, ret); });
}

int OP_CALL OpAudioSetSystemMute(op_handle handle, int muted) {
    return call_ret(handle, [&](op::Op &op, long *ret) { op.AudioSetSystemMute(muted, ret); });
}
'''

AUTOMATION_H_BLOCK = r'''
    // 音频音量（2026-11-21，WASAPI 会话级 + 系统级；idl id 456-463）
    STDMETHOD(AudioQueryWindow)(LONG_PTR hwnd, BSTR *retstr);
    STDMETHOD(AudioQueryPid)(LONG pid, BSTR *retstr);
    STDMETHOD(AudioGetPidFromHwnd)(LONG_PTR hwnd, LONG *ret);
    STDMETHOD(AudioSetSessionVolume)(LONG pid, DOUBLE volume, LONG *ret);
    STDMETHOD(AudioSetSessionMute)(LONG pid, LONG muted, LONG *ret);
    STDMETHOD(AudioGetSystemVolume)(BSTR *retstr);
    STDMETHOD(AudioSetSystemVolume)(DOUBLE volume, LONG *ret);
    STDMETHOD(AudioSetSystemMute)(LONG muted, LONG *ret);
'''

AUTOMATION_CPP_BLOCK = r'''
// ===== 音频音量（2026-11-21，WASAPI 会话级 + 系统级）=====

STDMETHODIMP OpAutomation::AudioQueryWindow(LONG_PTR hwnd, BSTR *retstr) {
    if (!retstr)
        return E_POINTER;
    wstring s;
    obj.AudioQueryWindow(static_cast<long long>(hwnd), s);
    return CopyOutBstr(retstr, s);
}

STDMETHODIMP OpAutomation::AudioQueryPid(LONG pid, BSTR *retstr) {
    if (!retstr)
        return E_POINTER;
    wstring s;
    obj.AudioQueryPid(pid, s);
    return CopyOutBstr(retstr, s);
}

STDMETHODIMP OpAutomation::AudioGetPidFromHwnd(LONG_PTR hwnd, LONG *ret) {
    if (!ret)
        return E_POINTER;
    *ret = 0;
    obj.AudioGetPidFromHwnd(static_cast<long long>(hwnd), ret);
    return S_OK;
}

STDMETHODIMP OpAutomation::AudioSetSessionVolume(LONG pid, DOUBLE volume, LONG *ret) {
    if (!ret)
        return E_POINTER;
    *ret = -4;
    obj.AudioSetSessionVolume(pid, static_cast<double>(volume), ret);
    return S_OK;
}

STDMETHODIMP OpAutomation::AudioSetSessionMute(LONG pid, LONG muted, LONG *ret) {
    if (!ret)
        return E_POINTER;
    *ret = -4;
    obj.AudioSetSessionMute(pid, muted, ret);
    return S_OK;
}

STDMETHODIMP OpAutomation::AudioGetSystemVolume(BSTR *retstr) {
    if (!retstr)
        return E_POINTER;
    wstring s;
    obj.AudioGetSystemVolume(s);
    return CopyOutBstr(retstr, s);
}

STDMETHODIMP OpAutomation::AudioSetSystemVolume(DOUBLE volume, LONG *ret) {
    if (!ret)
        return E_POINTER;
    *ret = 0;
    obj.AudioSetSystemVolume(static_cast<double>(volume), ret);
    return S_OK;
}

STDMETHODIMP OpAutomation::AudioSetSystemMute(LONG muted, LONG *ret) {
    if (!ret)
        return E_POINTER;
    *ret = 0;
    obj.AudioSetSystemMute(muted, ret);
    return S_OK;
}
'''


def insert_before_marker(path, marker, block, must_contain, min_lines, after=False):
    text = read(path)
    nl = nl_of(text)
    body = block.replace("\r\n", "\n").replace("\n", nl)
    if text.count("\n") < min_lines:
        print("  FAIL truncated:", os.path.basename(path))
        return False
    cnt = text.count(marker)
    if cnt != 1:
        print("  FAIL marker count =", cnt, "in", os.path.basename(path))
        return False
    idx = text.index(marker)
    pos = idx + len(marker) if after else idx
    return safe_write(path, text[:pos] + body + text[pos:], must_contain, min_lines)


def main():
    ok = True

    print("[1/8] include/libop.h")
    ok &= insert_before_marker(
        LIBOP_H,
        "    // 图像句柄注册表（2026-11-21，内存直操作一期地基）",
        LIBOP_H_BLOCK,
        ["AudioQueryWindow", "AudioSetSystemMute"],
        700,
    )

    print("[2/8] include/op_c_api.h  (MUST be inside extern \"C\")")
    # 关键：插在 #ifdef __cplusplus } #endif 之前，保证在 extern "C" 块内
    marker = "#ifdef __cplusplus" + nl_of(read(OP_C_API_H)) + "}" + nl_of(read(OP_C_API_H)) + "#endif"
    text = read(OP_C_API_H)
    nl = nl_of(text)
    marker = "#ifdef __cplusplus" + nl + "}" + nl + "#endif"
    if text.count(marker) != 1:
        print("  FAIL: extern C tail marker count =", text.count(marker))
        ok = False
    else:
        body = OP_C_API_H_BLOCK.replace("\r\n", "\n").replace("\n", nl)
        new_text = text.replace(marker, body + marker, 1)
        ok &= safe_write(OP_C_API_H, new_text, ["OpAudioQueryWindow", "OpAudioSetSystemMute"])
        # 校验位置：声明必须在 extern "C" 闭合之前
        got = read(OP_C_API_H)
        pos_decl = got.index("OpAudioQueryWindow")
        pos_tail = got.index(marker)
        inside = pos_decl < pos_tail
        print("  declaration inside extern C block:", inside)
        ok &= inside

    print("[3/8] libop/com/op.idl")
    ok &= insert_before_marker(
        OP_IDL,
        "\t[id(455)] HRESULT YoloDetectFromFileEx2(",
        OP_IDL_BLOCK,
        ["AudioQueryWindow", "id(463)"],
        300,
    )

    print("[4/8] libop/op/OpAudio.cpp (new file)")
    if not os.path.exists(OP_AUDIO_CPP):
        with io.open(OP_AUDIO_CPP, "w", encoding="utf-8", newline="") as f:
            f.write(OP_AUDIO_CPP_BLOCK.replace("\r\n", "\n"))
        print("  created")
    else:
        print("  SKIP exists")

    print("[5/8] libop/CMakeLists.txt (add OpAudio.cpp)")
    cm = read(CMAKELISTS)
    nl2 = nl_of(cm)
    anchor = '"op/OpYolo.cpp"'
    if anchor not in cm:
        # 回退：找任意 op/Op*.cpp
        anchor = None
    if anchor and "op/OpAudio.cpp" not in cm:
        cm = cm.replace(anchor, anchor + nl2 + '  # \u97f3\u9891\u97f3\u91cf\uff082026-11-21\uff09' + nl2 + '  "op/OpAudio.cpp"', 1)
        ok &= safe_write(CMAKELISTS, cm, ["op/OpAudio.cpp"])
    elif "op/OpAudio.cpp" in cm:
        print("  SKIP already present")
    else:
        print("  FAIL: anchor for OpAudio.cpp not found")
        ok = False

    print("[6/8] libop/c_api/op_c_api.cpp (append inside extern \"C\")")
    cpp = read(OP_C_API_CPP)
    nl3 = nl_of(cpp)
    body = OP_C_API_CPP_BLOCK.replace("\r\n", "\n").replace("\n", nl3)
    ok &= safe_write(OP_C_API_CPP, cpp + body, ["OpAudioQueryWindow", "OpAudioSetSystemMute"], 1000)
    # 实现也必须补 extern "C" 包裹（追加在文件末尾会脱离已有链接规格）
    fixed = read(OP_C_API_CPP)
    key = 'const wchar_t *OP_CALL OpAudioQueryWindow('
    i = fixed.index(key)
    head, tail = fixed[:i], fixed[i:]
    if 'extern "C" {' in head[-40:] or True:
        newfixed = head.rstrip() + nl3 + nl3 + 'extern "C" {' + nl3 + tail.rstrip() + nl3 + '}' + nl3
        safe_write(OP_C_API_CPP, newfixed, ["OpAudioQueryWindow", 'extern "C" {'])

    print("[7/8] OpAutomation.h")
    ok &= insert_before_marker(
        AUTOMATION_H,
        "    //-----------------------opcv---------------------------------",
        AUTOMATION_H_BLOCK,
        ["STDMETHOD(AudioQueryWindow)", "STDMETHOD(AudioSetSystemMute)"],
        400,
    )

    print("[8/8] OpAutomation.cpp (append)")
    cpp2 = read(AUTOMATION_CPP)
    nl4 = nl_of(cpp2)
    ok &= safe_write(
        AUTOMATION_CPP,
        cpp2 + AUTOMATION_CPP_BLOCK.replace("\r\n", "\n").replace("\n", nl4),
        ["OpAutomation::AudioQueryWindow", "OpAutomation::AudioSetSystemMute"],
        1000,
    )

    print("\nRESULT:", "OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
