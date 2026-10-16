#include "CursorShape.h"

#include <cstring>
#include <iomanip>
#include <sstream>
#include <vector>

namespace {

struct ScopedIconInfo {
    ICONINFO value = {};

    ~ScopedIconInfo() {
        if (value.hbmColor)
            ::DeleteObject(value.hbmColor);
        if (value.hbmMask)
            ::DeleteObject(value.hbmMask);
    }
};

struct ScopedBitmap {
    HBITMAP handle = nullptr;

    ~ScopedBitmap() {
        if (handle)
            ::DeleteObject(handle);
    }
};

struct ScopedDc {
    HDC handle = nullptr;

    ~ScopedDc() {
        if (handle)
            ::DeleteDC(handle);
    }
};

struct ScopedIcon {
    HICON handle = nullptr;

    ~ScopedIcon() {
        if (handle)
            ::DestroyIcon(handle);
    }

    ScopedIcon() = default;
    explicit ScopedIcon(HICON icon) : handle(icon) {
    }

    ScopedIcon(const ScopedIcon &) = delete;
    ScopedIcon &operator=(const ScopedIcon &) = delete;
};

struct SelectBitmapGuard {
    HDC dc = nullptr;
    HGDIOBJ old = nullptr;

    SelectBitmapGuard(HDC dc_, HBITMAP bitmap) : dc(dc_), old(::SelectObject(dc_, bitmap)) {
    }

    ~SelectBitmapGuard() {
        if (selected())
            ::SelectObject(dc, old);
    }

    bool selected() const {
        return dc && old && old != HGDI_ERROR;
    }
};

std::uint64_t fnv1a(const void *data, size_t size, std::uint64_t hash = 14695981039346656037ull) {
    const auto *bytes = static_cast<const unsigned char *>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

void hash_u32(std::uint64_t &hash, std::uint32_t value) {
    hash = fnv1a(&value, sizeof(value), hash);
}

bool bitmap_size(HBITMAP bitmap, long &width, long &height) {
    BITMAP bm = {};
    if (!bitmap || !::GetObject(bitmap, sizeof(bm), &bm))
        return false;

    width = bm.bmWidth;
    height = bm.bmHeight;
    return width > 0 && height > 0;
}

// 渲染核心（第二十五批抽取）：把光标画进 32bpp DIB，输出 BGRA 像素与宽高。
// icon_info 由调用方持有（ScopedIconInfo 负责释放内含位图）。
bool render_cursor_bgra(HCURSOR cursor, std::vector<unsigned char> &bgra, long &width, long &height,
                        ScopedIconInfo &icon_info) {
    ScopedIcon copied{::CopyIcon(cursor)};
    if (!copied.handle)
        return false;

    if (!::GetIconInfo(copied.handle, &icon_info.value))
        return false;

    if (!bitmap_size(icon_info.value.hbmColor ? icon_info.value.hbmColor : icon_info.value.hbmMask, width, height))
        return false;

    // 单色光标（无 hbmColor）的掩码高度是实际两倍：上半 XOR 下半 AND。
    if (!icon_info.value.hbmColor)
        height /= 2;
    if (height <= 0)
        return false;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *bits = nullptr;
    ScopedBitmap dib{::CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0)};
    if (!dib.handle || !bits)
        return false;

    ScopedDc dc{::CreateCompatibleDC(nullptr)};
    if (!dc.handle)
        return false;

    SelectBitmapGuard select(dc.handle, dib.handle);
    if (!select.selected())
        return false;
    // 统一渲染成 32 位 BGRA：彩色光标带 alpha，单色光标的透明区保持黑色 alpha=0。
    ::PatBlt(dc.handle, 0, 0, width, height, BLACKNESS);
    if (!::DrawIconEx(dc.handle, 0, 0, copied.handle, width, height, 0, nullptr, DI_NORMAL))
        return false;

    bgra.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    std::memcpy(bgra.data(), bits, bgra.size());
    return true;
}

// BGRA(top-down) → 32bpp BMP 字节流（BITMAPFILEHEADER + BITMAPINFOHEADER + 像素）。
bool wrap_bgra_as_bmp(const std::vector<unsigned char> &bgra, long width, long height,
                      std::vector<unsigned char> &bmp) {
    const size_t pixel_offset = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    if (width <= 0 || height <= 0 || bgra.size() != static_cast<size_t>(width) * static_cast<size_t>(height) * 4)
        return false;

    bmp.assign(pixel_offset + bgra.size(), 0);
    auto *bfh = reinterpret_cast<BITMAPFILEHEADER *>(bmp.data());
    bfh->bfType = static_cast<WORD>(0x4D42);
    bfh->bfOffBits = static_cast<DWORD>(pixel_offset);
    bfh->bfSize = static_cast<DWORD>(bmp.size());

    auto *bih = reinterpret_cast<BITMAPINFOHEADER *>(bmp.data() + sizeof(BITMAPFILEHEADER));
    bih->biSize = sizeof(BITMAPINFOHEADER);
    bih->biWidth = width;
    bih->biHeight = -height; // top-down
    bih->biPlanes = 1;
    bih->biBitCount = 32;
    bih->biCompression = BI_RGB;
    bih->biSizeImage = static_cast<DWORD>(bgra.size());

    std::memcpy(bmp.data() + pixel_offset, bgra.data(), bgra.size());
    return true;
}

} // namespace

namespace cursor_shape {

bool FromCursor(HCURSOR cursor, bool visible, CursorShapeInfo &info) {
    info = {};
    info.visible = visible;
    if (!cursor)
        return false;

    ScopedIconInfo icon_info;
    std::vector<unsigned char> bgra;
    long width = 0;
    long height = 0;
    if (!render_cursor_bgra(cursor, bgra, width, height, icon_info))
        return false;

    info.visible = visible;
    info.width = width;
    info.height = height;
    info.hot_x = static_cast<long>(icon_info.value.xHotspot);
    info.hot_y = static_cast<long>(icon_info.value.yHotspot);

    std::uint64_t hash = 14695981039346656037ull;
    hash_u32(hash, static_cast<std::uint32_t>(width));
    hash_u32(hash, static_cast<std::uint32_t>(height));
    hash_u32(hash, static_cast<std::uint32_t>(info.hot_x));
    hash_u32(hash, static_cast<std::uint32_t>(info.hot_y));
    hash = fnv1a(bgra.data(), bgra.size(), hash);
    info.hash = hash;
    return true;
}

bool FromSystem(CursorShapeInfo &info) {
    CURSORINFO cursor = {};
    cursor.cbSize = sizeof(cursor);
    if (!::GetCursorInfo(&cursor))
        return false;

    const bool visible = (cursor.flags & CURSOR_SHOWING) != 0;
    return FromCursor(cursor.hCursor, visible, info);
}

std::uint64_t PackMeta(const CursorShapeInfo &info, bool valid) {
    std::uint64_t meta = valid ? 1ull : 0ull;
    meta |= (info.visible ? 1ull : 0ull) << 1;
    meta |= (static_cast<std::uint64_t>(info.width) & 0xFFFFull) << 2;
    meta |= (static_cast<std::uint64_t>(info.height) & 0xFFFFull) << 18;
    meta |= (static_cast<std::uint64_t>(info.hot_x) & 0x7FFFull) << 34;
    meta |= (static_cast<std::uint64_t>(info.hot_y) & 0x7FFFull) << 49;
    return meta;
}

bool UnpackMeta(std::uint64_t meta, std::uint64_t hash, CursorShapeInfo &info) {
    if ((meta & 1ull) == 0)
        return false;

    info.visible = ((meta >> 1) & 1ull) != 0;
    info.width = static_cast<long>((meta >> 2) & 0xFFFFull);
    info.height = static_cast<long>((meta >> 18) & 0xFFFFull);
    info.hot_x = static_cast<long>((meta >> 34) & 0x7FFFull);
    info.hot_y = static_cast<long>((meta >> 49) & 0x7FFFull);
    info.hash = hash;
    if (info.width > 0 && info.height > 0)
        return info.hash != 0;
    return !info.visible && info.hash == 0;
}

std::wstring Format(const CursorShapeInfo &info) {
    std::wstringstream ss;
    ss << (info.visible ? 1 : 0) << L"," << std::uppercase << std::hex << std::setw(16) << std::setfill(L'0')
       << info.hash << std::dec << L"," << info.width << L"," << info.height << L"," << info.hot_x << L","
       << info.hot_y;
    return ss.str();
}

} // namespace cursor_shape

namespace cursor_image {

bool FromSystem(std::vector<unsigned char> &bmp_bytes, long *width, long *height) {
    bmp_bytes.clear();
    if (width)
        *width = 0;
    if (height)
        *height = 0;

    CURSORINFO cursor = {};
    cursor.cbSize = sizeof(cursor);
    if (!::GetCursorInfo(&cursor) || !(cursor.flags & CURSOR_SHOWING) || !cursor.hCursor)
        return false;

    ScopedIconInfo icon_info;
    std::vector<unsigned char> bgra;
    long w = 0;
    long h = 0;
    if (!render_cursor_bgra(cursor.hCursor, bgra, w, h, icon_info))
        return false;

    std::vector<unsigned char> bmp;
    if (!wrap_bgra_as_bmp(bgra, w, h, bmp))
        return false;

    bmp_bytes = std::move(bmp);
    if (width)
        *width = w;
    if (height)
        *height = h;
    return true;
}

} // namespace cursor_image
