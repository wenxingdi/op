#pragma once
// 音频音量服务（2026-11-21，WASAPI 会话级 + 系统级）。
//
// 设计见 docs/2026-11/音量模块_方案_20261121.md。
//
// 技术路线（沿用规划 §8.1 定稿）：
//   MMDeviceEnumerator -> GetDefaultAudioEndpoint(eRender, eConsole)
//     -> IAudioSessionManager2 -> 按 PID 匹配 IAudioSessionControl2
//       -> ISimpleAudioVolume      （滑块音量 / 静音，可写）
//       -> IAudioMeterInformation （GetPeakValue 实时响度，只读）
//     -> IAudioEndpointVolume      （系统总音量 / 系统静音，可写）
//
// 关键约定：
//   - **不缓存任何 COM 指针**：每次调用重新枚举。游戏重启后 PID 变化，
//     缓存会话指针会指向已失效对象（规划里 Python 版踩过的真机坑）。
//   - **COM 初始化 RAII 且只释放自己成功那次**：接受 RPC_E_CHANGED_MODE，
//     因为 op 是插件，宿主可能已在 STA 里初始化过 COM，此时不能 CoUninitialize。
//   - **DLL 无状态**：峰值保持/衰减/阈值判定全部留给调用方。
//
// ⚠ 最重要的语义区分（误用率最高）：
//   volume（滑块值）**与出声大小无直接关系**——滑块 1.0 也可能当前静音/无声。
//   判断"现在有没有声音"要看 **peak**。
//   found=false 表示"该 PID 没有音频会话"，**不等于没声音**（可能在放系统音，
//   或进程刚启动还没建会话）。

#include <string>

namespace op::audio {

// 单次会话查询结果。
struct SessionInfo {
    bool ok = false;    // 查询链路是否走通（不代表有声音）
    int code = 0;       // 0 成功；负数为失败分类（见 audio_service.cpp 顶部说明）
    bool found = false; // 该 PID 是否存在音频会话
    unsigned long pid = 0;
    bool muted = false;
    bool playing = false; // 会话状态 == Active
    double volume = 0.0;  // 滑块 0.0~1.0
    double peak = 0.0;    // 实时峰值 0.0~1.0
    std::wstring error;   // 失败原因（成功为空）
};

// 系统主音量查询结果。
struct SystemVolumeInfo {
    bool ok = false;
    int code = 0;
    bool muted = false;
    double volume = 0.0;
    std::wstring error;
};

// 按 PID 查询该进程的音频会话。
SessionInfo QueryByPid(unsigned long pid);

// 按窗口句柄查询（先取窗口 PID 再查会话）。hwnd 无效时返回 code=-4。
SessionInfo QueryByWindow(void *hwnd);

// 纯取窗口所属 PID（不碰 COM）。失败返回 0。
unsigned long GetPidFromWindow(void *hwnd);

// 设置会话滑块音量（0.0~1.0，越界自动 clamp）。成功返回 0，失败返回负 code。
int SetSessionVolume(unsigned long pid, double volume);

// 设置会话静音。成功返回 0，失败返回负 code。
int SetSessionMute(unsigned long pid, bool muted);

// 读取系统主音量。
SystemVolumeInfo GetSystemVolume();

// 设置系统主音量（0.0~1.0，越界自动 clamp）。成功返回 0，失败返回负 code。
int SetSystemVolume(double volume);

// 设置系统静音。成功返回 0，失败返回负 code。
int SetSystemMute(bool muted);

} // namespace op::audio
