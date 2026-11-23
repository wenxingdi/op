#include "OpContext.h"
#include "OpCaptureHelpers.h"
#include "OpResult.h"

#include "capture/FrameInfo.h"
#include "base/Utils.h"

#include <libop.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

namespace {

using op::capture::FrameInfo;

constexpr int small_block_size = 10;
constexpr int SC_DATA_TOP = 0;
constexpr int SC_DATA_BOTTOM = 1;

// FNV-1a 64：帧哈希足够快且对字节序/平台稳定，卡屏检测不需要密码学强度。
uint64_t fnv1a64(const void *data, size_t size) {
    const auto *p = static_cast<const unsigned char *>(data);
    uint64_t hash = 1469598103934665603ULL;
    for (size_t i = 0; i < size; ++i) {
        hash ^= p[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

int clamp_byte(long value) {
    return static_cast<int>(value < 0 ? 0 : (value > 255 ? 255 : value));
}

bool hex_nibble(wchar_t c, unsigned &value) {
    if (c >= L'0' && c <= L'9') {
        value = static_cast<unsigned>(c - L'0');
        return true;
    }
    if (c >= L'a' && c <= L'f') {
        value = static_cast<unsigned>(c - L'a' + 10);
        return true;
    }
    if (c >= L'A' && c <= L'F') {
        value = static_cast<unsigned>(c - L'A' + 10);
        return true;
    }
    return false;
}

// 解析 "RRGGBB"/"AARRGGBB"（允许 #/0x 前缀，忽略 "-delta"/"@x" 等后缀）。
// 6 位按不透明处理（alpha=255）。
bool parse_hex_color(const wchar_t *text, int &a, int &r, int &g, int &b) {
    if (!text)
        return false;
    std::wstring value(text);
    const size_t first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos)
        return false;
    const size_t last = value.find_last_not_of(L" \t\r\n");
    value = value.substr(first, last - first + 1);
    if (value.size() >= 2 && value[0] == L'0' && (value[1] == L'x' || value[1] == L'X')) {
        value.erase(0, 2);
    } else if (!value.empty() && value[0] == L'#') {
        value.erase(0, 1);
    }
    const size_t non_hex = value.find_first_not_of(L"0123456789abcdefABCDEF");
    if (non_hex != std::wstring::npos)
        value.erase(non_hex);
    if (value.size() != 6 && value.size() != 8)
        return false;

    unsigned nibbles[8] = {0};
    for (size_t i = 0; i < value.size(); ++i) {
        if (!hex_nibble(value[i], nibbles[i]))
            return false;
    }
    const size_t base = value.size() == 8 ? 2 : 0;
    a = value.size() == 8 ? static_cast<int>(nibbles[0] * 16 + nibbles[1]) : 255;
    r = static_cast<int>(nibbles[base] * 16 + nibbles[base + 1]);
    g = static_cast<int>(nibbles[base + 2] * 16 + nibbles[base + 3]);
    b = static_cast<int>(nibbles[base + 4] * 16 + nibbles[base + 5]);
    return true;
}

void rgb_to_hsv(int r, int g, int b, double &h, double &s, double &v) {
    const double rn = r / 255.0;
    const double gn = g / 255.0;
    const double bn = b / 255.0;
    const double max_c = rn > gn ? (rn > bn ? rn : bn) : (gn > bn ? gn : bn);
    const double min_c = rn < gn ? (rn < bn ? rn : bn) : (gn < bn ? gn : bn);
    const double delta = max_c - min_c;

    v = max_c * 100.0;
    s = max_c > 0.0 ? delta / max_c * 100.0 : 0.0;
    if (delta <= 0.0) {
        h = 0.0;
        return;
    }
    if (max_c == rn) {
        h = 60.0 * std::fmod((gn - bn) / delta, 6.0);
    } else if (max_c == gn) {
        h = 60.0 * ((bn - rn) / delta + 2.0);
    } else {
        h = 60.0 * ((rn - gn) / delta + 4.0);
    }
    if (h < 0.0)
        h += 360.0;
}

} // namespace

void op::Op::EnablePicCache(long enable, long *ret) {
    m_context->image_proc._enable_cache = enable;
    internal::set_result(ret, 1L);
}

void op::Op::ClearPicCache(long *ret) {
    internal::set_result(ret, m_context->image_proc.clear_pic_cache());
}

void op::Op::SetPicCacheMax(long max_count, long *ret) {
    m_context->image_proc.set_pic_cache_max(max_count);
    internal::set_result(ret, 1L);
}

void op::Op::CapturePre(const wchar_t *file, LONG *ret) {
    internal::set_result(ret, m_context->image_proc.Capture(file));
}

void op::Op::SetScreenDataMode(long mode, long *ret) {
    m_context->screen_data_mode = mode;
    internal::set_result(ret, 1L);
}
void op::Op::Capture(long x1, long y1, long x2, long y2, const wchar_t *file_name, long *ret) {

    internal::set_result(ret, 0L);

    internal::with_captured_region(m_context.get(), x1, y1, x2, y2, [&]() {
        internal::set_result(ret, m_context->image_proc.Capture(file_name));
    });
}
// 比较指定坐标点(x,y)的颜色
void op::Op::CmpColor(long x, long y, const wchar_t *color, double sim, long *ret) {
    // LONG rx = -1, ry = -1;
    long tx = x + small_block_size, ty = y + small_block_size;
    internal::set_result(ret, 0L);
    internal::with_captured_region(m_context.get(), x, y, tx, ty, small_block_size, small_block_size, [&]() {
        internal::set_result(ret, m_context->image_proc.CmpColor(x, y, color, sim));
    });
}
// 查找指定区域内的颜色
void op::Op::FindColor(long x1, long y1, long x2, long y2, const wchar_t *color, double sim, long dir, long *x, long *y,
                           long *ret) {

    long found_x = -1;
    long found_y = -1;
    internal::set_result(ret, 0L);
    internal::set_result(x, found_x);
    internal::set_result(y, found_y);

    internal::with_captured_region(m_context.get(), x1, y1, x2, y2, [&]() {
        internal::set_result(ret, m_context->image_proc.FindColor(color, sim, dir, found_x, found_y));
        internal::set_result(x, found_x);
        internal::set_result(y, found_y);
    });
}
// 查找指定区域内的所有颜色
void op::Op::FindColorEx(long x1, long y1, long x2, long y2, const wchar_t *color, double sim, long dir,
                        std::wstring &retstr) {
    // wstring str;
    retstr.clear();
    internal::with_captured_region(m_context.get(), x1, y1, x2, y2, [&]() {
        m_context->image_proc.FindColorEx(color, sim, dir, retstr);
    });
}
// 根据指定的多点查找颜色坐标
void op::Op::FindMultiColor(long x1, long y1, long x2, long y2, const wchar_t *first_color, const wchar_t *offset_color,
                           double sim, long dir, long *x, long *y, long *ret) {

    long found_x = -1;
    long found_y = -1;
    internal::set_result(ret, 0L);
    internal::set_result(x, found_x);
    internal::set_result(y, found_y);

    internal::with_captured_region(m_context.get(), x1, y1, x2, y2, [&]() {
        internal::set_result(ret,
                             m_context->image_proc.FindMultiColor(first_color, offset_color, sim, dir, found_x,
                                                                  found_y));
        internal::set_result(x, found_x);
        internal::set_result(y, found_y);
    });

    /*if (*ret) {
            rx += x1; ry += y1;
            rx -= m_context->bkproc._capture->get_client_x();
            ry -= m_context->bkproc._capture->get_client_y();
        }*/
}
// 根据指定的多点查找所有颜色坐标
void op::Op::FindMultiColorEx(long x1, long y1, long x2, long y2, const wchar_t *first_color,
                             const wchar_t *offset_color, double sim, long dir, std::wstring &retstr) {
    retstr.clear();
    internal::with_captured_region(m_context.get(), x1, y1, x2, y2, [&]() {
        m_context->image_proc.FindMultiColorEx(first_color, offset_color, sim, dir, retstr);
    });
    // retstr = str;
}
// 查找指定区域内的图片
void op::Op::FindPic(long x1, long y1, long x2, long y2, const wchar_t *files, const wchar_t *delta_color, double sim,
                    long dir, long *x, long *y, long *ret) {

    long found_x = -1;
    long found_y = -1;
    // 注意：FindPic 是**大漠索引语义**（返回命中的图片序号，-1=未找到），不是布尔契约。
    // 因此初值必须是 -1L（=未找到），**不要**按「0=失败」改成 0 —— 那会让"未找到"和
    // "命中第 0 张图"无法区分。既有测试 ImageColorTest.FindPicReturnsMinusOneWhenTemplateIsMissing 钉住此语义。
    internal::set_result(ret, -1L);
    internal::set_result(x, found_x);
    internal::set_result(y, found_y);

    internal::with_captured_region(m_context.get(), x1, y1, x2, y2, [&]() {
        internal::set_result(ret, m_context->image_proc.FindPic(files, delta_color, sim, dir, found_x, found_y));
        internal::set_result(x, found_x);
        internal::set_result(y, found_y);
    });

    /*if (*ret) {
            rx += x1; ry += y1;
            rx -= m_context->bkproc._capture->get_client_x();
            ry -= m_context->bkproc._capture->get_client_y();
        }*/
}
// 查找多个图片
void op::Op::FindPicEx(long x1, long y1, long x2, long y2, const wchar_t *files, const wchar_t *delta_color, double sim,
                      long dir, std::wstring &retstr) {

    retstr.clear();
    internal::with_captured_region(m_context.get(), x1, y1, x2, y2, [&]() {
        m_context->image_proc.FindPicEx(files, delta_color, sim, dir, retstr);
    });
}

void op::Op::FindPicExS(long x1, long y1, long x2, long y2, const wchar_t *files, const wchar_t *delta_color, double sim,
                       long dir, std::wstring &retstr) {
    retstr.clear();
    internal::with_captured_region(m_context.get(), x1, y1, x2, y2, [&]() {
        m_context->image_proc.FindPicEx(files, delta_color, sim, dir, retstr, false);
    });
}

void op::Op::FindColorBlock(long x1, long y1, long x2, long y2, const wchar_t *color, double sim, long count,
                           long height, long width, long *x, long *y, long *ret) {
    long found_x = -1;
    long found_y = -1;
    internal::set_result(ret, 0L);
    internal::set_result(x, found_x);
    internal::set_result(y, found_y);
    internal::with_captured_region(m_context.get(), x1, y1, x2, y2, [&]() {
        internal::set_result(ret,
                             m_context->image_proc.FindColorBlock(color, sim, count, height, width, found_x, found_y));
        internal::set_result(x, found_x);
        internal::set_result(y, found_y);
    });
}

void op::Op::FindColorBlockEx(long x1, long y1, long x2, long y2, const wchar_t *color, double sim, long count,
                             long height, long width, std::wstring &retstr) {

    retstr.clear();
    internal::with_captured_region(m_context.get(), x1, y1, x2, y2, [&]() {
        m_context->image_proc.FindColorBlockEx(color, sim, count, height, width, retstr);
    });
}

void op::Op::FindColorBlockExS(long x1, long y1, long x2, long y2, const wchar_t *color, double sim, long count,
                              long height, long width, long mode, std::wstring &retstr) {

    retstr.clear();
    internal::with_captured_region(m_context.get(), x1, y1, x2, y2, [&]() {
        m_context->image_proc.FindColorBlockExS(color, sim, count, height, width, mode, retstr);
    });
}

void op::Op::IsDisplayDead(long x1, long y1, long x2, long y2, long check_times, long interval_ms, long *ret) {
    internal::set_result(ret, 0L);
    if (check_times <= 0)
        check_times = 3;
    if (check_times < 2)
        check_times = 2;
    if (check_times > 20)
        check_times = 20;
    if (interval_ms <= 0)
        interval_ms = 500;
    if (interval_ms < 50)
        interval_ms = 50;
    if (interval_ms > 10000)
        interval_ms = 10000;

    uint64_t last_hash = 0;
    for (long i = 0; i < check_times; ++i) {
        uint64_t hash = 0;
        bool captured = false;
        internal::with_captured_region(m_context.get(), x1, y1, x2, y2, [&]() {
            const auto &img = m_context->image_proc._src;
            hash = fnv1a64(img.pdata, static_cast<size_t>(img.size()) * 4);
            captured = true;
        });
        if (!captured)
            return; // 截图失败：保守判活，ret 保持 0
        if (i > 0 && hash != last_hash)
            return; // 帧间有变化：存活
        last_hash = hash;
        if (i + 1 < check_times)
            ::Sleep(static_cast<DWORD>(interval_ms));
    }
    internal::set_result(ret, 1L); // 全部帧哈希一致：卡死
}

// 获取(x,y)的颜色
void op::Op::GetColor(long x, long y, std::wstring &ret) {
    color_t cr;
    auto tx = x + small_block_size, ty = y + small_block_size;
    internal::with_captured_region(m_context.get(), x, y, tx, ty, small_block_size, small_block_size, [&]() {
        cr = m_context->image_proc._src.at<color_t>(0, 0);
    });

    ret = cr.towstr();
}

void op::Op::Hex2RGB(const wchar_t *color, long *r, long *g, long *b, long *ret) {
    internal::set_result(ret, 0L);
    internal::set_result(r, 0L);
    internal::set_result(g, 0L);
    internal::set_result(b, 0L);
    int a = 0, rr = 0, gg = 0, bb = 0;
    if (!parse_hex_color(color, a, rr, gg, bb))
        return;
    internal::set_result(r, rr);
    internal::set_result(g, gg);
    internal::set_result(b, bb);
    internal::set_result(ret, 1L);
}

void op::Op::Hex2ARGB(const wchar_t *color, long *a, long *r, long *g, long *b, long *ret) {
    internal::set_result(ret, 0L);
    internal::set_result(a, 0L);
    internal::set_result(r, 0L);
    internal::set_result(g, 0L);
    internal::set_result(b, 0L);
    int aa = 0, rr = 0, gg = 0, bb = 0;
    if (!parse_hex_color(color, aa, rr, gg, bb))
        return;
    internal::set_result(a, aa);
    internal::set_result(r, rr);
    internal::set_result(g, gg);
    internal::set_result(b, bb);
    internal::set_result(ret, 1L);
}

namespace {

const wchar_t kHexDigits[] = L"0123456789ABCDEF";

void append_hex_byte(std::wstring &out, int value) {
    out.push_back(kHexDigits[(value >> 4) & 0xF]);
    out.push_back(kHexDigits[value & 0xF]);
}

} // namespace

void op::Op::ARGB2Hex(long a, long r, long g, long b, std::wstring &ret) {
    ret.clear();
    append_hex_byte(ret, clamp_byte(a));
    append_hex_byte(ret, clamp_byte(r));
    append_hex_byte(ret, clamp_byte(g));
    append_hex_byte(ret, clamp_byte(b));
}

void op::Op::GetColorHex(long r, long g, long b, std::wstring &ret) {
    ret.clear();
    append_hex_byte(ret, clamp_byte(r));
    append_hex_byte(ret, clamp_byte(g));
    append_hex_byte(ret, clamp_byte(b));
}

void op::Op::RGB2HSV(long r, long g, long b, double *h, double *s, double *v, long *ret) {
    internal::set_result(h, 0.0);
    internal::set_result(s, 0.0);
    internal::set_result(v, 0.0);
    double hh = 0.0, ss = 0.0, vv = 0.0;
    rgb_to_hsv(clamp_byte(r), clamp_byte(g), clamp_byte(b), hh, ss, vv);
    internal::set_result(h, hh);
    internal::set_result(s, ss);
    internal::set_result(v, vv);
    internal::set_result(ret, 1L);
}

void op::Op::Hex2HSV(const wchar_t *color, double *h, double *s, double *v, long *ret) {
    internal::set_result(ret, 0L);
    internal::set_result(h, 0.0);
    internal::set_result(s, 0.0);
    internal::set_result(v, 0.0);
    int a = 0, rr = 0, gg = 0, bb = 0;
    if (!parse_hex_color(color, a, rr, gg, bb))
        return;
    double hh = 0.0, ss = 0.0, vv = 0.0;
    rgb_to_hsv(rr, gg, bb, hh, ss, vv);
    internal::set_result(h, hh);
    internal::set_result(s, ss);
    internal::set_result(v, vv);
    internal::set_result(ret, 1L);
}

void op::Op::GetColorNum(long x1, long y1, long x2, long y2, const wchar_t *color, double sim, long *ret) {
    internal::set_result(ret, 0L);
    internal::with_captured_region(m_context.get(), x1, y1, x2, y2, [&]() {
        internal::set_result(ret, m_context->image_proc.GetColorNum(color, sim));
    });
}

void op::Op::SetDisplayInput(const wchar_t *mode, long *ret) {
    internal::set_result(ret, m_context->bkproc.set_display_method(mode));
}

void op::Op::LoadPic(const wchar_t *file_name, long *ret) {
    internal::set_result(ret, 0L);
    internal::guard_exceptions("LoadPic", [&]() { internal::set_result(ret, m_context->image_proc.LoadPic(file_name)); });
}

void op::Op::FreePic(const wchar_t *file_name, long *ret) {
    internal::set_result(ret, m_context->image_proc.FreePic(file_name));
}

void op::Op::LoadMemPic(const wchar_t *file_name, void *data, long size, long *ret) {
    internal::set_result(ret, 0L);
    internal::guard_exceptions("LoadMemPic",
                               [&]() { internal::set_result(ret, m_context->image_proc.LoadMemPic(file_name, data, size)); });
}

void op::Op::GetPicSize(const wchar_t *pic_name, long *width, long *height, long *ret) {
    internal::set_result(width, 0L);
    internal::set_result(height, 0L);
    internal::set_result(ret, 0L);
    internal::guard_exceptions("GetPicSize",
                               [&]() { internal::set_result(ret, m_context->image_proc.GetPicSize(pic_name, width, height)); });
}

void op::Op::GetScreenData(long x1, long y1, long x2, long y2, size_t *data, long *ret) {
    internal::set_result(data, 0);
    internal::set_result(ret, 0L);
    auto &img = m_context->image_proc._src;
    internal::with_captured_region(m_context.get(), x1, y1, x2, y2, [&]() {
        m_context->screenData.resize(img.size() * 4);

        if (m_context->screen_data_mode == SC_DATA_BOTTOM) {
            for (int i = 0; i < img.height; i++) {
                memcpy(m_context->screenData.data() + i * img.width * 4, img.ptr<char>(img.height - 1 - i),
                       img.width * 4);
            }
        } else {
            memcpy(m_context->screenData.data(), img.pdata, img.size() * 4);
        }
        internal::set_result(data, reinterpret_cast<size_t>(m_context->screenData.data()));
        internal::set_result(ret, 1L);
    });
}

void op::Op::GetScreenDataBmp(long x1, long y1, long x2, long y2, size_t *data, long *size, long *ret) {
    internal::set_result(data, 0);
    internal::set_result(size, 0L);
    internal::set_result(ret, 0L);
    internal::with_captured_region(m_context.get(), x1, y1, x2, y2, [&]() {
        auto &img = m_context->image_proc._src;

        BITMAPFILEHEADER bfh = {0}; // bmp file header
        BITMAPINFOHEADER bih = {0}; // bmp info header
        const int szBfh = sizeof(BITMAPFILEHEADER);
        const int szBih = sizeof(BITMAPINFOHEADER);
        bfh.bfOffBits = szBfh + szBih;
        bfh.bfSize = bfh.bfOffBits + img.width * img.height * 4;
        bfh.bfType = static_cast<WORD>(0x4d42);

        bih.biBitCount = 32; // 每个像素字节大小
        bih.biCompression = BI_RGB;
        // bih.biHeight = -img.height;//高度 反
        bih.biHeight = m_context->screen_data_mode == SC_DATA_BOTTOM ? img.height : -img.height; // 高度
        bih.biPlanes = 1;
        bih.biSize = sizeof(BITMAPINFOHEADER);
        bih.biSizeImage = img.width * 4 * img.height; // 图像数据大小
        bih.biWidth = img.width;                      // 宽度

        m_context->screenDataBmp.resize(bfh.bfSize);
        /*	std::ofstream f;
        f.open("xx.bmp",std::ios::binary);
        if (f) {
            f.write((char*)&bfh, sizeof(bfh));
            f.write((char*)&bih, sizeof(bih));
            f.write((char*)img.pdata, img.size() * 4);
        }

        f.close();*/
        auto dst = m_context->screenDataBmp.data();

        memcpy(dst, &bfh, sizeof(bfh));
        memcpy(dst + sizeof(bfh), &bih, sizeof(bih));
        dst += sizeof(bfh) + sizeof(bih);
        if (m_context->screen_data_mode == SC_DATA_BOTTOM) {
            for (int i = 0; i < img.height; i++) {
                memcpy(dst + i * img.width * 4, img.ptr<char>(img.height - 1 - i), img.width * 4);
            }
        } else {
            memcpy(dst, img.pdata, img.size() * 4);
        }

        // memcpy(dst + sizeof(bfh)+sizeof(bih), img.pdata, img.size()*4);
        internal::set_result(data, reinterpret_cast<size_t>(m_context->screenDataBmp.data()));
        internal::set_result(size, bfh.bfSize);
        internal::set_result(ret, 1L);
    });
}

void op::Op::GetScreenFrameInfo(long *frame_id, long *time) {
    FrameInfo info = {};
    if (m_context->bkproc.IsBind()) {
        m_context->bkproc._capture->getFrameInfo(info);
    }
    internal::set_result(frame_id, info.frameId);
    internal::set_result(time, info.time);
}

void op::Op::GetFPS(long *ret) {
    internal::set_result(ret, 0L);
    if (!m_context->bkproc.IsBind())
        return;

    FrameInfo a = {}, b = {};
    m_context->bkproc._capture->getFrameInfo(a);
    if (a.hwnd == 0)
        return; // 后端尚无帧

    ::Sleep(1000); // 采样窗口 1 秒

    m_context->bkproc._capture->getFrameInfo(b);
    if (b.hwnd == 0)
        return;

    const DWORD elapsed = b.time - a.time; // GetTickCount 域,DWORD 减法天然抗回绕
    if (elapsed == 0)
        return;

    const auto delta = static_cast<long>(b.frameId - a.frameId);
    internal::set_result(ret, static_cast<long>(delta * 1000.0 / elapsed + 0.5));
}

void op::Op::MatchPicName(const wchar_t *pic_name, std::wstring &retstr) {
    retstr.clear();
    internal::guard_exceptions("MatchPicName", [&]() {
    std::wstring s(pic_name);
    if (s.find(L'/') != s.npos || s.find(L'\\') != s.npos) {
        setlog("invalid pic_name");
    }

    s = std::regex_replace(s, std::wregex(L"(\\.|\\(|\\)|\\[|\\]|\\{|\\})"), L"\\$1");
    /*s = std::regex_replace(s, std::wregex(L"\\("), L"\\(");
    s = std::regex_replace(s, std::wregex(L"\\)"), L"\\)");
    s = std::regex_replace(s, std::wregex(L"\\["), L"\\[");
    s = std::regex_replace(s, std::wregex(L"\\]"), L"\\]");*/
    s = std::regex_replace(s, std::wregex(L"\\*"), L".*?");
    s = std::regex_replace(s, std::wregex(L"\\?"), L".?");

    // setlog(s.data());
    namespace fs = std::filesystem;
    fs::path path(m_context->curr_path);
    if (fs::exists(path)) {
        fs::directory_iterator iter(path);
        std::wstring tmp;
        std::wregex e(s);
        for (auto &it : iter) {
            if (it.status().type() == fs::file_type::regular) {
                tmp = it.path().filename();
                try {
                    if (std::regex_match(tmp, e)) {
                        retstr += tmp;
                        retstr += L"|";
                    }
                } catch (...) {
                    setlog("exception!");
                }
            }
        }
        if (!retstr.empty() && retstr.back() == L'|')
            retstr.pop_back();
    }
    });
}

// ===== 图像句柄注册表门面（2026-11-21，内存直操作一期地基）=====

void op::Op::LoadImageMem(void *data, long width, long height, long stride, const wchar_t *fmt,
                          std::wstring &ret_handle) {
    ret_handle.clear();
    const std::wstring fmt_text = fmt ? fmt : L"";
    internal::guard_exceptions("LoadImageMem", [&]() {
        ret_handle = m_context->image_proc.register_image_mem(data, width, height, stride, fmt_text);
    });
}

void op::Op::LoadImageBmp(void *data, long size, std::wstring &ret_handle) {
    ret_handle.clear();
    internal::guard_exceptions("LoadImageBmp", [&]() {
        ret_handle = m_context->image_proc.register_image_bmp(data, size);
    });
}

void op::Op::FreeImage(const wchar_t *handle, long *ret) {
    internal::set_result(ret, 0L);
    const std::wstring handle_text = handle ? handle : L"";
    internal::guard_exceptions("FreeImage",
                               [&]() { internal::set_result(ret, m_context->image_proc.release_image(handle_text)); });
}

void op::Op::FreeAllImages(long *ret) {
    internal::set_result(ret, 0L);
    internal::guard_exceptions("FreeAllImages",
                               [&]() { internal::set_result(ret, m_context->image_proc.release_all_images()); });
}

void op::Op::GetImageSize(const wchar_t *handle, long *width, long *height, long *channels, long *ret) {
    internal::set_result(width, 0L);
    internal::set_result(height, 0L);
    internal::set_result(channels, 0L);
    internal::set_result(ret, 0L);
    const std::wstring handle_text = handle ? handle : L"";
    internal::guard_exceptions("GetImageSize", [&]() {
        long w = 0, h = 0, c = 0;
        const long ok = m_context->image_proc.get_image_size(handle_text, w, h, c);
        internal::set_result(width, w);
        internal::set_result(height, h);
        internal::set_result(channels, c);
        internal::set_result(ret, ok);
    });
}

void op::Op::GetImageData(const wchar_t *handle, const wchar_t *fmt, void *buffer, long buffer_size, long *ret) {
    internal::set_result(ret, 0L);
    if (buffer == nullptr || buffer_size <= 0)
        return;
    const std::wstring handle_text = handle ? handle : L"";
    const std::wstring fmt_text = fmt ? fmt : L"";
    internal::guard_exceptions("GetImageData", [&]() {
        std::vector<unsigned char> bytes;
        const long n = m_context->image_proc.get_image_bytes(handle_text, fmt_text, bytes);
        if (n <= 0 || n > buffer_size)
            return; // 容量不足：不截断写（截断的 BMP 是坏数据，静默失败更危险）
        memcpy(buffer, bytes.data(), static_cast<size_t>(n));
        internal::set_result(ret, n);
    });
}

void op::Op::CropImage(const wchar_t *handle, long x, long y, long width, long height, std::wstring &ret_handle) {
    ret_handle.clear();
    const std::wstring handle_text = handle ? handle : L"";
    internal::guard_exceptions("CropImage", [&]() {
        ret_handle = m_context->image_proc.crop_image(handle_text, x, y, width, height);
    });
}

long op::Op::FindStrMem(const wchar_t *handle, const wchar_t *json_targets, double sim, std::wstring &ret_json) {
    ret_json.clear();
    const std::wstring handle_text = handle ? handle : L"";
    const std::wstring targets_text = json_targets ? json_targets : L"";
    long rc = -1;
    internal::guard_exceptions("FindStrMem", [&]() {
        rc = m_context->image_proc.find_str_mem(handle_text, targets_text, sim, ret_json);
    });
    return rc;
}

void op::Op::GetImageCount(long *ret) {
    internal::set_result(ret, 0L);
    internal::guard_exceptions("GetImageCount",
                               [&]() { internal::set_result(ret, m_context->image_proc.image_count()); });
}
