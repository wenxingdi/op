#include "HttpRequest.h"

#include "../base/Utils.h"
#include "../json/JsonValue.h"

#include <Windows.h>
#include <winhttp.h>

#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace op::net {

namespace {

int clamp_timeout(int timeout_ms) {
    if (timeout_ms <= 0)
        timeout_ms = 30000;
    if (timeout_ms < 1000)
        timeout_ms = 1000;
    if (timeout_ms > 300000)
        timeout_ms = 300000;
    return timeout_ms;
}

std::string wide_to_utf8(const std::wstring &s) {
    if (s.empty())
        return "";
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr,
                                        nullptr);
    if (n <= 0)
        return "";
    std::string out(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &out[0], n, nullptr, nullptr);
    return out;
}

std::wstring utf8_to_wide(const std::string &s) {
    if (s.empty())
        return L"";
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0)
        return L"";
    std::wstring out(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &out[0], n);
    return out;
}

// headers_json（JSON 对象）→ "K: V\r\n" 追加串；非字符串值跳过；解析失败返回 false。
bool build_headers(const std::wstring &headers_json, std::wstring &out) {
    out.clear();
    if (headers_json.empty())
        return true;
    json::Value root;
    if (!json::Parse(headers_json, root) || root.type != json::Type::Object)
        return false;
    for (const auto &kv : root.obj) {
        if (kv.second.type != json::Type::String)
            continue;
        out += kv.first;
        out += L": ";
        out += kv.second.str;
        out += L"\r\n";
    }
    return true;
}

struct UrlParts {
    bool secure = false;
    INTERNET_PORT port = 0;
    std::wstring host;
    std::wstring path; // 含 query
};

bool crack_url(const std::wstring &url, UrlParts &out) {
    URL_COMPONENTSW c = {};
    c.dwStructSize = sizeof(c);
    wchar_t host_buf[256] = {};
    wchar_t path_buf[4096] = {};
    wchar_t query_buf[4096] = {};
    c.lpszHostName = host_buf;
    c.dwHostNameLength = _countof(host_buf);
    c.lpszUrlPath = path_buf;
    c.dwUrlPathLength = _countof(path_buf);
    c.lpszExtraInfo = query_buf;
    c.dwExtraInfoLength = _countof(query_buf);
    if (!::WinHttpCrackUrl(url.c_str(), 0, 0, &c))
        return false;
    out.secure = (c.nScheme == INTERNET_SCHEME_HTTPS);
    out.port = c.nPort;
    out.host.assign(host_buf, c.dwHostNameLength);
    out.path.assign(path_buf, c.dwUrlPathLength);
    out.path.append(query_buf, c.dwExtraInfoLength);
    if (out.host.empty())
        return false;
    if (out.path.empty())
        out.path = L"/";
    return true;
}

HttpResult do_request(const wchar_t *method, const std::wstring &url, const std::wstring *body,
                      const std::wstring &headers_json, int timeout_ms, const std::wstring *download_path) {
    HttpResult result;
    const int timeout = clamp_timeout(timeout_ms);

    std::wstring headers;
    if (!build_headers(headers_json, headers)) {
        setlog("HttpRequest: headers_json 非法（须为 JSON 对象文本）");
        return result;
    }

    UrlParts parts;
    if (!crack_url(url, parts)) {
        setlog("HttpRequest: URL 解析失败: %ls", url.c_str());
        return result;
    }

    HINTERNET session =
        ::WinHttpOpen(L"op-master/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_BYPASS, 0, 0);
    if (!session) {
        setlog("HttpRequest: WinHttpOpen 失败: %lu", ::GetLastError());
        return result;
    }
    ::WinHttpSetTimeouts(session, timeout, timeout, timeout, timeout);

    HINTERNET connect = ::WinHttpConnect(session, parts.host.c_str(), parts.port, 0);
    if (!connect) {
        setlog("HttpRequest: WinHttpConnect 失败: %lu", ::GetLastError());
        ::WinHttpCloseHandle(session);
        return result;
    }

    const DWORD open_flags = parts.secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request =
        ::WinHttpOpenRequest(connect, method, parts.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, open_flags);
    if (!request) {
        setlog("HttpRequest: WinHttpOpenRequest 失败: %lu", ::GetLastError());
        ::WinHttpCloseHandle(connect);
        ::WinHttpCloseHandle(session);
        return result;
    }
    // 超时会话级已设；请求级再设一次（本机实测仅会话级对首字节等待不生效）。
    ::WinHttpSetTimeouts(request, timeout, timeout, timeout, timeout);

    std::wstring all_headers = headers;
    std::string body_utf8;
    if (body) {
        body_utf8 = wide_to_utf8(*body);
        wchar_t len_buf[32] = {};
        _snwprintf_s(len_buf, _countof(len_buf), L"Content-Length: %llu\r\n",
                     static_cast<unsigned long long>(body_utf8.size()));
        all_headers += len_buf;
    }

    BOOL sent = ::WinHttpSendRequest(request, all_headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                                                  : all_headers.c_str(),
                                     static_cast<DWORD>(all_headers.size()),
                                     body ? const_cast<char *>(body_utf8.data()) : WINHTTP_NO_REQUEST_DATA,
                                     body ? static_cast<DWORD>(body_utf8.size()) : 0,
                                     body ? static_cast<DWORD>(body_utf8.size()) : 0, 0);
    if (!sent) {
        setlog("HttpRequest: WinHttpSendRequest 失败: %lu", ::GetLastError());
        ::WinHttpCloseHandle(request);
        ::WinHttpCloseHandle(connect);
        ::WinHttpCloseHandle(session);
        return result;
    }

    if (!::WinHttpReceiveResponse(request, nullptr)) {
        setlog("HttpRequest: WinHttpReceiveResponse 失败: %lu", ::GetLastError());
        ::WinHttpCloseHandle(request);
        ::WinHttpCloseHandle(connect);
        ::WinHttpCloseHandle(session);
        return result;
    }

    DWORD status_code = 0;
    DWORD status_len = sizeof(status_code);
    if (::WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_len,
                              WINHTTP_NO_HEADER_INDEX))
        result.status = static_cast<long>(status_code);

    // 聚合响应体；下载模式边收边写文件。
    HANDLE file = INVALID_HANDLE_VALUE;
    if (download_path)
        file = ::CreateFileW(download_path->c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);

    std::vector<char> buffer;
    bool io_error = false;
    for (;;) {
        DWORD avail = 0;
        if (!::WinHttpQueryDataAvailable(request, &avail))
            break;
        if (avail == 0)
            break;
        std::vector<char> chunk(avail);
        DWORD read = 0;
        if (!::WinHttpReadData(request, chunk.data(), avail, &read) || read == 0)
            break;
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            if (!::WriteFile(file, chunk.data(), read, &written, nullptr) || written != read) {
                setlog("HttpRequest: 写文件失败: %ls", download_path->c_str());
                io_error = true;
                break;
            }
        } else {
            buffer.insert(buffer.end(), chunk.begin(), chunk.begin() + read);
        }
    }

    if (file != INVALID_HANDLE_VALUE)
        ::CloseHandle(file);

    if (!download_path)
        result.response = utf8_to_wide(std::string(buffer.begin(), buffer.end()));

    result.ok = (status_code >= 200 && status_code < 300) && !io_error;

    ::WinHttpCloseHandle(request);
    ::WinHttpCloseHandle(connect);
    ::WinHttpCloseHandle(session);
    return result;
}

} // namespace

HttpResult HttpGet(const std::wstring &url, const std::wstring &headers_json, int timeout_ms) {
    return do_request(L"GET", url, nullptr, headers_json, timeout_ms, nullptr);
}

HttpResult HttpPost(const std::wstring &url, const std::wstring &body, const std::wstring &headers_json,
                    int timeout_ms) {
    return do_request(L"POST", url, &body, headers_json, timeout_ms, nullptr);
}

HttpResult HttpDownloadFile(const std::wstring &url, const std::wstring &file_name, int timeout_ms) {
    return do_request(L"GET", url, nullptr, L"", timeout_ms, &file_name);
}

} // namespace op::net
