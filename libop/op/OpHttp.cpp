// OpHttp.cpp: HTTP 小族实现（P3 第二项 2026-10-16）。底层 network/HttpRequest（WinHTTP），
// 本文件只做参数防御 + 结果回填。用途：脚本对接大模型 OpenAI 兼容 API（配 JSON 族闭环）。
#include <libop.h>

#include "../network/HttpRequest.h"

#include <string>

void op::Op::HttpGet(const wchar_t *url, const wchar_t *headers_json, long timeout_ms, std::wstring &retstr,
                     long *ret) {
    retstr.clear();
    *ret = 0;
    if (!url || url[0] == L'\0')
        return;
    const op::net::HttpResult r = op::net::HttpGet(url, headers_json ? headers_json : L"", static_cast<int>(timeout_ms));
    retstr = r.response; // 非 2xx 也带回响应体，错误 JSON 可见
    *ret = r.ok ? 1 : 0;
}

void op::Op::HttpPost(const wchar_t *url, const wchar_t *body, const wchar_t *headers_json, long timeout_ms,
                      std::wstring &retstr, long *ret) {
    retstr.clear();
    *ret = 0;
    if (!url || url[0] == L'\0')
        return;
    const op::net::HttpResult r = op::net::HttpPost(url, body ? body : L"", headers_json ? headers_json : L"",
                                                    static_cast<int>(timeout_ms));
    retstr = r.response;
    *ret = r.ok ? 1 : 0;
}

void op::Op::HttpDownloadFile(const wchar_t *url, const wchar_t *file_name, long timeout_ms, long *ret) {
    *ret = 0;
    if (!url || url[0] == L'\0' || !file_name || file_name[0] == L'\0')
        return;
    const op::net::HttpResult r = op::net::HttpDownloadFile(url, file_name, static_cast<int>(timeout_ms));
    *ret = r.ok ? 1 : 0;
}
