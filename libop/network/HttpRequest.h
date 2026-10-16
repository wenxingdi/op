#pragma once
// HTTP 小族底层（P3 第二项，2026-10-16）：基于系统 WinHTTP 的简单请求客户端。
// 设计要点：
// - 零第三方依赖，winhttp.dll 系统自带；HTTPS/TLS 与系统代理自动继承。
// - 请求/响应体经 UTF-8 编解码（与 OP 既有 HTTP 后端约定一致）；header 用
//   JSON 对象文本传递（复用 libop/json 解析器），如
//   {"Authorization":"Bearer sk-xxx","Content-Type":"application/json"}。
// - 超时统一四阶段（解析/连接/发送/接收）取同一值，钳 1000-300000ms。
// - 非 2xx 不算传输失败：ok=false 但 response 带回响应体（大模型错误 JSON 可见）。
//
// 注意：与 network/HttpClient.h/.cpp（旧 OCR/YOLO 远程后端，ANSI 版，已移出构建）
// 完全无关，本文件为独立新实现。

#include <string>

namespace op::net {

struct HttpResult {
    bool ok = false;        // 传输成功且状态码 2xx
    long status = 0;        // HTTP 状态码（传输失败为 0）
    std::wstring response;  // 响应体（非 2xx 时也带回；传输失败为空）
};

// GET。headers_json 为空串表示无自定义头。
HttpResult HttpGet(const std::wstring &url, const std::wstring &headers_json, int timeout_ms);

// POST。body 为宽字符，内部按 UTF-8 发送；自动补 Content-Length。
HttpResult HttpPost(const std::wstring &url, const std::wstring &body, const std::wstring &headers_json,
                    int timeout_ms);

// 下载到文件（原始字节流式落盘，不经 UTF-8 解码）。parent 目录须已存在。
// ok=true 表示 2xx 且文件完整写入。
HttpResult HttpDownloadFile(const std::wstring &url, const std::wstring &file_name, int timeout_ms);

} // namespace op::net
