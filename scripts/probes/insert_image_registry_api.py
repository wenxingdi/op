# 给 C API 三层补图像注册表导出（安全插入，带校验与回滚）
# 用法：python scripts\probes\insert_image_registry_api.py
#
# 安全要点（前两次踩过的坑）：
#   1) 读入后校验行数与关键字，不符预期立刻退出不写文件
#   2) 写入前备份 .bak，写入后复检行数与关键字，失败自动回滚
#   3) 幂等：检测到已插入则跳过

import io
import os
import sys

REPO = r"D:\AutoPro\op-master\op"

LIBOP_H = os.path.join(REPO, "include", "libop.h")
OP_C_API_H = os.path.join(REPO, "include", "op_c_api.h")
OP_IDL = os.path.join(REPO, "libop", "com", "op.idl")
OP_IMAGE_CPP = os.path.join(REPO, "libop", "op", "OpImage.cpp")
OP_C_API_CPP = os.path.join(REPO, "libop", "c_api", "op_c_api.cpp")
OP_AUTOMATION_H = os.path.join(REPO, "libop", "com", "OpAutomation.h")
OP_AUTOMATION_CPP = os.path.join(REPO, "libop", "com", "OpAutomation.cpp")

# ---------------------------------------------------------------------------
LIBOP_H_BLOCK = r'''
    // ===== 图像句柄注册表（2026-11-21，内存直操作一期地基）=====
    // 目的：让调用方把**自己手上已有的像素**直接交给 op，省掉落盘再读盘的往返。
    // 句柄形态为字符串 img:<n>，与既有 pic:<路径> 的显示输入风格一致，
    // 因此在 C API / COM 层都是普通 BSTR，客户端零学习成本。
    // 设计见 docs/2026-11/内存模块优化_图像句柄注册表_方案_20261121.md。
    //
    // 注册后像素**不可变**：句柄可被多线程并发读，允许就地写会引入数据竞争。
    //   需要变体请新建句柄（CropImage 即如此）。
    // 注册表达上限（256）时**拒绝新增并返回空串**，不会淘汰既有句柄。

    // 从原始像素内存注册图像。fmt: bgra(默认) / bgr / gray（也接受 4/3/1）。
    // stride<=0 表示紧凑排列（width*bpp）；stride>width*bpp 视为带行填充，逐行复制。
    // 成功返回句柄字符串，失败返回空串（参数非法 / 未知格式 / 尺寸超 64MB / 注册表满）。
    void LoadImageMem(_In_ void *data, _In_ long width, _In_ long height, _In_ long stride,
                      _In_ const wchar_t *fmt, _Out_ std::wstring &ret_handle);
    // 从 BMP 字节流注册（自动解析 24/32bpp、top-down/bottom-up、行填充）。
    void LoadImageBmp(_In_ void *data, _In_ long size, _Out_ std::wstring &ret_handle);
    // 释放单个图像句柄。ret: 1=成功，0=句柄不存在。
    void FreeImage(_In_ const wchar_t *handle, _Out_ long *ret);
    // 释放全部图像句柄。ret: 被释放的数量。
    void FreeAllImages(_Out_ long *ret);
    // 查询图像尺寸。channels 恒为 4（内部统一 BGRA）。ret: 1=成功，0=句柄不存在。
    void GetImageSize(_In_ const wchar_t *handle, _Out_ long *width, _Out_ long *height, _Out_ long *channels,
                      _Out_ long *ret);
    // 读回像素。fmt: raw(BGRA 紧凑) / bmp(带 BMP 头的完整字节流)。
    // buffer 为调用方提供的缓冲区，buffer_size 为其容量；ret: 实际写出的字节数（不足则 0）。
    void GetImageData(_In_ const wchar_t *handle, _In_ const wchar_t *fmt, _In_ void *buffer,
                      _In_ long buffer_size, _Out_ long *ret);
    // 裁剪并注册为新句柄（深拷贝）。区域自动 clamp 到图像内；空区域返回空串。
    void CropImage(_In_ const wchar_t *handle, _In_ long x, _In_ long y, _In_ long width, _In_ long height,
                   _Out_ std::wstring &ret_handle);
    // 在图像句柄上做多目标查找（复用 P1 FindStrBatch 的匹配内核）。
    // 返回值 = 目标数；句柄无效或 JSON 非法返回 -1。坐标从图像左上角算起。
    long FindStrMem(_In_ const wchar_t *handle, _In_ const wchar_t *json_targets, _In_ double sim,
                    _Out_ std::wstring &ret_json);
    // 当前注册表条目数（诊断用）。
    void GetImageCount(_Out_ long *ret);
'''

# ---------------------------------------------------------------------------
OP_C_API_H_BLOCK = r'''
/* ===== 图像句柄注册表（P4 2026-11-21，内存直操作一期地基）=====
 * 目的：让调用方把**自己手上已有的像素**直接交给 op，省掉"落盘再读盘"的往返
 * （该链路实测 50~200ms，内存注册 <5ms）。
 *
 * 句柄是字符串 "img:<n>"，与既有 "pic:<路径>" 的显示输入风格一致，
 * 因此在 C/COM/Python 各层都是普通字符串，客户端零学习成本。
 *
 * 约定：
 *  - 注册后像素**不可变**（句柄可被多线程并发读；要变体就新建句柄）
 *  - 注册表上限 256 条，达上限**拒绝新增并返回空串**（不淘汰既有句柄）
 *  - 非法输入（nullptr/0 尺寸/未知格式/超 64MB）一律返回空串或 0，不崩溃
 */

/* 从原始像素内存注册。fmt: "bgra"(默认)/"bgr"/"gray"（也接受 "4"/"3"/"1"）。
 * stride<=0 表示紧凑排列；stride>width*bpp 视为带行填充，逐行复制。
 * 返回句柄字符串；失败返回空串。 */
OP_C_API const wchar_t *OP_CALL OpLoadImageMem(op_handle handle, const void *data, int width, int height,
                                                int stride, const wchar_t *fmt);
/* 从 BMP 字节流注册（支持 24/32bpp、top-down/bottom-up、行填充）。返回句柄；失败空串。 */
OP_C_API const wchar_t *OP_CALL OpLoadImageBmp(op_handle handle, const void *data, int size);
/* 释放单个图像句柄。返回 1=成功，0=句柄不存在。 */
OP_C_API int OP_CALL OpFreeImage(op_handle handle, const wchar_t *image_handle);
/* 释放全部图像句柄。返回被释放的数量。 */
OP_C_API int OP_CALL OpFreeAllImages(op_handle handle);
/* 查询图像尺寸。channels 恒为 4（内部统一 BGRA）。返回 1=成功，0=句柄不存在。 */
OP_C_API int OP_CALL OpGetImageSize(op_handle handle, const wchar_t *image_handle, int *width, int *height,
                                    int *channels);
/* 读回像素到调用方缓冲区。fmt: "raw"(BGRA 紧凑)/"bmp"(带 BMP 头)。
 * 返回实际写出的字节数；缓冲区不足或句柄无效返回 0。
 * 提示：先调 OpGetImageSize 算好容量（raw = w*h*4，bmp = w*h*4 + 54）。 */
OP_C_API int OP_CALL OpGetImageData(op_handle handle, const wchar_t *image_handle, const wchar_t *fmt,
                                    void *buffer, int buffer_size);
/* 裁剪并注册为新句柄（深拷贝）。区域自动 clamp；空区域返回空串。 */
OP_C_API const wchar_t *OP_CALL OpCropImage(op_handle handle, const wchar_t *image_handle, int x, int y, int width,
                                             int height);
/* 在图像句柄上做多目标查找（复用 OpFindStrBatch 的匹配内核与 JSON 格式）。
 * 返回 JSON；句柄无效或 JSON 非法返回 nullptr。坐标从图像左上角算起。 */
OP_C_API const wchar_t *OP_CALL OpFindStrMem(op_handle handle, const wchar_t *image_handle,
                                              const wchar_t *json_targets, double sim);
/* 当前注册表条目数（诊断用）。 */
OP_C_API int OP_CALL OpGetImageCount(op_handle handle);
'''

# ---------------------------------------------------------------------------
OP_IDL_BLOCK = r'''
	// ---- 图像句柄注册表（P4 2026-11-21，内存直操作一期地基）----
	// 句柄为字符串 "img:<n>"，与 pic:<路径> 风格一致。设计见
	// docs/2026-11/内存模块优化_图像句柄注册表_方案_20261121.md。
	// 约定：注册后像素不可变；上限 256 条，满则拒绝新增（不淘汰既有句柄）；
	//       非法输入返回空串/0，不抛异常。
	// LoadImageMem 的 data 传像素缓冲区首地址（易语言用取变量地址）。
	[id(445)] HRESULT LoadImageMem([in] LONG_PTR data, [in] LONG width, [in] LONG height, [in] LONG stride, [in] BSTR fmt, [out, retval] BSTR* retstr);
	[id(446)] HRESULT LoadImageBmp([in] LONG_PTR data, [in] LONG size, [out, retval] BSTR* retstr);
	[id(447)] HRESULT FreeImage([in] BSTR image_handle, [out, retval] LONG* ret);
	[id(448)] HRESULT FreeAllImages([out, retval] LONG* ret);
	[id(449)] HRESULT GetImageSize([in] BSTR image_handle, [in, out] LONG* width, [in, out] LONG* height, [in, out] LONG* channels, [out, retval] LONG* ret);
	[id(450)] HRESULT GetImageData([in] BSTR image_handle, [in] BSTR fmt, [in] LONG_PTR buffer, [in] LONG buffer_size, [out, retval] LONG* ret);
	[id(451)] HRESULT CropImage([in] BSTR image_handle, [in] LONG x, [in] LONG y, [in] LONG width, [in] LONG height, [out, retval] BSTR* retstr);
	[id(452)] HRESULT FindStrMem([in] BSTR image_handle, [in] BSTR json_targets, [in] DOUBLE sim, [out, retval] BSTR* retstr);
	[id(453)] HRESULT GetImageCount([out, retval] LONG* ret);
'''

# ---------------------------------------------------------------------------
OP_IMAGE_CPP_BLOCK = r'''
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
'''

# ---------------------------------------------------------------------------
OP_C_API_CPP_BLOCK = r'''
/* ---- P4（2026-11-21）图像句柄注册表：内存直操作一期地基 ---- */

const wchar_t *OP_CALL OpLoadImageMem(op_handle handle, const void *data, int width, int height, int stride,
                                       const wchar_t *fmt) {
    return call_string(handle, [&](op::Op &op, std::wstring &ret) {
        op.LoadImageMem(const_cast<void *>(data), width, height, stride, safe_text(fmt), ret);
    });
}

const wchar_t *OP_CALL OpLoadImageBmp(op_handle handle, const void *data, int size) {
    return call_string(handle, [&](op::Op &op, std::wstring &ret) {
        op.LoadImageBmp(const_cast<void *>(data), size, ret);
    });
}

int OP_CALL OpFreeImage(op_handle handle, const wchar_t *image_handle) {
    return call_ret(handle, [&](op::Op &op, long *ret) { op.FreeImage(safe_text(image_handle), ret); });
}

int OP_CALL OpFreeAllImages(op_handle handle) {
    return call_ret(handle, [](op::Op &op, long *ret) { op.FreeAllImages(ret); });
}

int OP_CALL OpGetImageSize(op_handle handle, const wchar_t *image_handle, int *width, int *height, int *channels) {
    out_int(width, 0);
    out_int(height, 0);
    out_int(channels, 0);
    return call_ret(handle, [&](op::Op &op, long *ret) {
        long w = 0, h = 0, c = 0;
        op.GetImageSize(safe_text(image_handle), &w, &h, &c, ret);
        out_int(width, w);
        out_int(height, h);
        out_int(channels, c);
    });
}

int OP_CALL OpGetImageData(op_handle handle, const wchar_t *image_handle, const wchar_t *fmt, void *buffer,
                           int buffer_size) {
    return call_ret(handle, [&](op::Op &op, long *ret) {
        op.GetImageData(safe_text(image_handle), safe_text(fmt), buffer, buffer_size, ret);
    });
}

const wchar_t *OP_CALL OpCropImage(op_handle handle, const wchar_t *image_handle, int x, int y, int width,
                                    int height) {
    return call_string(handle, [&](op::Op &op, std::wstring &ret) {
        op.CropImage(safe_text(image_handle), x, y, width, height, ret);
    });
}

const wchar_t *OP_CALL OpFindStrMem(op_handle handle, const wchar_t *image_handle, const wchar_t *json_targets,
                                     double sim) {
    if (!handle)
        return nullptr;
    try {
        handle->string_result.clear();
        const long rc = handle->op.FindStrMem(safe_text(image_handle), safe_text(json_targets), sim,
                                              handle->string_result);
        if (rc < 0)
            return nullptr;
        return handle->string_result.c_str();
    } catch (const std::exception &e) {
        setlog("op_c_api: exception in OpFindStrMem: %s", e.what());
        return nullptr;
    } catch (...) {
        setlog("op_c_api: unknown exception in OpFindStrMem");
        return nullptr;
    }
}

int OP_CALL OpGetImageCount(op_handle handle) {
    return call_ret(handle, [](op::Op &op, long *ret) { op.GetImageCount(ret); });
}
'''

# ---------------------------------------------------------------------------
OP_AUTOMATION_H_BLOCK = r'''
    // P4 图像句柄注册表（2026-11-21，内存直操作一期地基；idl id 445-453）
    STDMETHOD(LoadImageMem)(LONG_PTR data, LONG width, LONG height, LONG stride, BSTR fmt, BSTR *retstr);
    STDMETHOD(LoadImageBmp)(LONG_PTR data, LONG size, BSTR *retstr);
    STDMETHOD(FreeImage)(BSTR image_handle, LONG *ret);
    STDMETHOD(FreeAllImages)(LONG *ret);
    STDMETHOD(GetImageSize)(BSTR image_handle, LONG *width, LONG *height, LONG *channels, LONG *ret);
    STDMETHOD(GetImageData)(BSTR image_handle, BSTR fmt, LONG_PTR buffer, LONG buffer_size, LONG *ret);
    STDMETHOD(CropImage)(BSTR image_handle, LONG x, LONG y, LONG width, LONG height, BSTR *retstr);
    STDMETHOD(FindStrMem)(BSTR image_handle, BSTR json_targets, DOUBLE sim, BSTR *retstr);
    STDMETHOD(GetImageCount)(LONG *ret);
'''

OP_AUTOMATION_CPP_BLOCK = r'''
// ===== P4 图像句柄注册表（2026-11-21，内存直操作一期地基）=====

STDMETHODIMP OpAutomation::LoadImageMem(LONG_PTR data, LONG width, LONG height, LONG stride, BSTR fmt,
                                        BSTR *retstr) {
    if (!retstr)
        return E_POINTER;
    wstring s;
    obj.LoadImageMem(reinterpret_cast<void *>(data), width, height, stride, fmt, s);
    return CopyOutBstr(retstr, s);
}

STDMETHODIMP OpAutomation::LoadImageBmp(LONG_PTR data, LONG size, BSTR *retstr) {
    if (!retstr)
        return E_POINTER;
    wstring s;
    obj.LoadImageBmp(reinterpret_cast<void *>(data), size, s);
    return CopyOutBstr(retstr, s);
}

STDMETHODIMP OpAutomation::FreeImage(BSTR image_handle, LONG *ret) {
    if (!ret)
        return E_POINTER;
    *ret = 0;
    wstring handle_text = image_handle ? image_handle : L"";
    obj.FreeImage(handle_text.c_str(), ret);
    return S_OK;
}

STDMETHODIMP OpAutomation::FreeAllImages(LONG *ret) {
    if (!ret)
        return E_POINTER;
    *ret = 0;
    obj.FreeAllImages(ret);
    return S_OK;
}

STDMETHODIMP OpAutomation::GetImageSize(BSTR image_handle, LONG *width, LONG *height, LONG *channels, LONG *ret) {
    if (!ret)
        return E_POINTER;
    *ret = 0;
    LONG w = 0, h = 0, c = 0;
    wstring handle_text = image_handle ? image_handle : L"";
    obj.GetImageSize(handle_text.c_str(), &w, &h, &c, ret);
    if (width)
        *width = w;
    if (height)
        *height = h;
    if (channels)
        *channels = c;
    return S_OK;
}

STDMETHODIMP OpAutomation::GetImageData(BSTR image_handle, BSTR fmt, LONG_PTR buffer, LONG buffer_size, LONG *ret) {
    if (!ret)
        return E_POINTER;
    *ret = 0;
    wstring handle_text = image_handle ? image_handle : L"";
    wstring fmt_text = fmt ? fmt : L"";
    obj.GetImageData(handle_text.c_str(), fmt_text.c_str(), reinterpret_cast<void *>(buffer), buffer_size, ret);
    return S_OK;
}

STDMETHODIMP OpAutomation::CropImage(BSTR image_handle, LONG x, LONG y, LONG width, LONG height, BSTR *retstr) {
    if (!retstr)
        return E_POINTER;
    wstring s;
    wstring handle_text = image_handle ? image_handle : L"";
    obj.CropImage(handle_text.c_str(), x, y, width, height, s);
    return CopyOutBstr(retstr, s);
}

STDMETHODIMP OpAutomation::FindStrMem(BSTR image_handle, BSTR json_targets, DOUBLE sim, BSTR *retstr) {
    if (!retstr)
        return E_POINTER;
    wstring s;
    wstring handle_text = image_handle ? image_handle : L"";
    wstring targets_text = json_targets ? json_targets : L"";
    obj.FindStrMem(handle_text.c_str(), targets_text.c_str(), static_cast<double>(sim), s);
    return CopyOutBstr(retstr, s);
}

STDMETHODIMP OpAutomation::GetImageCount(LONG *ret) {
    if (!ret)
        return E_POINTER;
    *ret = 0;
    obj.GetImageCount(ret);
    return S_OK;
}
'''


def read(p):
    with io.open(p, "r", encoding="utf-8", newline="") as f:
        return f.read()


def write_checked(path, text, backup_text, must_contain):
    with io.open(path + ".bak", "w", encoding="utf-8", newline="") as f:
        f.write(backup_text)
    with io.open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    got = read(path)
    ok = all(k in got for k in must_contain)
    if not ok:
        with io.open(path, "w", encoding="utf-8", newline="") as f:
            f.write(backup_text)
        print("  ROLLBACK:", os.path.basename(path), "missing", [k for k in must_contain if k not in got])
        return False
    os.remove(path + ".bak")
    return True


def insert_before(path, marker, block, must_contain, expect_min_lines, after=False):
    """在 marker 之前（after=True 时之后）插入 block（marker 必须唯一出现）。"""
    if not os.path.isfile(path):
        print("  FAIL: not found", path)
        return False
    text = read(path)
    nl = "\r\n" if "\r\n" in text else "\n"
    body = block.replace("\r\n", "\n").replace("\n", nl)
    lines = text.count("\n")
    if lines < expect_min_lines:
        print("  FAIL: file looks truncated (", lines, "lines )")
        return False
    if must_contain[0] in text and block.split("\n")[1].strip() and block.split("\n")[1].strip() in text:
        print("  SKIP already present:", os.path.basename(path))
        return True
    cnt = text.count(marker)
    if cnt != 1:
        print("  FAIL: marker count =", cnt, "in", os.path.basename(path))
        return False
    idx = text.index(marker)
    if after:
        pos = idx + len(marker)
        new_text = text[:pos] + body + text[pos:]
    else:
        new_text = text[:idx] + body + text[idx:]
    return write_checked(path, new_text, text, must_contain + [marker])


def append_at_end(path, block, must_contain):
    if not os.path.isfile(path):
        print("  FAIL: not found", path)
        return False
    text = read(path)
    nl = "\r\n" if "\r\n" in text else "\n"
    body = block.replace("\r\n", "\n").replace("\n", nl)
    new_text = text + body
    return write_checked(path, new_text, text, must_contain)


def main():
    ok = True

    print("[1/6] include/libop.h")
    ok &= insert_before(
        LIBOP_H,
        "    void FindStr(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ const wchar_t *strs, _In_ const wchar_t *color,",
        LIBOP_H_BLOCK,
        ["LoadImageMem", "GetImageCount", "FindStrMem"],
        500,
    )

    print("[2/6] include/op_c_api.h")
    ok &= insert_before(
        OP_C_API_H,
        "OP_C_API int OP_CALL OpFindStr(",
        OP_C_API_H_BLOCK,
        ["OpLoadImageMem", "OpFindStrMem", "OpGetImageCount"],
        400,
    )

    print("[3/6] libop/com/op.idl")
    ok &= insert_before(
        OP_IDL,
        "\t[id(444)] HRESULT OcrLastProfile([out, retval] BSTR* retstr);",
        OP_IDL_BLOCK,
        ["LoadImageMem", "GetImageCount", "id(453)"],
        300,
        after=True,
    )

    print("[4/6] libop/op/OpImage.cpp (append)")
    ok &= append_at_end(OP_IMAGE_CPP, OP_IMAGE_CPP_BLOCK, ["op::Op::LoadImageMem", "op::Op::FindStrMem"])

    print("[5/6] libop/c_api/op_c_api.cpp (append)")
    ok &= append_at_end(OP_C_API_CPP, OP_C_API_CPP_BLOCK, ["OpLoadImageMem", "OpGetImageCount"])

    print("[6/6] OpAutomation.h / .cpp")
    ok &= insert_before(
        OP_AUTOMATION_H,
        "    //-----------------------opcv---------------------------------",
        OP_AUTOMATION_H_BLOCK,
        ["STDMETHOD(LoadImageMem)", "STDMETHOD(GetImageCount)"],
        400,
    )
    ok &= append_at_end(OP_AUTOMATION_CPP, OP_AUTOMATION_CPP_BLOCK, ["OpAutomation::LoadImageMem", "OpAutomation::GetImageCount"])

    print("\nRESULT:", "OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
