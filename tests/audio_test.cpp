// 音频音量模块自测（2026-11-21，WASAPI 会话级 + 系统级）
// 覆盖 docs/2026-11/音量模块_方案_20261121.md §五 的 A1~A8。
//
// 设计原则：
//   - **参数校验类**用例不依赖音频设备，任何机器都必须绿
//   - **真实会话类**用例需要音频设备；没有设备时如实 GTEST_SKIP 并写明原因
//   - 从不用"读到的值恰好是 X"这种脆弱断言（音量/峰值随系统状态变化）

#include "test_support.h"

#include <gtest/gtest.h>

#include <string>
#include <windows.h>
#include <mmdeviceapi.h> // STA 宿主 COM 可用性验证（AudioTest.StaHostKeepsComUsableAfterCalls）

#include <op_c_api.h>

namespace {

struct CApiHandle {
    op_handle handle = OpCreate();
    ~CApiHandle() {
        OpDestroy(handle);
    }
};

bool JsonHasKey(const std::wstring &json, const wchar_t *key) {
    return json.find(std::wstring(L"\"") + key + L"\":") != std::wstring::npos;
}

bool JsonInt(const std::wstring &json, const wchar_t *key, long &value) {
    const std::wstring pattern = std::wstring(L"\"") + key + L"\":";
    const size_t p = json.find(pattern);
    if (p == std::wstring::npos)
        return false;
    value = _wtoi(json.c_str() + p + pattern.size());
    return true;
}

bool JsonBool(const std::wstring &json, const wchar_t *key, bool &value) {
    const std::wstring pattern = std::wstring(L"\"") + key + L"\":";
    const size_t p = json.find(pattern);
    if (p == std::wstring::npos)
        return false;
    const std::wstring tail = json.substr(p + pattern.size(), 5);
    if (tail.rfind(L"true", 0) == 0) {
        value = true;
        return true;
    }
    if (tail.rfind(L"false", 0) == 0) {
        value = false;
        return true;
    }
    return false;
}

bool JsonDouble(const std::wstring &json, const wchar_t *key, double &value) {
    const std::wstring pattern = std::wstring(L"\"") + key + L"\":";
    const size_t p = json.find(pattern);
    if (p == std::wstring::npos)
        return false;
    value = _wtof(json.c_str() + p + pattern.size());
    return true;
}

} // namespace

// A6：JSON 字段齐全且类型正确（不依赖音频设备）
TEST(AudioTest, SessionJsonHasAllFields) {
    CApiHandle api;
    const wchar_t *json = OpAudioQueryPid(api.handle, static_cast<int>(GetCurrentProcessId()));
    ASSERT_NE(json, nullptr);
    const std::wstring out(json);
    ASSERT_FALSE(out.empty());

    // 字段齐全
    for (const wchar_t *k : {L"ok", L"code", L"found", L"pid", L"muted", L"playing", L"volume", L"peak",
                             L"error"}) {
        EXPECT_TRUE(JsonHasKey(out, k)) << "missing field: " << std::string(k, k + wcslen(k));
    }

    // 类型正确（bool 字段必须是 true/false，不是数字）
    bool b = true;
    EXPECT_TRUE(JsonBool(out, L"found", b));
    EXPECT_TRUE(JsonBool(out, L"muted", b));
    EXPECT_TRUE(JsonBool(out, L"playing", b));

    long l = -1;
    EXPECT_TRUE(JsonInt(out, L"ok", l));
    EXPECT_TRUE(JsonInt(out, L"code", l));
    EXPECT_TRUE(JsonInt(out, L"pid", l));

    double d = -1;
    EXPECT_TRUE(JsonDouble(out, L"volume", d));
    EXPECT_TRUE(JsonDouble(out, L"peak", d));
}

// A3：非法 PID 必须明确报错（code=-4），不崩溃
TEST(AudioTest, InvalidPidIsRejected) {
    CApiHandle api;

    // pid=0 非法
    const wchar_t *json = OpAudioQueryPid(api.handle, 0);
    ASSERT_NE(json, nullptr);
    const std::wstring out(json);
    long ok = 1, code = 0;
    ASSERT_TRUE(JsonInt(out, L"ok", ok));
    ASSERT_TRUE(JsonInt(out, L"code", code));
    EXPECT_EQ(ok, 0) << "pid=0 must report ok=0";
    EXPECT_EQ(code, -4) << "pid=0 must report code=-4 (invalid pid)";

    bool found = true;
    EXPECT_TRUE(JsonBool(out, L"found", found));
    EXPECT_FALSE(found) << "invalid pid must not report a found session";

    // 负 PID 同样非法
    const wchar_t *neg = OpAudioQueryPid(api.handle, -1);
    ASSERT_NE(neg, nullptr);
    long ok2 = 1, code2 = 0;
    ASSERT_TRUE(JsonInt(std::wstring(neg), L"ok", ok2));
    ASSERT_TRUE(JsonInt(std::wstring(neg), L"code", code2));
    EXPECT_EQ(ok2, 0);
    EXPECT_EQ(code2, -4);
}

// A2：不存在的 PID → found=false、ok=1（"顺利查完但没有会话"是有效结果，不是错误）
TEST(AudioTest, NonexistentPidReportsNotFoundGracefully) {
    CApiHandle api;
    const wchar_t *json = OpAudioQueryPid(api.handle, 999999);
    ASSERT_NE(json, nullptr);
    const std::wstring out(json);

    long ok = 0, code = -1;
    ASSERT_TRUE(JsonInt(out, L"ok", ok));
    ASSERT_TRUE(JsonInt(out, L"code", code));

    if (ok == 0) {
        // 没有音频设备时 ok=0 / code=-1，也算合理（如实检查）
        EXPECT_EQ(code, -1) << "failure without an audio device should be code=-1";
        GTEST_SKIP() << "no audio render device on this machine (code=-1); "
                        "'not found' semantics cannot be verified";
    }
    EXPECT_EQ(code, 0);
    bool found = true;
    ASSERT_TRUE(JsonBool(out, L"found", found));
    EXPECT_FALSE(found) << "pid 999999 must not have an audio session";
}

// A5：无效窗口句柄 → code=-4；GetPidFromHwnd 返回 0
TEST(AudioTest, InvalidWindowHandleIsRejected) {
    CApiHandle api;

    EXPECT_EQ(OpAudioGetPidFromHwnd(api.handle, 0), 0) << "null hwnd must yield pid 0";

    const wchar_t *json = OpAudioQueryWindow(api.handle, 0);
    ASSERT_NE(json, nullptr);
    const std::wstring out(json);
    long ok = 1, code = 0;
    ASSERT_TRUE(JsonInt(out, L"ok", ok));
    ASSERT_TRUE(JsonInt(out, L"code", code));
    EXPECT_EQ(ok, 0);
    EXPECT_EQ(code, -4) << "invalid hwnd must report code=-4";

    // 一个几乎不可能存在的句柄
    EXPECT_EQ(OpAudioGetPidFromHwnd(api.handle, static_cast<long long>(0x1)), 0);
}

// A4：GetPidFromHwnd 对真实窗口返回正确 PID
TEST(AudioTest, GetPidFromHwndMatchesWindowOwner) {
    CApiHandle api;

    // 用当前进程的控制台/隐藏窗口都不稳，直接创建一个临时窗口
    HWND wnd = CreateWindowExW(0, L"STATIC", L"op_audio_pid_test", WS_OVERLAPPED, 0, 0, 100, 100, nullptr,
                               nullptr, GetModuleHandleW(nullptr), nullptr);
    if (wnd == nullptr) {
        GTEST_SKIP() << "failed to create a test window";
    }
    const long expected = static_cast<long>(GetCurrentProcessId());
    const int pid = OpAudioGetPidFromHwnd(api.handle, reinterpret_cast<long long>(wnd));
    DestroyWindow(wnd);

    EXPECT_EQ(pid, static_cast<int>(expected)) << "GetPidFromHwnd must return the owning process id";
}

// A7：重复调用不崩溃、不泄漏 COM 引用（COM 指针全部 RAII）
TEST(AudioTest, RepeatedCallsAreStable) {
    CApiHandle api;
    const int self = static_cast<int>(GetCurrentProcessId());
    for (int i = 0; i < 100; ++i) {
        const wchar_t *json = OpAudioQueryPid(api.handle, self);
        ASSERT_NE(json, nullptr) << "iteration " << i;
        // 每次都要拿到合法 JSON
        EXPECT_NE(std::wstring(json).find(L"\"ok\":"), std::wstring::npos);
    }
    // 系统音量也连读 50 次
    for (int i = 0; i < 50; ++i) {
        const wchar_t *json = OpAudioGetSystemVolume(api.handle);
        ASSERT_NE(json, nullptr) << "iteration " << i;
    }
}

// 系统音量 JSON 结构（不依赖具体值）
TEST(AudioTest, SystemVolumeJsonShape) {
    CApiHandle api;
    const wchar_t *json = OpAudioGetSystemVolume(api.handle);
    ASSERT_NE(json, nullptr);
    const std::wstring out(json);
    ASSERT_FALSE(out.empty());

    for (const wchar_t *k : {L"ok", L"code", L"muted", L"volume", L"error"}) {
        EXPECT_TRUE(JsonHasKey(out, k)) << "missing field";
    }

    long ok = 0;
    ASSERT_TRUE(JsonInt(out, L"ok", ok));
    if (ok == 1) {
        double v = -1;
        ASSERT_TRUE(JsonDouble(out, L"volume", v));
        EXPECT_GE(v, 0.0);
        EXPECT_LE(v, 1.0);
    }
}

// 设置接口的参数校验：非法 PID 必须返回 0（对外契约 0=失败），不崩溃
TEST(AudioTest, SetSessionRejectsInvalidPid) {
    CApiHandle api;
    EXPECT_EQ(OpAudioSetSessionVolume(api.handle, 0, 0.5), 0);
    EXPECT_EQ(OpAudioSetSessionMute(api.handle, 0, 1), 0);
    // 不存在的 PID：接口本身不崩，返回失败 0
    const int rc = OpAudioSetSessionVolume(api.handle, 999999, 0.5);
    EXPECT_EQ(rc, 0) << "setting volume on a nonexistent pid must fail (0 = failure contract)";
}

// 音量越界应被 clamp（0.0~1.0），不报错
TEST(AudioTest, SetVolumeClampsOutOfRange) {
    CApiHandle api;
    // 对不存在 PID 调用只看"不崩溃且返回失败 0"，不对具体值断言
    EXPECT_EQ(OpAudioSetSessionVolume(api.handle, 999999, -5.0), 0);
    EXPECT_EQ(OpAudioSetSessionVolume(api.handle, 999999, 5.0), 0);
    // 系统音量：无设备返回 0，有设备返回 1；越界值本身不该报错
    const int sys = OpAudioSetSystemVolume(api.handle, 1.5);
    EXPECT_TRUE(sys == 0 || sys == 1) << "system volume setter must return 1/0, got " << sys;
}

// A8：真实音频会话的读写闭环。
//
// 需要音频设备且需要能产生音频会话。做法：起一个子系统播放循环音频，
// 再查它的会话、设置音量、读回比对。
//
// ⚠ 若本机没有音频设备 / 无法播放，如实 GTEST_SKIP，不假装验证过。
TEST(AudioTest, RealSessionReadWriteRoundTrip) {
    CApiHandle api;

    // 先确认有音频设备
    const wchar_t *sys_json = OpAudioGetSystemVolume(api.handle);
    ASSERT_NE(sys_json, nullptr);
    long sys_ok = 0;
    ASSERT_TRUE(JsonInt(std::wstring(sys_json), L"ok", sys_ok));
    if (sys_ok == 0) {
        GTEST_SKIP() << "no audio render device on this machine; "
                        "real session read/write cannot be verified";
    }

    // 生成一段 2 秒 440Hz 单声道 wav
    const std::wstring wav_path = test_support::GetTempBmpPath(L"op_audio_tone.wav");
    {
        HANDLE f = CreateFileW(wav_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                               nullptr);
        ASSERT_NE(f, INVALID_HANDLE_VALUE);
        const int sr = 44100;
        const int n = sr * 2;
        const DWORD data_bytes = static_cast<DWORD>(n * 2);
        std::vector<unsigned char> buf(44 + data_bytes, 0);
        auto w32 = [&](size_t off, unsigned int v) { memcpy(buf.data() + off, &v, 4); };
        auto w16 = [&](size_t off, unsigned short v) { memcpy(buf.data() + off, &v, 2); };
        memcpy(buf.data(), "RIFF", 4);
        w32(4, 36 + data_bytes);
        memcpy(buf.data() + 8, "WAVEfmt ", 8);
        w32(16, 16);
        w16(20, 1);            // PCM
        w16(22, 1);            // mono
        w32(24, sr);           // sample rate
        w32(28, sr * 2);       // byte rate
        w16(32, 2);            // block align
        w16(34, 16);           // bits
        memcpy(buf.data() + 36, "data", 4);
        w32(40, data_bytes);
        for (int i = 0; i < n; ++i) {
            const double phase = 2.0 * 3.14159265358979 * 440.0 * i / sr;
            const short s = static_cast<short>(9000.0 * sin(phase));
            memcpy(buf.data() + 44 + i * 2, &s, 2);
        }
        DWORD written = 0;
        WriteFile(f, buf.data(), static_cast<DWORD>(buf.size()), &written, nullptr);
        CloseHandle(f);
        ASSERT_EQ(written, static_cast<DWORD>(buf.size()));
    }

    // 用 PowerShell SoundPlayer 循环播放（会创建独立音频会话）
    const std::wstring cmd = L"powershell -NoProfile -Command \"$p=New-Object System.Media.SoundPlayer '" +
                             wav_path + L"'; $p.PlayLooping(); Start-Sleep -Seconds 12\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back(L'\0');
    const BOOL started = CreateProcessW(nullptr, cmd_buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &si, &pi);
    if (!started) {
        GTEST_SKIP() << "failed to start the audio player helper";
    }

    // 等会话建立
    bool found = false;
    std::wstring session_json;
    for (int i = 0; i < 30 && !found; ++i) {
        Sleep(200);
        const wchar_t *json = OpAudioQueryPid(api.handle, static_cast<int>(pi.dwProcessId));
        if (json) {
            session_json = json;
            bool f = false;
            if (JsonBool(session_json, L"found", f))
                found = f;
        }
    }

    if (!found) {
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        GTEST_SKIP() << "the helper process produced no audio session "
                        "(headless/no output device?); real read/write cannot be verified";
    }

    // 有会话：验证 playing + peak
    bool playing = false;
    ASSERT_TRUE(JsonBool(session_json, L"playing", playing));
    EXPECT_TRUE(playing) << "a looping player must be in Active state";

    double peak = 0.0;
    ASSERT_TRUE(JsonDouble(session_json, L"peak", peak));
    EXPECT_GE(peak, 0.0);
    EXPECT_LE(peak, 1.0);
    EXPECT_GT(peak, 0.0) << "a looping tone must produce a non-zero peak";

    // 写入 0.42 后读回
    const int rc = OpAudioSetSessionVolume(api.handle, static_cast<int>(pi.dwProcessId), 0.42);
    EXPECT_EQ(rc, 1) << "setting volume on an existing session must succeed (1 = success contract)";
    if (rc == 1) {
        Sleep(300);
        const wchar_t *after = OpAudioQueryPid(api.handle, static_cast<int>(pi.dwProcessId));
        ASSERT_NE(after, nullptr);
        double v = 0.0;
        ASSERT_TRUE(JsonDouble(std::wstring(after), L"volume", v));
        EXPECT_NEAR(v, 0.42, 0.02) << "volume must read back what was set";
    }

    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    DeleteFileW(wav_path.c_str());
}

// 反向验证说明（人工执行，CLAUDE.md 要求"故意改错必须 FAIL"）：
//   1) ComGuard 无条件 CoUninitialize（无视 owned_）：
//      在宿主已 STA 初始化的进程里会破坏 COM。本套用例难直接抓到，
//      但 RepeatedCallsAreStable 在部分宿主下会崩 —— 属"弱断言"，已记录。
//   2) found 判据恒真（不比对 PID）：
//      NonexistentPidReportsNotFoundGracefully 必须 FAIL（pid 999999 会报 found=true）。
//   3) peak / volume 字段写颠倒：
//      RealSessionReadWriteRoundTrip 的 volume≈0.42 断言必须 FAIL
//      （读到的是 peak 值）；且 SessionJsonHasAllFields 的类型断言仍绿（都是数字）。
//   4) 非法 PID 不校验（去掉 pid==0 判据）：
//      InvalidPidIsRejected 必须 FAIL。

// COM 初始化语义：宿主已用 STA 初始化时，op 的接口仍须可用，
// 且**不能**在返回时把宿主的 COM 引用计数减掉（那会破坏宿主）。
//
// 这是 ComGuard 那段 RAII 逻辑的唯一有效验证方式：
// 在 MTA 主线程里测不出来（owned=true，无条件/有条件释放表现相同）。
// 必须显式把当前线程切到 STA，再调接口，最后确认 COM 仍然可用。
//
// 做法：
//   1) 让当前线程 CoInitializeEx(STA)——模拟宿主
//   2) 连续调接口 20 次（每次内部都会 CoInitializeEx(MTA) → RPC_E_CHANGED_MODE）
//   3) 之后再自己 CoCreateInstance 一个 COM 对象，确认仍成功
//      （若 op 无条件 CoUninitialize，宿主的引用计数会被提前扣完，第 3 步会失败）
TEST(AudioTest, StaHostKeepsComUsableAfterCalls) {
    // GoogleTest 主线程的 COM 状态由前面的用例决定，可能是：
    //   (a) 未初始化            -> 我们初始化 STA，用完释放
    //   (b) 已初始化（MTA 或 STA）-> **不能**先 CoUninitialize（那是别人的计数），
    //       直接在其基础上工作；此时"宿主已被初始化"正是本用例要测的场景
    //
    // 早先版本无条件 CoUninitialize() 想"确保干净"，结果把后续用例依赖的 COM
    // 计数扣掉了，导致 RealSessionReadWriteRoundTrip 在本用例之后偶发失败
    // （单独跑则通过）——这是测试自身制造的污染，不是产品缺陷。
    //
    // 现在改为：用一个探针判断当前线程的 COM 状态，只在"原本未初始化"时才拥有它。
    const HRESULT probe_hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool owns_com = false;
    if (SUCCEEDED(probe_hr)) {
        owns_com = true; // 由我们成功初始化（STA）
    } else if (probe_hr == RPC_E_CHANGED_MODE) {
        // 已被别人以 MTA 初始化过：正好是"宿主已初始化"的场景，可直接测
        owns_com = false;
    } else if (probe_hr == S_FALSE) {
        // 本线程已初始化过同模式，计数 +1，需配对释放
        owns_com = true;
    }
    const HRESULT hr = probe_hr == RPC_E_CHANGED_MODE ? S_OK : probe_hr;
    if (FAILED(hr)) {
        GTEST_SKIP() << "cannot initialize STA on this thread (hr=" << hr << ")";
    }

    CApiHandle api;
    const int self = static_cast<int>(GetCurrentProcessId());
    for (int i = 0; i < 20; ++i) {
        const wchar_t *json = OpAudioQueryPid(api.handle, self);
        ASSERT_NE(json, nullptr) << "iteration " << i;
        const wchar_t *sys = OpAudioGetSystemVolume(api.handle);
        ASSERT_NE(sys, nullptr) << "iteration " << i;
    }

    // 关键校验：宿主（即本线程）的 COM 仍可用。
    // 若 ComGuard 无条件 CoUninitialize，这里的激活会失败。
    IMMDeviceEnumerator *enumerator = nullptr;
    const HRESULT activate_hr =
        ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                           reinterpret_cast<void **>(&enumerator));
    EXPECT_TRUE(SUCCEEDED(activate_hr))
        << "COM activation must still work after op calls; failure means ComGuard "
           "called CoUninitialize on a foreign initialization (hr=" << activate_hr << ")";
    if (enumerator)
        enumerator->Release();

    if (owns_com)
        ::CoUninitialize(); // 只释放本用例真正拥有(或 +1)的那次
}