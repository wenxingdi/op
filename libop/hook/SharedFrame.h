#pragma once

#include "../capture/FrameInfo.h"
#include "../ipc/SharedMemory.h"
#include "DisplayHook.h" // CopyImageData 的声明在这里（历史位置），不在 DxCaptureCommon.h
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace op::hook {

inline size_t RequiredSharedFrameBytes(std::uint64_t width, std::uint64_t height) {
    if (width == 0 || height == 0)
        return 0;

    constexpr std::uint64_t bytes_per_pixel = 4;
    constexpr std::uint64_t header_bytes = sizeof(op::capture::FrameInfo);
    constexpr std::uint64_t max_size = (std::numeric_limits<size_t>::max)();
    if (width > max_size / height)
        return 0;
    const std::uint64_t pixels = width * height;
    if (pixels > (max_size - header_bytes) / bytes_per_pixel)
        return 0;
    return static_cast<size_t>(header_bytes + pixels * bytes_per_pixel);
}

inline bool SharedFrameHasCapacity(const op::SharedMemory &mem, std::uint64_t width, std::uint64_t height) {
    const size_t required = RequiredSharedFrameBytes(width, height);
    return required != 0 && mem.size() >= required;
}

inline bool WriteSharedFrameHeader(op::SharedMemory &mem, HWND hwnd, int width, int height) {
    if (mem.size() < sizeof(op::capture::FrameInfo))
        return false;
    auto *info = reinterpret_cast<op::capture::FrameInfo *>(mem.data<unsigned char>());
    info->format(hwnd, width, height);
    return true;
}

// ---------------------------------------------------------------------------
// 帧读/写辅助
//
// 原先这几件东西散在两处：读取侧三个函数在 HookCapture.cpp 的**匿名命名空间**里，
// 写入侧的 make_shared_frame_span/write_shared_frame 在 D3D10Capture.cpp 与
// D3D11Capture.cpp 里**逐字重复了各一份**。
//
// 搬进本头文件的实际动机是可测试性：HookCapture.cpp 依赖 blackbone，编不进测试进程，
// 于是 hook 捕获最关键的一条链路（6 个后端写入同一份共享内存 → 宿主统一校验读取）
// 长期零单测 —— 改错了没有任何用例会报。这里是 header-only 的 inline 纯函数，
// 测试可直接 include 后调用。
// ---------------------------------------------------------------------------

// 共享帧的读取视图：帧头 + 像素区。
struct HookFrameView {
    op::capture::FrameInfo *info = nullptr;
    std::span<std::byte> pixels;
};

// 帧是否可用（可读）。三重校验，任一不过即判「无帧」：
//   1) 校验和自洽 —— chk 由 hwnd/frameId/time/width/height 五个字段算出，重算必须同值。
//      写入侧（目标进程）与读取侧（宿主）隔进程，且读侧可能正好读到"写到一半"的帧
//      （写 header 在前、像素在后，中间没有屏障），所以这一条是撕裂帧的唯一防线。
//   2) hwnd 匹配当前绑定窗口 —— 挡住上一个绑定残留的旧帧。
//   3) 宽高为正 —— 挡住"header 已写、尺寸字段仍是 0"的初始化态。
inline bool isHookFrameReady(const op::capture::FrameInfo &info, HWND hwnd) {
    op::capture::FrameInfo expected = info;
    const auto chk = expected.chk;
    expected.fmtChk();
    return chk == expected.chk && info.hwnd == reinterpret_cast<unsigned __int64>(hwnd) && info.width > 0 &&
           info.height > 0;
}

inline HookFrameView makeHookFrameView(op::SharedMemory &sharedMemory, int width, int height) {
    auto *base = sharedMemory.data<std::byte>();
    auto *info = reinterpret_cast<op::capture::FrameInfo *>(base);
    const auto pixelBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;

    // 使用 span 表达像素区，后续行拷贝不再直接散落裸指针偏移。
    return {info, {base + sizeof(op::capture::FrameInfo), pixelBytes}};
}

// 取像素区第 row 行、从 x 起 width 列的一段（每像素 4 字节）。调用方负责保证区间在界内。
inline std::span<const std::byte> hookFrameRow(std::span<const std::byte> pixels, int srcWidth, int row, int x,
                                               int width) {
    const auto offset = (static_cast<size_t>(row) * static_cast<size_t>(srcWidth) + static_cast<size_t>(x)) * 4;
    const auto bytes = static_cast<size_t>(width) * 4;
    return pixels.subspan(offset, bytes);
}

// 写入侧的共享帧 span（帧头 + 像素）。
inline std::span<std::byte> make_shared_frame_span(op::SharedMemory &mem, UINT width, UINT height) {
    const auto pixelBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    return {mem.data<std::byte>(), sizeof(op::capture::FrameInfo) + pixelBytes};
}

// 写帧头 + 拷贝像素。使用 span 明确区分帧头和像素区，避免共享内存裸指针偏移散落在捕获逻辑里。
inline void write_shared_frame(std::span<std::byte> sharedFrame, HWND hwnd, UINT width, UINT height,
                               const void *source, int sourceRows, int sourceCols, int rowPitch, int format) {
    auto frameInfoBytes = sharedFrame.first(sizeof(op::capture::FrameInfo));
    auto pixelBytes = sharedFrame.subspan(sizeof(op::capture::FrameInfo));

    reinterpret_cast<op::capture::FrameInfo *>(frameInfoBytes.data())->format(hwnd, width, height);
    CopyImageData(reinterpret_cast<char *>(pixelBytes.data()), static_cast<const char *>(source), sourceRows,
                  sourceCols, rowPitch, format);
}

} // namespace op::hook
