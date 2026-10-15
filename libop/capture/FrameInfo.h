#ifndef OP_CAPTURE_FRAME_INFO_H_
#define OP_CAPTURE_FRAME_INFO_H_
#include <Windows.h>
#include <ostream>

namespace op::capture {

#pragma pack(1)
struct FrameInfo {
    unsigned __int64 hwnd;
    unsigned __int32 frameId;
    unsigned __int32 time;
    unsigned __int32 width;
    unsigned __int32 height;
    unsigned __int32 chk;
    void fmtChk() {
        chk = (hwnd >> 32) ^ (hwnd & 0xffffffffull) ^ frameId ^ time ^ width ^ height;
    }

    // 帧头推进一格并落校验和。inc=false 时只更新 hwnd/尺寸/时间，帧序号不变
    // （DXGI/WGC 覆写既有帧头时用）。时间统一取 GetTickCount64 截断到 u32。
    void format(HWND hwnd_, int w_, int h_, bool inc = true) {
        hwnd = (unsigned __int64)hwnd_;
        frameId = inc ? frameId + 1 : frameId;
        time = static_cast<unsigned __int32>(::GetTickCount64());
        width = w_;
        height = h_;
        fmtChk();
    }
};
#pragma pack()

} // namespace op::capture

std::ostream &operator<<(std::ostream &o, op::capture::FrameInfo const &rhs);
std::wostream &operator<<(std::wostream &o, op::capture::FrameInfo const &rhs);

#endif // OP_CAPTURE_FRAME_INFO_H_
