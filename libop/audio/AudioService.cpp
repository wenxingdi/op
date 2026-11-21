// 音频音量服务实现（2026-11-21，WASAPI 会话级 + 系统级）。
// 设计见 docs/2026-11/音量模块_方案_20261121.md。
//
// 错误码约定（同时用于 SessionInfo.code / 返回值）：
//    0 成功
//   -1 无默认音频渲染设备（GetDefaultAudioEndpoint 失败）
//   -2 创建 IAudioSessionManager2 失败
//   -3 枚举会话失败
//   -4 传入 PID/HWND 非法
//   -5 设置音量/静音失败
//
// 本文件刻意**不缓存任何 COM 指针**：每次调用重新枚举设备与会话。
// 理由：op 是插件，宿主进程可能长时间运行；游戏/应用重启后 PID 变化，
// 缓存会话指针会指向已释放对象（规划文档里 Python 版踩过的真机坑之一）。
// 枚举成本实测 <2ms，远比缓存失效的调试成本低。

#include "AudioService.h"

#include <Windows.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <atomic>
#include <string>

namespace op::audio {
namespace {

constexpr const wchar_t *kErrNoDevice = L"no default audio render device";
constexpr const wchar_t *kErrNoManager = L"failed to activate IAudioSessionManager2";
constexpr const wchar_t *kErrEnumFailed = L"failed to enumerate audio sessions";
constexpr const wchar_t *kErrBadPid = L"invalid pid";
constexpr const wchar_t *kErrBadHwnd = L"invalid window handle";
constexpr const wchar_t *kErrSetFailed = L"failed to set volume/mute";

// COM 初始化 RAII。
//
// 必须接受 RPC_E_CHANGED_MODE：op 是**插件**，宿主可能已经在 STA 里初始化过 COM
// （例如某些易语言/Python 宿主）。此时 CoInitializeEx(MTA) 返回该码，
// 我们**可以直接使用 COM**，但**不能 CoUninitialize**（那一次不是我们初始化的，
// 释放会破坏宿主的 COM 状态）。
// 所以 owned 只在 SUCCEEDED(hr) 时为真，析构时仅释放自己成功的那一次。
class ComGuard {
  public:
    ComGuard() {
        const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        owned_ = SUCCEEDED(hr);
        usable_ = owned_ || hr == RPC_E_CHANGED_MODE;
    }
    ~ComGuard() {
        if (owned_)
            ::CoUninitialize();
    }
    ComGuard(const ComGuard &) = delete;
    ComGuard &operator=(const ComGuard &) = delete;
    bool usable() const { return usable_; }

  private:
    bool owned_ = false;
    bool usable_ = false;
};

// 轻量 CComPtr 替代（本仓库没有 ATL 依赖，且 ATL 在部分构建配置下不可用）。
// 只做最小 RAII：持有一个接口指针，析构 Release，AddRef 由调用方负责。
template <typename T> class ComPtr {
  public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr &) = delete;
    ComPtr &operator=(const ComPtr &) = delete;

    T **put() {
        reset();
        return &ptr_;
    }
    T *get() const { return ptr_; }
    T *operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

    void reset() {
        if (ptr_) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

  private:
    T *ptr_ = nullptr;
};

// 打开默认渲染设备的会话管理器。失败返回 false 并写 code/error。
bool OpenSessionManager(ComPtr<IMMDeviceEnumerator> &enumerator, ComPtr<IMMDevice> &device,
                        ComPtr<IAudioSessionManager2> &manager, int &code, std::wstring &error) {
    HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(enumerator.put()));
    if (FAILED(hr) || !enumerator) {
        code = -1;
        error = kErrNoDevice;
        return false;
    }

    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.put());
    if (FAILED(hr) || !device) {
        code = -1;
        error = kErrNoDevice;
        return false;
    }

    hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void **>(manager.put()));
    if (FAILED(hr) || !manager) {
        code = -2;
        error = kErrNoManager;
        return false;
    }
    return true;
}

// 在给定管理器里按 PID 找会话，同时取到音量与峰值接口。
// 找不到时 found=false（不是错误）。
bool FindSessionForPid(IAudioSessionManager2 *manager, unsigned long pid, bool &found, bool &muted, bool &playing,
                       double &volume, double &peak) {
    found = muted = playing = false;
    volume = peak = 0.0;

    ComPtr<IAudioSessionEnumerator> sessions;
    if (FAILED(manager->GetSessionEnumerator(sessions.put())) || !sessions)
        return false;

    int count = 0;
    if (FAILED(sessions->GetCount(&count)) || count <= 0)
        return true; // 枚举成功但没有任何会话：found=false 是有效结果

    for (int i = 0; i < count; ++i) {
        ComPtr<IAudioSessionControl> control;
        if (FAILED(sessions->GetSession(i, control.put())) || !control)
            continue;

        ComPtr<IAudioSessionControl2> control2;
        if (FAILED(control->QueryInterface(__uuidof(IAudioSessionControl2),
                                           reinterpret_cast<void **>(control2.put()))) ||
            !control2)
            continue;

        DWORD session_pid = 0;
        if (FAILED(control2->GetProcessId(&session_pid)))
            continue;
        if (session_pid != pid)
            continue;

        // 命中该 PID 的会话
        found = true;

        AudioSessionState state = AudioSessionStateInactive;
        if (SUCCEEDED(control->GetState(&state)))
            playing = (state == AudioSessionStateActive);

        ComPtr<ISimpleAudioVolume> simple;
        if (SUCCEEDED(control->QueryInterface(__uuidof(ISimpleAudioVolume),
                                              reinterpret_cast<void **>(simple.put()))) &&
            simple) {
            BOOL m = FALSE;
            if (SUCCEEDED(simple->GetMute(&m)))
                muted = (m != FALSE);
            float v = 0.0f;
            if (SUCCEEDED(simple->GetMasterVolume(&v)))
                volume = static_cast<double>(v);
        }

        ComPtr<IAudioMeterInformation> meter;
        if (SUCCEEDED(control->QueryInterface(__uuidof(IAudioMeterInformation),
                                              reinterpret_cast<void **>(meter.put()))) &&
            meter) {
            float p = 0.0f;
            if (SUCCEEDED(meter->GetPeakValue(&p)))
                peak = static_cast<double>(p);
        }
        return true;
    }
    return true;
}

// 按 PID 拿到可写的 ISimpleAudioVolume。成功返回 0，否则负 code。
int OpenSessionVolume(unsigned long pid, ComPtr<ISimpleAudioVolume> &out) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioSessionManager2> manager;
    int code = 0;
    std::wstring error;
    if (!OpenSessionManager(enumerator, device, manager, code, error))
        return code;

    ComPtr<IAudioSessionEnumerator> sessions;
    if (FAILED(manager->GetSessionEnumerator(sessions.put())) || !sessions)
        return -3;
    int count = 0;
    if (FAILED(sessions->GetCount(&count)) || count <= 0)
        return -3;

    for (int i = 0; i < count; ++i) {
        ComPtr<IAudioSessionControl> control;
        if (FAILED(sessions->GetSession(i, control.put())) || !control)
            continue;
        ComPtr<IAudioSessionControl2> control2;
        if (FAILED(control->QueryInterface(__uuidof(IAudioSessionControl2),
                                           reinterpret_cast<void **>(control2.put()))) ||
            !control2)
            continue;
        DWORD session_pid = 0;
        if (FAILED(control2->GetProcessId(&session_pid)))
            continue;
        if (session_pid != pid)
            continue;
        if (FAILED(control->QueryInterface(__uuidof(ISimpleAudioVolume),
                                           reinterpret_cast<void **>(out.put()))) ||
            !out)
            return -5;
        return 0;
    }
    return -5; // 没找到该 PID 的会话
}

// 取系统主音量接口。
bool OpenEndpointVolume(ComPtr<IAudioEndpointVolume> &out, int &code, std::wstring &error) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioSessionManager2> manager;
    if (!OpenSessionManager(enumerator, device, manager, code, error))
        return false;
    if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void **>(out.put()))) ||
        !out) {
        code = -2;
        error = L"failed to activate IAudioEndpointVolume";
        return false;
    }
    return true;
}

double Clamp01(double v) {
    if (v < 0.0)
        return 0.0;
    if (v > 1.0)
        return 1.0;
    return v;
}

} // namespace

unsigned long GetPidFromWindow(void *hwnd) {
    if (hwnd == nullptr)
        return 0;
    const HWND h = static_cast<HWND>(hwnd);
    if (!::IsWindow(h))
        return 0;
    DWORD pid = 0;
    ::GetWindowThreadProcessId(h, &pid);
    return static_cast<unsigned long>(pid);
}

SessionInfo QueryByWindow(void *hwnd) {
    SessionInfo info;
    if (hwnd == nullptr) {
        info.code = -4;
        info.error = kErrBadHwnd;
        return info;
    }
    const HWND h = static_cast<HWND>(hwnd);
    if (!::IsWindow(h)) {
        info.code = -4;
        info.error = kErrBadHwnd;
        return info;
    }
    DWORD pid = 0;
    ::GetWindowThreadProcessId(h, &pid);
    if (pid == 0) {
        info.code = -4;
        info.error = kErrBadPid;
        return info;
    }
    return QueryByPid(static_cast<unsigned long>(pid));
}

SessionInfo QueryByPid(unsigned long pid) {
    SessionInfo info;
    info.pid = pid;
    if (pid == 0) {
        info.code = -4;
        info.error = kErrBadPid;
        return info;
    }

    ComGuard com;
    if (!com.usable()) {
        info.code = -2;
        info.error = L"COM initialization failed";
        return info;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioSessionManager2> manager;
    int code = 0;
    std::wstring error;
    if (!OpenSessionManager(enumerator, device, manager, code, error)) {
        info.code = code;
        info.error = error;
        return info;
    }

    const bool enumerated =
        FindSessionForPid(manager.get(), pid, info.found, info.muted, info.playing, info.volume, info.peak);
    if (!enumerated) {
        info.code = -3;
        info.error = kErrEnumFailed;
        return info;
    }
    // 枚举成功即为成功；found=false 是"该 PID 没有会话"的有效结果，不是错误
    info.ok = true;
    info.code = 0;
    return info;
}

int SetSessionVolume(unsigned long pid, double volume) {
    if (pid == 0)
        return -4;
    ComGuard com;
    if (!com.usable())
        return -2;

    ComPtr<ISimpleAudioVolume> simple;
    const int opened = OpenSessionVolume(pid, simple);
    if (opened != 0)
        return opened;

    const float v = static_cast<float>(Clamp01(volume));
    if (FAILED(simple->SetMasterVolume(v, nullptr)))
        return -5;
    return 0;
}

int SetSessionMute(unsigned long pid, bool muted) {
    if (pid == 0)
        return -4;
    ComGuard com;
    if (!com.usable())
        return -2;

    ComPtr<ISimpleAudioVolume> simple;
    const int opened = OpenSessionVolume(pid, simple);
    if (opened != 0)
        return opened;

    if (FAILED(simple->SetMute(muted ? TRUE : FALSE, nullptr)))
        return -5;
    return 0;
}

SystemVolumeInfo GetSystemVolume() {
    SystemVolumeInfo info;
    ComGuard com;
    if (!com.usable()) {
        info.code = -2;
        info.error = L"COM initialization failed";
        return info;
    }

    ComPtr<IAudioEndpointVolume> endpoint;
    int code = 0;
    std::wstring error;
    if (!OpenEndpointVolume(endpoint, code, error)) {
        info.code = code;
        info.error = error;
        return info;
    }

    BOOL m = FALSE;
    if (SUCCEEDED(endpoint->GetMute(&m)))
        info.muted = (m != FALSE);
    float v = 0.0f;
    if (SUCCEEDED(endpoint->GetMasterVolumeLevelScalar(&v)))
        info.volume = static_cast<double>(v);
    info.ok = true;
    return info;
}

int SetSystemVolume(double volume) {
    ComGuard com;
    if (!com.usable())
        return -2;
    ComPtr<IAudioEndpointVolume> endpoint;
    int code = 0;
    std::wstring error;
    if (!OpenEndpointVolume(endpoint, code, error))
        return code;
    const float v = static_cast<float>(Clamp01(volume));
    if (FAILED(endpoint->SetMasterVolumeLevelScalar(v, nullptr)))
        return -5;
    return 0;
}

int SetSystemMute(bool muted) {
    ComGuard com;
    if (!com.usable())
        return -2;
    ComPtr<IAudioEndpointVolume> endpoint;
    int code = 0;
    std::wstring error;
    if (!OpenEndpointVolume(endpoint, code, error))
        return code;
    if (FAILED(endpoint->SetMute(muted ? TRUE : FALSE, nullptr)))
        return -5;
    return 0;
}

} // namespace op::audio
