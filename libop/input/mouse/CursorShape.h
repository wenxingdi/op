#pragma once

#include <Windows.h>
#include <cstdint>
#include <string>
#include <vector>

struct CursorShapeInfo {
    bool visible = false;
    std::uint64_t hash = 0;
    long width = 0;
    long height = 0;
    long hot_x = 0;
    long hot_y = 0;
};

namespace cursor_shape {

bool FromCursor(HCURSOR cursor, bool visible, CursorShapeInfo &info);
bool FromSystem(CursorShapeInfo &info);
std::uint64_t PackMeta(const CursorShapeInfo &info, bool valid);
bool UnpackMeta(std::uint64_t meta, std::uint64_t hash, CursorShapeInfo &info);
std::wstring Format(const CursorShapeInfo &info);

} // namespace cursor_shape

// 光标位图抓取（P2 第二十五批 2026-10-16）：渲染核心与 cursor_shape 共用。
namespace cursor_image {

// 抓取当前系统光标为 32bpp BMP（top-down BGRA，含 alpha 通道）。
// 失败（光标不可见/句柄无效）返回 false。
bool FromSystem(std::vector<unsigned char> &bmp_bytes, long *width, long *height);

} // namespace cursor_image
