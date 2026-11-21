# 内存直操作二期：三层声明 + CMake 接线（2026-11-21）
# 用法：python scripts\probes\insert_memimage_api.py
#
# 关键教训（YOLO 阶段踩过）：
#   C API 声明**必须插进 op_c_api.h 的 extern "C" 块内**，否则 x64 下变成 C++ 修饰名，
#   C++ 测试照常全绿但 Python/易语言调不到。本脚本显式在闭合前插入并校验位置。

import io
import os
import sys

REPO = r"D:\AutoPro\op-master\op"

LIBOP_H = os.path.join(REPO, "include", "libop.h")
OP_C_API_H = os.path.join(REPO, "include", "op_c_api.h")
OP_IDL = os.path.join(REPO, "libop", "com", "op.idl")
OP_C_API_CPP = os.path.join(REPO, "libop", "c_api", "op_c_api.cpp")
AUTOMATION_H = os.path.join(REPO, "libop", "com", "OpAutomation.h")
AUTOMATION_CPP = os.path.join(REPO, "libop", "com", "OpAutomation.cpp")
CMAKELISTS = os.path.join(REPO, "libop", "CMakeLists.txt")

# DISPID 464~477
LIBOP_H_BLOCK = r'''
    // ===== 内存直操作二期：识别入口的内存版（2026-11-21）=====
    // 设计见 docs/2026-11/内存直操作二期_全家桶_方案_20261121.md
    //
    // 与屏幕版的**唯一区别**是"图从哪来"：
    //   屏幕版 x1..y2 是客户区坐标；内存版 x1..y2 是**图像内坐标**（相对 img 句柄左上角）。
    //   其余参数逐字相同，出参坐标也是图像内坐标（不受窗口偏移影响）。
    //   区域传 (0,0,0,0) 表示**整张图**（脚本里"全图查找"的自然写法）。
    //
    // 实现上直接复用屏幕版：只把"截屏"换成"从句柄取图"，偏移清零。
    // 因此行为与屏幕版严格一致，这是结构保证而非测试推断。
    //
    // 句柄无效/已释放时不执行识别，返回与屏幕版"截屏失败"相同的默认值。

    // 结构化 OCR（逐字坐标 + conf），输出与 OcrEx2 完全相同
    void OcrFromMemEx2(_In_ const wchar_t *image_handle, _In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2,
                       _In_ const wchar_t *color, _In_ double sim, _Out_ std::wstring &retjson);
    // 颜色二值化 OCR（含智能选路），返回值 = 行数
    long OcrAutoColorFromMemEx2(_In_ const wchar_t *image_handle, _In_ long x1, _In_ long y1, _In_ long x2,
                                _In_ long y2, _In_ const wchar_t *color, _In_ double sim,
                                _Out_ std::wstring &retjson);

    // 找图：ret = 命中的图片序号（-1=未找到，大漠索引语义）
    void FindPicFromMem(_In_ const wchar_t *image_handle, _In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2,
                        _In_ const wchar_t *files, _In_ const wchar_t *delta_color, _In_ double sim, _In_ long dir,
                        _Out_ long *x, _Out_ long *y, _Out_ long *ret);
    void FindPicExFromMem(_In_ const wchar_t *image_handle, _In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2,
                          _In_ const wchar_t *files, _In_ const wchar_t *delta_color, _In_ double sim, _In_ long dir,
                          _Out_ std::wstring &retstr);

    // 找色 / 取色 / 比色 / 计数
    void FindColorFromMem(_In_ const wchar_t *image_handle, _In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2,
                          _In_ const wchar_t *color, _In_ double sim, _In_ long dir, _Out_ long *x, _Out_ long *y,
                          _Out_ long *ret);
    void FindColorExFromMem(_In_ const wchar_t *image_handle, _In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2,
                            _In_ const wchar_t *color, _In_ double sim, _In_ long dir, _Out_ std::wstring &retstr);
    void GetColorFromMem(_In_ const wchar_t *image_handle, _In_ long x, _In_ long y, _Out_ std::wstring &retstr);
    void CmpColorFromMem(_In_ const wchar_t *image_handle, _In_ long x, _In_ long y, _In_ const wchar_t *color,
                         _In_ double sim, _Out_ long *ret);
    void GetColorNumFromMem(_In_ const wchar_t *image_handle, _In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2,
                            _In_ const wchar_t *color, _In_ double sim, _Out_ long *ret);

    // 多色串 / 色块
    void FindMultiColorFromMem(_In_ const wchar_t *image_handle, _In_ long x1, _In_ long y1, _In_ long x2,
                               _In_ long y2, _In_ const wchar_t *first_color, _In_ const wchar_t *offset_color,
                               _In_ double sim, _In_ long dir, _Out_ long *x, _Out_ long *y, _Out_ long *ret);
    void FindMultiColorExFromMem(_In_ const wchar_t *image_handle, _In_ long x1, _In_ long y1, _In_ long x2,
                                 _In_ long y2, _In_ const wchar_t *first_color, _In_ const wchar_t *offset_color,
                                 _In_ double sim, _In_ long dir, _Out_ std::wstring &retstr);
    void FindColorBlockExFromMem(_In_ const wchar_t *image_handle, _In_ long x1, _In_ long y1, _In_ long x2,
                                 _In_ long y2, _In_ const wchar_t *color, _In_ double sim, _In_ long count,
                                 _In_ long height, _In_ long width, _Out_ std::wstring &retstr);

    // YOLO（内存版）。ret = 检测条数；JSON 结构同屏幕版
    void YoloDetectFromMem(_In_ const wchar_t *image_handle, _In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2,
                           _In_ double conf, _In_ double iou, _Out_ std::wstring &retjson, _Out_ long *ret);
    // 带过滤/截断；json_options 同 YoloDetectEx2。非法 options 返回 -1
    long YoloDetectFromMemEx2(_In_ const wchar_t *image_handle, _In_ long x1, _In_ long y1, _In_ long x2,
                              _In_ long y2, _In_ double conf, _In_ double iou, _In_ const wchar_t *json_options,
                              _Out_ std::wstring &retjson, _Out_ long *ret);
'''

OP_C_API_H_BLOCK = r'''
/* ===== 内存直操作二期：识别入口的内存版（2026-11-21）=====
 * 设计见 docs/2026-11/内存直操作二期_全家桶_方案_20261121.md
 *
 * 与屏幕版的唯一区别是"图从哪来"：
 *   屏幕版 x1..y2 是客户区坐标；内存版 x1..y2 是**图像内坐标**（相对 img: 句柄左上角）。
 *   其余参数逐字相同；出参坐标也是图像内坐标。
 *   区域传 (0,0,0,0) 表示**整张图**。
 *
 * 实现直接复用屏幕版（只把截屏换成从句柄取图、偏移清零），
 * 所以行为严格一致。句柄无效时返回与"截屏失败"相同的默认值。
 */
OP_C_API const wchar_t *OP_CALL OpOcrFromMemEx2(op_handle handle, const wchar_t *image_handle, int x1, int y1,
                                                 int x2, int y2, const wchar_t *color, double sim);
OP_C_API const wchar_t *OP_CALL OpOcrAutoColorFromMemEx2(op_handle handle, const wchar_t *image_handle, int x1,
                                                          int y1, int x2, int y2, const wchar_t *color, double sim,
                                                          int *find_count);
OP_C_API int OP_CALL OpFindPicFromMem(op_handle handle, const wchar_t *image_handle, int x1, int y1, int x2, int y2,
                                      const wchar_t *files, const wchar_t *delta_color, double sim, int dir, int *x,
                                      int *y);
OP_C_API const wchar_t *OP_CALL OpFindPicExFromMem(op_handle handle, const wchar_t *image_handle, int x1, int y1,
                                                     int x2, int y2, const wchar_t *files, const wchar_t *delta_color,
                                                     double sim, int dir);
OP_C_API int OP_CALL OpFindColorFromMem(op_handle handle, const wchar_t *image_handle, int x1, int y1, int x2, int y2,
                                        const wchar_t *color, double sim, int dir, int *x, int *y);
OP_C_API const wchar_t *OP_CALL OpFindColorExFromMem(op_handle handle, const wchar_t *image_handle, int x1, int y1,
                                                      int x2, int y2, const wchar_t *color, double sim, int dir);
OP_C_API const wchar_t *OP_CALL OpGetColorFromMem(op_handle handle, const wchar_t *image_handle, int x, int y);
OP_C_API int OP_CALL OpCmpColorFromMem(op_handle handle, const wchar_t *image_handle, int x, int y,
                                       const wchar_t *color, double sim);
OP_C_API int OP_CALL OpGetColorNumFromMem(op_handle handle, const wchar_t *image_handle, int x1, int y1, int x2, int y2,
                                          const wchar_t *color, double sim);
OP_C_API int OP_CALL OpFindMultiColorFromMem(op_handle handle, const wchar_t *image_handle, int x1, int y1, int x2,
                                              int y2, const wchar_t *first_color, const wchar_t *offset_color,
                                              double sim, int dir, int *x, int *y);
OP_C_API const wchar_t *OP_CALL OpFindMultiColorExFromMem(op_handle handle, const wchar_t *image_handle, int x1, int y1,
                                                           int x2, int y2, const wchar_t *first_color,
                                                           const wchar_t *offset_color, double sim, int dir);
OP_C_API const wchar_t *OP_CALL OpFindColorBlockExFromMem(op_handle handle, const wchar_t *image_handle, int x1,
                                                           int y1, int x2, int y2, const wchar_t *color, double sim,
                                                           int count, int height, int width);
OP_C_API const wchar_t *OP_CALL OpYoloDetectFromMem(op_handle handle, const wchar_t *image_handle, int x1, int y1,
                                                     int x2, int y2, double conf, double iou, int *find_count);
OP_C_API const wchar_t *OP_CALL OpYoloDetectFromMemEx2(op_handle handle, const wchar_t *image_handle, int x1, int y1,
                                                        int x2, int y2, double conf, double iou,
                                                        const wchar_t *json_options, int *find_count);
'''

OP_IDL_BLOCK = r'''
	// ---- 内存直操作二期（2026-11-21）：识别入口的内存版 ----
	// 与屏幕版唯一区别：x1..y2 是**图像内坐标**（相对 img: 句柄左上角），出参也是图像内坐标。
	// 区域 (0,0,0,0) 表示整张图。句柄无效时返回与"截屏失败"相同的默认值。
	[id(464)] HRESULT OcrFromMemEx2([in] BSTR image_handle, [in] LONG x1, [in] LONG y1, [in] LONG x2, [in] LONG y2, [in] BSTR color, [in] DOUBLE sim, [out, retval] BSTR* retstr);
	[id(465)] HRESULT OcrAutoColorFromMemEx2([in] BSTR image_handle, [in] LONG x1, [in] LONG y1, [in] LONG x2, [in] LONG y2, [in] BSTR color, [in] DOUBLE sim, [out] BSTR* retstr, [out, retval] LONG* ret);
	[id(466)] HRESULT FindPicFromMem([in] BSTR image_handle, [in] LONG x1, [in] LONG y1, [in] LONG x2, [in] LONG y2, [in] BSTR files, [in] BSTR delta_color, [in] DOUBLE sim, [in] LONG dir, [in, out] LONG* x, [in, out] LONG* y, [out, retval] LONG* ret);
	[id(467)] HRESULT FindPicExFromMem([in] BSTR image_handle, [in] LONG x1, [in] LONG y1, [in] LONG x2, [in] LONG y2, [in] BSTR files, [in] BSTR delta_color, [in] DOUBLE sim, [in] LONG dir, [out, retval] BSTR* retstr);
	[id(468)] HRESULT FindColorFromMem([in] BSTR image_handle, [in] LONG x1, [in] LONG y1, [in] LONG x2, [in] LONG y2, [in] BSTR color, [in] DOUBLE sim, [in] LONG dir, [in, out] LONG* x, [in, out] LONG* y, [out, retval] LONG* ret);
	[id(469)] HRESULT FindColorExFromMem([in] BSTR image_handle, [in] LONG x1, [in] LONG y1, [in] LONG x2, [in] LONG y2, [in] BSTR color, [in] DOUBLE sim, [in] LONG dir, [out, retval] BSTR* retstr);
	[id(470)] HRESULT GetColorFromMem([in] BSTR image_handle, [in] LONG x, [in] LONG y, [out, retval] BSTR* retstr);
	[id(471)] HRESULT CmpColorFromMem([in] BSTR image_handle, [in] LONG x, [in] LONG y, [in] BSTR color, [in] DOUBLE sim, [out, retval] LONG* ret);
	[id(472)] HRESULT GetColorNumFromMem([in] BSTR image_handle, [in] LONG x1, [in] LONG y1, [in] LONG x2, [in] LONG y2, [in] BSTR color, [in] DOUBLE sim, [out, retval] LONG* ret);
	[id(473)] HRESULT FindMultiColorFromMem([in] BSTR image_handle, [in] LONG x1, [in] LONG y1, [in] LONG x2, [in] LONG y2, [in] BSTR first_color, [in] BSTR offset_color, [in] DOUBLE sim, [in] LONG dir, [in, out] LONG* x, [in, out] LONG* y, [out, retval] LONG* ret);
	[id(474)] HRESULT FindMultiColorExFromMem([in] BSTR image_handle, [in] LONG x1, [in] LONG y1, [in] LONG x2, [in] LONG y2, [in] BSTR first_color, [in] BSTR offset_color, [in] DOUBLE sim, [in] LONG dir, [out, retval] BSTR* retstr);
	[id(475)] HRESULT FindColorBlockExFromMem([in] BSTR image_handle, [in] LONG x1, [in] LONG y1, [in] LONG x2, [in] LONG y2, [in] BSTR color, [in] DOUBLE sim, [in] LONG count, [in] LONG height, [in] LONG width, [out, retval] BSTR* retstr);
	[id(476)] HRESULT YoloDetectFromMem([in] BSTR image_handle, [in] LONG x1, [in] LONG y1, [in] LONG x2, [in] LONG y2, [in] DOUBLE conf, [in] DOUBLE iou, [out] BSTR* retstr, [out, retval] LONG* ret);
	[id(477)] HRESULT YoloDetectFromMemEx2([in] BSTR image_handle, [in] LONG x1, [in] LONG y1, [in] LONG x2, [in] LONG y2, [in] DOUBLE conf, [in] DOUBLE iou, [in] BSTR json_options, [out] BSTR* retstr, [out, retval] LONG* ret);
'''

OP_C_API_CPP_BLOCK = r'''
/* ---- 内存直操作二期：识别入口的内存版（2026-11-21） ---- */

const wchar_t *OP_CALL OpOcrFromMemEx2(op_handle handle, const wchar_t *image_handle, int x1, int y1, int x2, int y2,
                                        const wchar_t *color, double sim) {
    return call_string(handle, [&](op::Op &op, std::wstring &ret) {
        op.OcrFromMemEx2(safe_text(image_handle), x1, y1, x2, y2, safe_text(color), sim, ret);
    });
}

const wchar_t *OP_CALL OpOcrAutoColorFromMemEx2(op_handle handle, const wchar_t *image_handle, int x1, int y1, int x2,
                                                 int y2, const wchar_t *color, double sim, int *find_count) {
    long ct = 0;
    const wchar_t *ret = call_string(handle, [&](op::Op &op, std::wstring &s) {
        ct = op.OcrAutoColorFromMemEx2(safe_text(image_handle), x1, y1, x2, y2, safe_text(color), sim, s);
    });
    out_int(find_count, ct);
    return ret;
}

int OP_CALL OpFindPicFromMem(op_handle handle, const wchar_t *image_handle, int x1, int y1, int x2, int y2,
                             const wchar_t *files, const wchar_t *delta_color, double sim, int dir, int *x, int *y) {
    return call_ret(handle, [&](op::Op &op, long *ret) {
        long lx = -1, ly = -1;
        op.FindPicFromMem(safe_text(image_handle), x1, y1, x2, y2, safe_text(files), safe_text(delta_color), sim, dir,
                          &lx, &ly, ret);
        out_int(x, lx);
        out_int(y, ly);
    });
}

const wchar_t *OP_CALL OpFindPicExFromMem(op_handle handle, const wchar_t *image_handle, int x1, int y1, int x2,
                                           int y2, const wchar_t *files, const wchar_t *delta_color, double sim,
                                           int dir) {
    return call_string(handle, [&](op::Op &op, std::wstring &ret) {
        op.FindPicExFromMem(safe_text(image_handle), x1, y1, x2, y2, safe_text(files), safe_text(delta_color), sim,
                            dir, ret);
    });
}

int OP_CALL OpFindColorFromMem(op_handle handle, const wchar_t *image_handle, int x1, int y1, int x2, int y2,
                               const wchar_t *color, double sim, int dir, int *x, int *y) {
    return call_ret(handle, [&](op::Op &op, long *ret) {
        long lx = -1, ly = -1;
        op.FindColorFromMem(safe_text(image_handle), x1, y1, x2, y2, safe_text(color), sim, dir, &lx, &ly, ret);
        out_int(x, lx);
        out_int(y, ly);
    });
}

const wchar_t *OP_CALL OpFindColorExFromMem(op_handle handle, const wchar_t *image_handle, int x1, int y1, int x2,
                                             int y2, const wchar_t *color, double sim, int dir) {
    return call_string(handle, [&](op::Op &op, std::wstring &ret) {
        op.FindColorExFromMem(safe_text(image_handle), x1, y1, x2, y2, safe_text(color), sim, dir, ret);
    });
}

const wchar_t *OP_CALL OpGetColorFromMem(op_handle handle, const wchar_t *image_handle, int x, int y) {
    return call_string(handle, [&](op::Op &op, std::wstring &ret) {
        op.GetColorFromMem(safe_text(image_handle), x, y, ret);
    });
}

int OP_CALL OpCmpColorFromMem(op_handle handle, const wchar_t *image_handle, int x, int y, const wchar_t *color,
                              double sim) {
    return call_ret(handle, [&](op::Op &op, long *ret) {
        op.CmpColorFromMem(safe_text(image_handle), x, y, safe_text(color), sim, ret);
    });
}

int OP_CALL OpGetColorNumFromMem(op_handle handle, const wchar_t *image_handle, int x1, int y1, int x2, int y2,
                                 const wchar_t *color, double sim) {
    return call_ret(handle, [&](op::Op &op, long *ret) {
        op.GetColorNumFromMem(safe_text(image_handle), x1, y1, x2, y2, safe_text(color), sim, ret);
    });
}

int OP_CALL OpFindMultiColorFromMem(op_handle handle, const wchar_t *image_handle, int x1, int y1, int x2, int y2,
                                    const wchar_t *first_color, const wchar_t *offset_color, double sim, int dir,
                                    int *x, int *y) {
    return call_ret(handle, [&](op::Op &op, long *ret) {
        long lx = -1, ly = -1;
        op.FindMultiColorFromMem(safe_text(image_handle), x1, y1, x2, y2, safe_text(first_color), safe_text(offset_color),
                                 sim, dir, &lx, &ly, ret);
        out_int(x, lx);
        out_int(y, ly);
    });
}

const wchar_t *OP_CALL OpFindMultiColorExFromMem(op_handle handle, const wchar_t *image_handle, int x1, int y1, int x2,
                                                  int y2, const wchar_t *first_color, const wchar_t *offset_color,
                                                  double sim, int dir) {
    return call_string(handle, [&](op::Op &op, std::wstring &ret) {
        op.FindMultiColorExFromMem(safe_text(image_handle), x1, y1, x2, y2, safe_text(first_color),
                                   safe_text(offset_color), sim, dir, ret);
    });
}

const wchar_t *OP_CALL OpFindColorBlockExFromMem(op_handle handle, const wchar_t *image_handle, int x1, int y1,
                                                  int x2, int y2, const wchar_t *color, double sim, int count,
                                                  int height, int width) {
    return call_string(handle, [&](op::Op &op, std::wstring &ret) {
        op.FindColorBlockExFromMem(safe_text(image_handle), x1, y1, x2, y2, safe_text(color), sim, count, height, width,
                                   ret);
    });
}

const wchar_t *OP_CALL OpYoloDetectFromMem(op_handle handle, const wchar_t *image_handle, int x1, int y1, int x2,
                                            int y2, double conf, double iou, int *find_count) {
    long ct = 0;
    const wchar_t *ret = call_string(handle, [&](op::Op &op, std::wstring &s) {
        op.YoloDetectFromMem(safe_text(image_handle), x1, y1, x2, y2, conf, iou, s, &ct);
    });
    out_int(find_count, ct);
    return ret;
}

const wchar_t *OP_CALL OpYoloDetectFromMemEx2(op_handle handle, const wchar_t *image_handle, int x1, int y1, int x2,
                                               int y2, double conf, double iou, const wchar_t *json_options,
                                               int *find_count) {
    if (!handle) {
        out_int(find_count, 0);
        return nullptr;
    }
    try {
        handle->string_result.clear();
        long found = 0;
        const long rc = handle->op.YoloDetectFromMemEx2(safe_text(image_handle), x1, y1, x2, y2, conf, iou,
                                                         safe_text(json_options), handle->string_result, &found);
        out_int(find_count, found);
        if (rc < 0)
            return nullptr; // 参数非法
        return handle->string_result.c_str();
    } catch (const std::exception &e) {
        setlog("op_c_api: exception in OpYoloDetectFromMemEx2: %s", e.what());
        out_int(find_count, 0);
        return nullptr;
    } catch (...) {
        setlog("op_c_api: unknown exception in OpYoloDetectFromMemEx2");
        out_int(find_count, 0);
        return nullptr;
    }
}
'''

AUTOMATION_H_BLOCK = r'''
    // 内存直操作二期（2026-11-21，idl id 464-477）：识别入口的内存版
    STDMETHOD(OcrFromMemEx2)(BSTR image_handle, LONG x1, LONG y1, LONG x2, LONG y2, BSTR color, DOUBLE sim,
                             BSTR *retstr);
    STDMETHOD(OcrAutoColorFromMemEx2)(BSTR image_handle, LONG x1, LONG y1, LONG x2, LONG y2, BSTR color, DOUBLE sim,
                                      BSTR *retstr, LONG *ret);
    STDMETHOD(FindPicFromMem)(BSTR image_handle, LONG x1, LONG y1, LONG x2, LONG y2, BSTR files, BSTR delta_color,
                              DOUBLE sim, LONG dir, LONG *x, LONG *y, LONG *ret);
    STDMETHOD(FindPicExFromMem)(BSTR image_handle, LONG x1, LONG y1, LONG x2, LONG y2, BSTR files, BSTR delta_color,
                                DOUBLE sim, LONG dir, BSTR *retstr);
    STDMETHOD(FindColorFromMem)(BSTR image_handle, LONG x1, LONG y1, LONG x2, LONG y2, BSTR color, DOUBLE sim,
                                LONG dir, LONG *x, LONG *y, LONG *ret);
    STDMETHOD(FindColorExFromMem)(BSTR image_handle, LONG x1, LONG y1, LONG x2, LONG y2, BSTR color, DOUBLE sim,
                                  LONG dir, BSTR *retstr);
    STDMETHOD(GetColorFromMem)(BSTR image_handle, LONG x, LONG y, BSTR *retstr);
    STDMETHOD(CmpColorFromMem)(BSTR image_handle, LONG x, LONG y, BSTR color, DOUBLE sim, LONG *ret);
    STDMETHOD(GetColorNumFromMem)(BSTR image_handle, LONG x1, LONG y1, LONG x2, LONG y2, BSTR color, DOUBLE sim,
                                  LONG *ret);
    STDMETHOD(FindMultiColorFromMem)(BSTR image_handle, LONG x1, LONG y1, LONG x2, LONG y2, BSTR first_color,
                                     BSTR offset_color, DOUBLE sim, LONG dir, LONG *x, LONG *y, LONG *ret);
    STDMETHOD(FindMultiColorExFromMem)(BSTR image_handle, LONG x1, LONG y1, LONG x2, LONG y2, BSTR first_color,
                                       BSTR offset_color, DOUBLE sim, LONG dir, BSTR *retstr);
    STDMETHOD(FindColorBlockExFromMem)(BSTR image_handle, LONG x1, LONG y1, LONG x2, LONG y2, BSTR color, DOUBLE sim,
                                       LONG count, LONG height, LONG width, BSTR *retstr);
    STDMETHOD(YoloDetectFromMem)(BSTR image_handle, LONG x1, LONG y1, LONG x2, LONG y2, DOUBLE conf, DOUBLE iou,
                                 BSTR *retstr, LONG *ret);
    STDMETHOD(YoloDetectFromMemEx2)(BSTR image_handle, LONG x1, LONG y1, LONG x2, LONG y2, DOUBLE conf, DOUBLE iou,
                                    BSTR json_options, BSTR *retstr, LONG *ret);
'''

AUTOMATION_CPP_BLOCK = r'''
// ===== 内存直操作二期：识别入口的内存版（2026-11-21）=====

STDMETHODIMP OpAutomation::OcrFromMemEx2(BSTR image_handle, LONG x1, LONG y1, LONG x2, LONG y2, BSTR color, DOUBLE sim,
                                        BSTR *retstr) {
    if (!retstr)
        return E_POINTER;
    wstring s;
    obj.OcrFromMemEx2(image_handle, x1, y1, x2, y2, color, static_cast<double>(sim), s);
    return CopyOutBstr(retstr, s);
}

STDMETHODIMP OpAutomation::OcrAutoColorFromMemEx2(BSTR image_handle, LONG x1, LONG y1, LONG x2, LONG y2, BSTR color,
                                                  DOUBLE sim, BSTR *retstr, LONG *ret) {
    if (!retstr || !ret)
        return E_POINTER;
    *ret = 0;
    wstring s;
    *ret = obj.OcrAutoColorFromMemEx2(image_handle, x1, y1, x2, y2, color, static_cast<double>(sim), s);
    return CopyOutBstr(retstr, s);
}

STDMETHODIMP OpAutomation::FindPicFromMem(BSTR image_handle, LONG x1, LONG y1, LONG x2, LONG y2, BSTR files,
                                          BSTR delta_color, DOUBLE sim, LONG dir, LONG *x, LONG *y, LONG *ret) {
    if (!ret)
        return E_POINTER;
    long lx = -1, ly = -1;
    *ret = obj.FindPicFromMem(image_handle, x1, y1, x2, y2, files, delta_color, static_cast<double>(sim), dir, &lx,
                              &ly, nullptr);
    if (x)
        *x = lx;
    if (y)
        *y = ly;
    return S_OK;
}

STDMETHODIMP OpAutomation::FindPicExFromMem(BSTR image_handle, LONG x1, LONG y1, LONG x2, LONG y2, BSTR files,
                                            BSTR delta_color, DOUBLE sim, LONG dir, BSTR *retstr) {
    if (!retstr)
        return E_POINTER;
    wstring s;
    obj.FindPicExFromMem(image_handle, x1, y1, x2, y2, files, delta_color, static_cast<double>(sim), dir, s);
    return CopyOutBstr(retstr, s);
}

STDMETHODIMP OpAutomation::FindColorFromMem(BSTR image_handle, LONG x1, LONG y1, LONG x2, LONG y2, BSTR color,
                                           DOUBLE sim, LONG dir, LONG *x, LONG *y, LONG *ret) {
    if (!ret)
        return E_POINTER;
    long lx = -1, ly = -1;
    *ret = obj.FindColorFromMem(image_handle, x1, y1, x2, y2, color, static_cast<double>(sim), dir, &lx, &ly, nullptr);
    if (x)
        *x = lx;
    if (y)
        *y = ly;
    return S_OK;
}

STDMETHODIMP OpAutomation::FindColorExFromMem(BSTR image_handle, LONG x1, LONG y1, LONG x2, LONG y2, BSTR color,
                                             DOUBLE sim, LONG dir, BSTR *retstr) {
    if (!retstr)
        return E_POINTER;
    wstring s;
    obj.FindColorExFromMem(image_handle, x1, y1, x2, y2, color, static_cast<double>(sim), dir, s);
    return CopyOutBstr(retstr, s);
}

STDMETHODIMP OpAutomation::GetColorFromMem(BSTR image_handle, LONG x, LONG y, BSTR *retstr) {
    if (!retstr)
        return E_POINTER;
    wstring s;
    obj.GetColorFromMem(image_handle, x, y, s);
    return CopyOutBstr(retstr, s);
}

STDMETHODIMP OpAutomation::CmpColorFromMem(BSTR image_handle, LONG x, LONG y, BSTR color, DOUBLE sim, LONG *ret) {
    if (!ret)
        return E_POINTER;
    *ret = 0;
    obj.CmpColorFromMem(image_handle, x, y, color, static_cast<double>(sim), ret);
    return S_OK;
}

STDMETHODIMP OpAutomation::GetColorNumFromMem(BSTR image_handle, LONG x1, LONG y1, LONG x2, LONG y2, BSTR color,
                                              DOUBLE sim, LONG *ret) {
    if (!ret)
        return E_POINTER;
    *ret = 0;
    obj.GetColorNumFromMem(image_handle, x1, y1, x2, y2, color, static_cast<double>(sim), ret);
    return S_OK;
}

STDMETHODIMP OpAutomation::FindMultiColorFromMem(BSTR image_handle, LONG x1, LONG y1, LONG x2, LONG y2, BSTR first_color,
                                                 BSTR offset_color, DOUBLE sim, LONG dir, LONG *x, LONG *y, LONG *ret) {
    if (!ret)
        return E_POINTER;
    long lx = -1, ly = -1;
    *ret = obj.FindMultiColorFromMem(image_handle, x1, y1, x2, y2, first_color, offset_color, static_cast<double>(sim),
                                     dir, &lx, &ly, nullptr);
    if (x)
        *x = lx;
    if (y)
        *y = ly;
    return S_OK;
}

STDMETHODIMP OpAutomation::FindMultiColorExFromMem(BSTR image_handle, LONG x1, LONG y1, LONG x2, LONG y2,
                                                   BSTR first_color, BSTR offset_color, DOUBLE sim, LONG dir,
                                                   BSTR *retstr) {
    if (!retstr)
        return E_POINTER;
    wstring s;
    obj.FindMultiColorExFromMem(image_handle, x1, y1, x2, y2, first_color, offset_color, static_cast<double>(sim), dir,
                                s);
    return CopyOutBstr(retstr, s);
}

STDMETHODIMP OpAutomation::FindColorBlockExFromMem(BSTR image_handle, LONG x1, LONG y1, LONG x2, LONG y2, BSTR color,
                                                   DOUBLE sim, LONG count, LONG height, LONG width, BSTR *retstr) {
    if (!retstr)
        return E_POINTER;
    wstring s;
    obj.FindColorBlockExFromMem(image_handle, x1, y1, x2, y2, color, static_cast<double>(sim), count, height, width, s);
    return CopyOutBstr(retstr, s);
}

STDMETHODIMP OpAutomation::YoloDetectFromMem(BSTR image_handle, LONG x1, LONG y1, LONG x2, LONG y2, DOUBLE conf,
                                             DOUBLE iou, BSTR *retstr, LONG *ret) {
    if (!retstr || !ret)
        return E_POINTER;
    *ret = 0;
    wstring s;
    obj.YoloDetectFromMem(image_handle, x1, y1, x2, y2, static_cast<double>(conf), static_cast<double>(iou), s, ret);
    return CopyOutBstr(retstr, s);
}

STDMETHODIMP OpAutomation::YoloDetectFromMemEx2(BSTR image_handle, LONG x1, LONG y1, LONG x2, LONG y2, DOUBLE conf,
                                                DOUBLE iou, BSTR json_options, BSTR *retstr, LONG *ret) {
    if (!retstr || !ret)
        return E_POINTER;
    *ret = 0;
    wstring s;
    wstring options_text = json_options ? json_options : L"";
    obj.YoloDetectFromMemEx2(image_handle, x1, y1, x2, y2, static_cast<double>(conf), static_cast<double>(iou),
                             options_text.c_str(), s, ret);
    return CopyOutBstr(retstr, s);
}
'''


def read(p):
    with io.open(p, "r", encoding="utf-8", newline="") as f:
        return f.read()


def safe_write(path, new_text, must_contain, min_lines=1):
    old = read(path)
    if old.count("\n") < min_lines:
        print("  FAIL truncated:", os.path.basename(path))
        return False
    with io.open(path + ".bak", "w", encoding="utf-8", newline="") as f:
        f.write(old)
    with io.open(path, "w", encoding="utf-8", newline="") as f:
        f.write(new_text)
    got = read(path)
    missing = [k for k in must_contain if k not in got]
    if missing:
        with io.open(path, "w", encoding="utf-8", newline="") as f:
            f.write(old)
        print("  ROLLBACK:", os.path.basename(path), "missing", missing)
        return False
    os.remove(path + ".bak")
    print("  OK:", os.path.basename(path))
    return True


def main():
    ok = True

    print("[1/7] include/libop.h")
    t = read(LIBOP_H)
    nl = "\r\n" if "\r\n" in t else "\n"
    marker = "    // ===== \u97f3\u9891\u97f3\u91cf\uff082026-11-21\uff0cWASAPI \u4f1a\u8bdd\u7ea7 + \u7cfb\u7edf\u7ea7\uff09====="
    if marker not in t:
        print("  FAIL libop.h marker miss")
        ok = False
    else:
        body = LIBOP_H_BLOCK.replace("\r\n", "\n").replace("\n", nl)
        ok &= safe_write(LIBOP_H, t.replace(marker, body + marker, 1), ["OcrFromMemEx2", "YoloDetectFromMemEx2"], 700)

    print("[2/7] include/op_c_api.h (inside extern \"C\")")
    t = read(OP_C_API_H)
    nl = "\r\n" if "\r\n" in t else "\n"
    tail = "#ifdef __cplusplus" + nl + "}" + nl + "#endif"
    if t.count(tail) != 1:
        print("  FAIL tail marker count =", t.count(tail))
        ok = False
    else:
        body = OP_C_API_H_BLOCK.replace("\r\n", "\n").replace("\n", nl)
        ok &= safe_write(OP_C_API_H, t.replace(tail, body + tail, 1), ["OpOcrFromMemEx2", "OpYoloDetectFromMemEx2"])
        got = read(OP_C_API_H)
        inside = got.index("OpOcrFromMemEx2") < got.index(tail)
        print("  declaration inside extern C:", inside)
        ok &= inside

    print("[3/7] libop/com/op.idl")
    t = read(OP_IDL)
    nl = "\r\n" if "\r\n" in t else "\n"
    # IDL 里最后一个 id 是 463，插在其后
    marker = "\t[id(463)] HRESULT AudioSetSystemMute([in] LONG muted, [out, retval] LONG* ret);"
    if marker not in t:
        print("  FAIL idl marker miss")
        ok = False
    else:
        body = OP_IDL_BLOCK.replace("\r\n", "\n").replace("\n", nl)
        pos = t.index(marker) + len(marker)
        ok &= safe_write(OP_IDL, t[:pos] + body + t[pos:], ["OcrFromMemEx2", "id(477)"], 300)

    print("[4/7] libop/CMakeLists.txt")
    t = read(CMAKELISTS)
    nl = "\r\n" if "\r\n" in t else "\n"
    anchor = '"op/OpAudio.cpp"'
    if "op/OpMemImage.cpp" in t:
        print("  SKIP already present")
    elif anchor not in t:
        print("  FAIL anchor miss")
        ok = False
    else:
        t = t.replace(anchor, anchor + nl + '  # \u5185\u5b58\u76f4\u64cd\u4f5c\u4e8c\u671f\uff082026-11-21\uff09' + nl + '  "op/OpMemImage.cpp"', 1)
        ok &= safe_write(CMAKELISTS, t, ["op/OpMemImage.cpp"])

    print("[5/7] libop/c_api/op_c_api.cpp (append in extern \"C\")")
    t = read(OP_C_API_CPP)
    nl = "\r\n" if "\r\n" in t else "\n"
    body = OP_C_API_CPP_BLOCK.replace("\r\n", "\n").replace("\n", nl)
    newt = t.rstrip() + nl + nl + 'extern "C" {' + nl + body.rstrip() + nl + "}" + nl
    ok &= safe_write(OP_C_API_CPP, newt, ["OpOcrFromMemEx2", "OpYoloDetectFromMemEx2"], 1000)

    print("[6/7] OpAutomation.h")
    t = read(AUTOMATION_H)
    nl = "\r\n" if "\r\n" in t else "\n"
    marker = "    //-----------------------opcv---------------------------------"
    if marker not in t:
        print("  FAIL automation.h marker miss")
        ok = False
    else:
        body = AUTOMATION_H_BLOCK.replace("\r\n", "\n").replace("\n", nl)
        ok &= safe_write(AUTOMATION_H, t.replace(marker, body + marker, 1),
                         ["STDMETHOD(OcrFromMemEx2)", "STDMETHOD(YoloDetectFromMemEx2)"], 400)

    print("[7/7] OpAutomation.cpp (append)")
    t = read(AUTOMATION_CPP)
    nl = "\r\n" if "\r\n" in t else "\n"
    body = AUTOMATION_CPP_BLOCK.replace("\r\n", "\n").replace("\n", nl)
    ok &= safe_write(AUTOMATION_CPP, t.rstrip() + nl + body, ["OpAutomation::OcrFromMemEx2", "OpAutomation::YoloDetectFromMemEx2"], 1000)

    print("\nRESULT:", "OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
