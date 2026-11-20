# 安全地把图像注册表实现插入 ImageSearchService.cpp
# 用法：python scripts\probes\insert_image_registry.py
#
# 为什么要用脚本而不是行号编辑：上一轮用 PowerShell 行号操作时，
# $lines 意外为空导致整文件被覆盖（仅剩 274 行），只能 git checkout 恢复。
# 脚本做三件事保证安全：
#   1) 读入后校验行数与关键字，不符合预期立即退出（不写文件）
#   2) 在 "} // namespace op::image" 之前插入
#   3) 写入前把原文件备份为 .bak，写入后再次校验行数与关键字

import io
import os
import sys

REPO = r"D:\AutoPro\op-master\op"
TARGET = os.path.join(REPO, "libop", "image", "ImageSearchService.cpp")
SENTINEL = "} // namespace op::image"

IMPL = r'''
// ============================================================================
// 图像句柄注册表（2026-11-21，内存直操作一期地基）
// ============================================================================
//
// 目标：让调用方直接把自己手上的像素交给 op，省掉"落盘 -> 读盘"的往返
// （该链路实测 50~200ms，内存注册 <5ms）。
//
// 设计要点与取舍见 docs/2026-11/内存模块优化_图像句柄注册表_方案_20261121.md：
//   - 句柄形态 "img:<n>"：C API / COM 层都是普通字符串，客户端零改动
//   - 用 shared_ptr 快照：读的同时别的线程 Free 也不会悬垂（与 g_pic_cache 同策略）
//   - 达上限**拒绝新增**而非静默淘汰：图像句柄是调用方显式创建的，
//     静默清空会让对方手里的句柄突然失效，是更糟的失败模式
//   - 注册后像素**不可变**（本期不做就地修改）：句柄可被多线程并发读，
//     允许就地写会引入数据竞争；要变体就新建句柄

namespace {

// 单张图像像素上限：与 OCR 引擎的 64MB 限制对齐，防止调用方误传巨大尺寸。
constexpr size_t kMaxImageBytes = 64ULL * 1024 * 1024;

std::wstring make_image_handle(long seq) {
    return L"img:" + std::to_wstring(seq);
}

// 解析 "img:<n>"；不是本注册表的句柄返回 false。只做形态校验，不查表。
bool is_image_handle(const std::wstring &handle) {
    return handle.rfind(L"img:", 0) == 0 && handle.size() > 4;
}

// 把任意像素格式按 stride 复制成内部 BGRA（紧凑）。失败返回 false。
bool copy_to_bgra(const void *data, long width, long height, long stride, const std::wstring &fmt,
                  std::shared_ptr<Image> &out) {
    out = nullptr;
    if (data == nullptr || width <= 0 || height <= 0)
        return false;
    const size_t px = size_t(width) * size_t(height);
    if (px == 0 || px * 4 > kMaxImageBytes)
        return false;

    // 格式归一：接受 "bgra"/"bgr"/"gray" 以及对应的通道数 "4"/"3"/"1"
    int bpp = 4;
    std::wstring f = fmt;
    for (auto &c : f)
        c = static_cast<wchar_t>(towlower(c));
    if (f.empty() || f == L"bgra" || f == L"4")
        bpp = 4;
    else if (f == L"bgr" || f == L"3")
        bpp = 3;
    else if (f == L"gray" || f == L"1" || f == L"grey")
        bpp = 1;
    else
        return false; // 未知格式：显式失败，不猜

    const long min_stride = width * bpp;
    if (stride <= 0)
        stride = min_stride;
    if (stride < min_stride)
        return false; // 行宽不足以容纳一行像素：非法

    auto img = std::make_shared<Image>();
    img->create(width, height);
    if (img->pdata == nullptr)
        return false;

    const auto *src = static_cast<const unsigned char *>(data);
    for (long y = 0; y < height; ++y) {
        const unsigned char *row = src + size_t(y) * size_t(stride);
        unsigned char *dst = reinterpret_cast<unsigned char *>(img->pdata) + size_t(y) * size_t(width) * 4;
        for (long x = 0; x < width; ++x) {
            const unsigned char *p = row + size_t(x) * size_t(bpp);
            unsigned char *q = dst + size_t(x) * 4;
            if (bpp == 4) {
                q[0] = p[0]; // B
                q[1] = p[1]; // G
                q[2] = p[2]; // R
                q[3] = p[3]; // A
            } else if (bpp == 3) {
                q[0] = p[0];
                q[1] = p[1];
                q[2] = p[2];
                q[3] = 255;
            } else {
                q[0] = p[0];
                q[1] = p[0];
                q[2] = p[0];
                q[3] = 255;
            }
        }
    }
    out = std::move(img);
    return true;
}

// 生成 BMP 字节流（32bpp BGRA，top-down）。
void encode_bmp32(const Image &img, std::vector<unsigned char> &out) {
    out.clear();
    if (img.width <= 0 || img.height <= 0 || img.pdata == nullptr)
        return;
    const size_t pixel_bytes = size_t(img.width) * size_t(img.height) * 4;
    const size_t header = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    out.resize(header + pixel_bytes, 0);

    auto *bfh = reinterpret_cast<BITMAPFILEHEADER *>(out.data());
    auto *bih = reinterpret_cast<BITMAPINFOHEADER *>(out.data() + sizeof(BITMAPFILEHEADER));
    bfh->bfType = 0x4D42; // 'BM'
    bfh->bfOffBits = static_cast<DWORD>(header);
    bfh->bfSize = static_cast<DWORD>(out.size());
    bih->biSize = sizeof(BITMAPINFOHEADER);
    bih->biWidth = img.width;
    bih->biHeight = -img.height; // top-down，与内部存储一致
    bih->biPlanes = 1;
    bih->biBitCount = 32;
    bih->biCompression = BI_RGB;
    bih->biSizeImage = static_cast<DWORD>(pixel_bytes);
    memcpy(out.data() + header, img.pdata, pixel_bytes);
}

} // namespace

std::wstring ImageSearchService::register_image_mem(const void *data, long width, long height, long stride,
                                                    const std::wstring &fmt) {
    std::shared_ptr<Image> img;
    if (!copy_to_bgra(data, width, height, stride, fmt, img))
        return L"";
    return register_image_shared(std::move(img));
}

std::wstring ImageSearchService::register_image_shared(std::shared_ptr<Image> img) {
    if (!img || img->pdata == nullptr || img->width <= 0 || img->height <= 0)
        return L"";

    std::unique_lock<std::shared_mutex> lock(g_image_registry_mutex);
    const long cap = g_image_registry_max.load(std::memory_order_relaxed);
    if (cap > 0 && static_cast<long>(g_image_registry.size()) >= cap)
        return L""; // 拒绝新增，不淘汰既有句柄（见文件头说明）
    const long seq = ++g_image_registry_seq;
    const std::wstring handle = make_image_handle(seq);
    g_image_registry[handle] = std::move(img);
    return handle;
}

std::shared_ptr<Image> ImageSearchService::acquire_image(const std::wstring &handle) {
    if (!is_image_handle(handle))
        return nullptr;
    std::shared_lock<std::shared_mutex> lock(g_image_registry_mutex);
    const auto it = g_image_registry.find(handle);
    if (it == g_image_registry.end())
        return nullptr;
    return it->second; // shared_ptr 快照：锁外使用也安全
}

std::wstring ImageSearchService::register_image_bmp(const void *data, long size) {
    if (data == nullptr || size <= 0)
        return L"";
    // 直接复用仓库既有的 BMP 解码（支持 24/32bpp、top-down/bottom-up、行填充）。
    auto img = std::make_shared<Image>();
    if (!img->read(const_cast<void *>(data), size))
        return L"";
    if (img->width <= 0 || img->height <= 0 || img->pdata == nullptr)
        return L"";
    if (size_t(img->width) * size_t(img->height) * 4 > kMaxImageBytes)
        return L"";
    return register_image_shared(std::move(img));
}

long ImageSearchService::release_image(const std::wstring &handle) {
    if (!is_image_handle(handle))
        return 0;
    std::unique_lock<std::shared_mutex> lock(g_image_registry_mutex);
    return g_image_registry.erase(handle) > 0 ? 1 : 0;
}

long ImageSearchService::release_all_images() {
    std::unique_lock<std::shared_mutex> lock(g_image_registry_mutex);
    const long n = static_cast<long>(g_image_registry.size());
    g_image_registry.clear();
    return n;
}

long ImageSearchService::image_count() {
    std::shared_lock<std::shared_mutex> lock(g_image_registry_mutex);
    return static_cast<long>(g_image_registry.size());
}

long ImageSearchService::get_image_size(const std::wstring &handle, long &width, long &height, long &channels) {
    width = height = channels = 0;
    auto img = acquire_image(handle);
    if (!img)
        return 0;
    width = img->width;
    height = img->height;
    channels = 4; // 内部统一 BGRA
    return 1;
}

long ImageSearchService::get_image_bytes(const std::wstring &handle, const std::wstring &fmt,
                                         std::vector<unsigned char> &out) {
    out.clear();
    auto img = acquire_image(handle);
    if (!img)
        return 0;

    std::wstring f = fmt;
    for (auto &c : f)
        c = static_cast<wchar_t>(towlower(c));
    if (f.empty() || f == L"raw") {
        const size_t n = size_t(img->width) * size_t(img->height) * 4;
        out.assign(reinterpret_cast<const unsigned char *>(img->pdata),
                   reinterpret_cast<const unsigned char *>(img->pdata) + n);
        return static_cast<long>(out.size());
    }
    if (f == L"bmp") {
        encode_bmp32(*img, out);
        return static_cast<long>(out.size());
    }
    return 0; // 未知格式：显式失败
}

std::wstring ImageSearchService::crop_image(const std::wstring &handle, long x, long y, long w, long h) {
    auto img = acquire_image(handle);
    if (!img)
        return L"";

    // clamp 到图像内（负坐标/超界都收敛，不报错——裁剪是高频操作，宽容更有用）
    long x0 = x < 0 ? 0 : x;
    long y0 = y < 0 ? 0 : y;
    long x1 = x + w;
    long y1 = y + h;
    if (x1 > img->width)
        x1 = img->width;
    if (y1 > img->height)
        y1 = img->height;
    if (x1 <= x0 || y1 <= y0)
        return L"";

    const long cw = x1 - x0;
    const long ch = y1 - y0;
    auto dst = std::make_shared<Image>();
    dst->create(cw, ch);
    if (dst->pdata == nullptr)
        return L"";

    // 深拷贝：源按 (x0,y0) 偏移 + 行跨度 width*4
    const auto *src = reinterpret_cast<const unsigned char *>(img->pdata);
    auto *d = reinterpret_cast<unsigned char *>(dst->pdata);
    for (long row = 0; row < ch; ++row) {
        const unsigned char *s = src + (size_t(y0 + row) * size_t(img->width) + size_t(x0)) * 4;
        memcpy(d + size_t(row) * size_t(cw) * 4, s, size_t(cw) * 4);
    }
    return register_image_shared(std::move(dst));
}

long ImageSearchService::find_str_mem(const std::wstring &handle, const std::wstring &json_targets, double sim,
                                      std::wstring &out_json) {
    out_json.clear();
    auto img = acquire_image(handle);
    if (!img)
        return -1;

    // 复用 P1 的匹配内核：临时把 _src 指向这份像素。
    // 保存并恢复原 _src，避免污染同一 ImageSearchService 实例的后续屏幕调用。
    Image saved = std::move(_src);
    _src = *img; // Image 拷贝构造是深拷贝（create + memcpy）
    struct RestoreGuard {
        Image *target;
        Image *saved;
        ~RestoreGuard() { *target = std::move(*saved); }
    } guard{&_src, &saved};

    return find_str_batch(json_targets, L"", sim, out_json);
}

'''


def main():
    if not os.path.isfile(TARGET):
        print("FAIL: target not found:", TARGET)
        return 1

    with io.open(TARGET, "r", encoding="utf-8", newline="") as f:
        text = f.read()

    lines = text.splitlines()
    before = len(lines)
    print("lines before:", before)

    # 校验：必须包含关键函数与哨兵，否则拒绝写入
    required = ["find_str_batch", "collect_ocr_lines", SENTINEL]
    for key in required:
        if key not in text:
            print("FAIL: missing required marker:", key)
            return 1
    if before < 2000:
        print("FAIL: file looks truncated (<2000 lines), refusing to write")
        return 1
    if text.count(SENTINEL) != 1:
        print("FAIL: sentinel must appear exactly once, found:", text.count(SENTINEL))
        return 1
    if "图像句柄注册表（2026-11-21" in text:
        print("SKIP: implementation already present")
        return 0

    nl = "\r\n" if "\r\n" in text else "\n"
    impl = IMPL.replace("\r\n", "\n").replace("\n", nl)

    idx = text.index(SENTINEL)
    new_text = text[:idx] + impl + text[idx:]

    backup = TARGET + ".bak"
    with io.open(backup, "w", encoding="utf-8", newline="") as f:
        f.write(text)

    with io.open(TARGET, "w", encoding="utf-8", newline="") as f:
        f.write(new_text)

    # 写入后校验
    with io.open(TARGET, "r", encoding="utf-8", newline="") as f:
        check = f.read()
    after = len(check.splitlines())
    ok = (
        after > before
        and "register_image_mem" in check
        and check.count(SENTINEL) == 1
        and "find_str_batch" in check
    )
    print("lines after:", after, "| ok:", ok)
    if not ok:
        with io.open(TARGET, "w", encoding="utf-8", newline="") as f:
            f.write(text)
        print("ROLLED BACK")
        return 1
    os.remove(backup)
    print("OK: inserted image registry implementation")
    return 0


if __name__ == "__main__":
    sys.exit(main())
