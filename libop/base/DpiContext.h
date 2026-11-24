#pragma once
#ifndef OP_BASE_DPI_CONTEXT_H_
#define OP_BASE_DPI_CONTEXT_H_

#include <Windows.h>

namespace op::win32 {

// ── 为什么需要这个 ────────────────────────────────────────────────────────────
// 同一个窗口，在「宿主线程的 DPI 上下文」和「窗口自身的 DPI 上下文」下量出来的
// 客户区尺寸/位置可能不同。典型场景：目标进程 DPI 不感知（VNC 查看器、老程序、
// 部分游戏），宿主是 DPI aware 且系统缩放 150%：
//   · DPI-aware 宿主跨进程 GetClientRect → 606x421（物理像素）
//   · 窗口 DC 里的位图（BitBlt / PrintWindow 抓到的帧）→ 404x281（逻辑像素）
// 两者差 dpi/96 = 1.5 倍。op 的区域/找图/OCR 都以「帧像素」为坐标空间，所以所有
// 窗口度量与屏幕↔客户区换算必须在窗口自身的 DPI 上下文里做，否则：
//   · ScreenToClient 把物理屏幕坐标当逻辑用 → 回填区域整体偏大 1.5 倍
//   · GetClientSize 报 606x421 而帧只有 404x281 → 区域越界判定失效、越界读垃圾
//   · gdi 的 dx_/dy_ 在物理空间算、作用在逻辑表面上 → 固定平移
//
// 本类在作用域内把当前线程切到 hwnd 所属窗口的 DPI 上下文；窗口与线程上下文
// 一致时**不切换**，因此对普通 DPI 感知窗口的行为与数值完全不变（零回归）。
// ────────────────────────────────────────────────────────────────────────────
class dpi_context_scope {
  public:
    explicit dpi_context_scope(HWND hwnd) noexcept;
    ~dpi_context_scope();

    dpi_context_scope(const dpi_context_scope &) = delete;
    dpi_context_scope &operator=(const dpi_context_scope &) = delete;

    // 是否真的发生了上下文切换（false 表示目标窗口与调用线程同上下文，
    // 此时数值与切换前一致，调用方可省掉缩放换算）。
    bool changed() const noexcept {
        return changed_;
    }

  private:
    void *previous_ = nullptr;
    bool changed_ = false;
};

// 「宿主线程上下文口径 / 窗口自身上下文口径」的像素比例。
// 1.0 表示无差异（普通 DPI 感知窗口，绝大多数字场景）。
// DPI 不感知窗口在 150% 缩放系统上返回 1.5。
double window_pixel_scale(HWND hwnd) noexcept;

// 屏幕坐标 → 帧口径客户区坐标。
// host_screen 是调用方视角的屏幕坐标（DPI-aware 宿主下即物理像素，也就是框选/取色得到的值）。
// 实现是「宿主上下文先 ScreenToClient 得物理客户区偏移，再除以像素比」——
// **不能**按屏幕原点直接缩放：非客户区（标题栏/边框）不随 DPI 缩放，按原点比例换算会
// 引入 (边框物理 − 边框逻辑×比例) 的固定偏差（实测 15px，正是首版实现的 bug）。
POINT screen_to_client_frame(HWND hwnd, POINT host_screen) noexcept;

// 帧口径客户区坐标 → 屏幕坐标（宿主口径）。与上面互逆：物理客户区原点 + 偏移×像素比。
POINT client_frame_to_screen(HWND hwnd, POINT frame_client) noexcept;

// 运行环境是否支持 DPI 上下文 API（Win10 1607+）。不支持时所有接口退化为恒等。
bool dpi_context_api_available() noexcept;

} // namespace op::win32

#endif // OP_BASE_DPI_CONTEXT_H_
