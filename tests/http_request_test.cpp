// HTTP 小族自测（P3 第二项，2026-10-16）：OpHttpGet/OpHttpPost/OpHttpDownloadFile。
// 不依赖外网：测试进程内嵌 WinSock 回环 HTTP 服务器（127.0.0.1:动态端口），
// 服务器把收到的请求行/头/体回显给用例做断言，可按用例配置状态码与延迟。

// winsock2.h 必须最先：test_support.h/gtest 会带入 Windows.h，后引入 winsock2
// 会触发 ws2def.h sockaddr 重定义（C2011）。
#include <winsock2.h>

#include "test_support.h"

#include <gtest/gtest.h>

#include <op_c_api.h>

#include <Windows.h>
#include <ws2tcpip.h>

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace {

// 单请求回环服务器：accept 一个连接，读完整请求，交给 handler 生成响应。
// handler 收到 "METHOD path\r\nHEADERS\r\n\r\nBODY"，返回 "STATUS\r\nBODY"。
class MiniHttpServer {
public:
    struct Captured {
        std::string method;
        std::string path;
        std::string headers;
        std::string body;
    };

    MiniHttpServer(std::function<std::pair<int, std::string>(const Captured &)> handler, int delay_ms = 0)
        : handler_(std::move(handler)), delay_ms_(delay_ms) {
        // 构造失败抛异常（gtest 会把异常记为测试失败）；构造函数里不能用 ASSERT。
        WSADATA wsa = {};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            throw std::runtime_error("WSAStartup failed");
        listen_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_ == INVALID_SOCKET)
            throw std::runtime_error("socket failed");
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0; // 动态端口
        if (bind(listen_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
            throw std::runtime_error("bind failed");
        if (listen(listen_, 1) != 0)
            throw std::runtime_error("listen failed");
        int len = sizeof(addr);
        if (getsockname(listen_, reinterpret_cast<sockaddr *>(&addr), &len) != 0)
            throw std::runtime_error("getsockname failed");
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] { serve(); });
    }

    ~MiniHttpServer() {
        closesocket(listen_);
        if (thread_.joinable())
            thread_.join();
        WSACleanup();
    }

    std::wstring url(const std::string &path) const {
        return L"http://127.0.0.1:" + std::to_wstring(port_) +
               std::wstring(path.begin(), path.end());
    }

private:
    void serve() {
        SOCKET client = accept(listen_, nullptr, nullptr);
        if (client == INVALID_SOCKET)
            return; // 监听 socket 被关闭（测试收尾）
        std::string req;
        char buf[4096];
        // 读到头结束
        while (req.find("\r\n\r\n") == std::string::npos) {
            const int n = recv(client, buf, sizeof(buf), 0);
            if (n <= 0)
                break;
            req.append(buf, static_cast<size_t>(n));
        }
        Captured cap;
        const size_t head_end = req.find("\r\n\r\n");
        if (head_end != std::string::npos) {
            const std::string head = req.substr(0, head_end);
            cap.body = req.substr(head_end + 4);
            const size_t sp1 = head.find(' ');
            const size_t sp2 = head.find(' ', sp1 + 1);
            cap.method = head.substr(0, sp1);
            cap.path = head.substr(sp1 + 1, sp2 - sp1 - 1);
            cap.headers = head.substr(sp2 + 1);
            // 按 Content-Length 补读 body
            const std::string key = "content-length:";
            std::string lower = cap.headers;
            for (auto &ch : lower)
                ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
            const size_t pos = lower.find(key);
            if (pos != std::string::npos) {
                const size_t begin = lower.find_first_not_of(" \t", pos + key.size());
                const size_t end = lower.find("\r\n", begin);
                const size_t want = std::strtoul(lower.substr(begin, end - begin).c_str(), nullptr, 10);
                while (cap.body.size() < want) {
                    const int n = recv(client, buf, sizeof(buf), 0);
                    if (n <= 0)
                        break;
                    cap.body.append(buf, static_cast<size_t>(n));
                }
                cap.body.resize(want);
            }
        }
        if (delay_ms_ > 0)
            Sleep(static_cast<DWORD>(delay_ms_));
        auto [status, body] = handler_(cap);
        std::string resp = "HTTP/1.1 " + std::to_string(status) +
                           (status == 200 ? " OK" : " ERR") +
                           "\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: " +
                           std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        send(client, resp.data(), static_cast<int>(resp.size()), 0);
        closesocket(client);
    }

    std::function<std::pair<int, std::string>(const Captured &)> handler_;
    int delay_ms_;
    SOCKET listen_ = INVALID_SOCKET;
    u_short port_ = 0;
    std::thread thread_;
};

struct CApiHandle {
    op_handle handle = OpCreate();
    ~CApiHandle() {
        OpDestroy(handle);
    }
};

TEST(HttpRequestTest, GetRoundTrip) {
    MiniHttpServer server([](const MiniHttpServer::Captured &cap) {
        EXPECT_EQ(cap.method, "GET");
        EXPECT_EQ(cap.path, "/get?x=1");
        return std::make_pair(200, std::string("{\"pong\":true}"));
    });
    CApiHandle api;
    const wchar_t *r = OpHttpGet(api.handle, server.url("/get?x=1").c_str(), L"", 5000);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(std::wstring(r), L"{\"pong\":true}");
}

TEST(HttpRequestTest, HeaderPassthrough) {
    MiniHttpServer server([](const MiniHttpServer::Captured &cap) {
        // 请求头回显进响应体，用例侧用 JSON 族校验
        const bool has_auth = cap.headers.find("Authorization: Bearer test123") != std::string::npos;
        const bool has_custom = cap.headers.find("X-Custom: yes") != std::string::npos;
        return std::make_pair(200, std::string("{\"auth\":") + (has_auth ? "true" : "false") +
                                       ",\"custom\":" + (has_custom ? "true" : "false") + "}");
    });
    CApiHandle api;
    const std::wstring headers = L"{\"Authorization\":\"Bearer test123\",\"X-Custom\":\"yes\"}";
    const wchar_t *r = OpHttpGet(api.handle, server.url("/h").c_str(), headers.c_str(), 5000);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(std::wstring(r), L"{\"auth\":true,\"custom\":true}");
    // 非对象 headers_json → 空串
    EXPECT_STREQ(OpHttpGet(api.handle, server.url("/h").c_str(), L"[1,2]", 5000), L"");
}

TEST(HttpRequestTest, PostBodyUtf8AndContentType) {
    MiniHttpServer server([](const MiniHttpServer::Captured &cap) {
        EXPECT_EQ(cap.method, "POST");
        EXPECT_TRUE(cap.headers.find("Content-Type: application/json") != std::string::npos);
        // body 原样回显（UTF-8 字节流）
        return std::make_pair(200, cap.body);
    });
    CApiHandle api;
    const std::wstring body = L"{\"msg\":\"你好\"}";
    const std::wstring headers = L"{\"Content-Type\":\"application/json\"}";
    const wchar_t *r = OpHttpPost(api.handle, server.url("/p").c_str(), body.c_str(), headers.c_str(), 5000);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(std::wstring(r), body);
    // 用 JSON 族验证闭环：解析响应并取出中文字段。
    // 注意 c_api 字符串返回共用 string_result 缓冲：先把响应拷出来再调下一个字符串函数。
    const std::wstring resp = r;
    EXPECT_EQ(OpJsonGetString(api.handle, resp.c_str(), L"msg"), std::wstring(L"你好"));
}

TEST(HttpRequestTest, Non2xxStillReturnsBody) {
    MiniHttpServer server([](const MiniHttpServer::Captured &) {
        return std::make_pair(500, std::string("{\"error\":\"boom\"}"));
    });
    CApiHandle api;
    // 非 2xx：响应体仍带回（大模型错误 JSON 可见），由调用方用 JSON 族判错
    const wchar_t *r = OpHttpPost(api.handle, server.url("/e").c_str(), L"{}", L"", 5000);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(std::wstring(r), L"{\"error\":\"boom\"}");
}

TEST(HttpRequestTest, TimeoutReturnsEmpty) {
    // 连接阶段超时可稳定复现（TEST-NET 黑洞地址 10.255.255.1，无路由不可达）。
    // 已连接但慢响应的首字节等待不受 WinHTTP 四阶段超时约束（实测本机 4s 延迟
    // 的服务器配 1000ms 超时仍等满 4s），该限制记入 CHANGELOG 与手册，
    // 需要硬超时的调用方应在工作线程中调 OpHttpGet 并自行等待。
    CApiHandle api;
    const DWORD begin = GetTickCount();
    const wchar_t *r = OpHttpGet(api.handle, L"http://10.255.255.1:81/slow", L"", 1000);
    const DWORD elapsed = GetTickCount() - begin;
    EXPECT_LT(elapsed, 15000u); // 1s 客户端超时生效，没有陷入系统级长 connect 重试
    EXPECT_STREQ(r, L"");
}

TEST(HttpRequestTest, DownloadFileWritesBytes) {
    std::string payload(65536, '\0');
    for (size_t i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<char>(i % 251);
    MiniHttpServer server([&payload](const MiniHttpServer::Captured &cap) {
        EXPECT_EQ(cap.method, "GET");
        return std::make_pair(200, payload);
    });
    CApiHandle api;
    wchar_t temp[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, temp);
    const std::wstring path = std::wstring(temp) + L"op_http_dl_test.bin";
    DeleteFileW(path.c_str());
    EXPECT_EQ(OpHttpDownloadFile(api.handle, server.url("/f").c_str(), path.c_str(), 5000), 1);
    std::ifstream in(path, std::ios::binary);
    ASSERT_TRUE(in.good());
    std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(got, payload);
    DeleteFileW(path.c_str());
}

TEST(HttpRequestTest, InvalidInputReturnsEmpty) {
    CApiHandle api;
    EXPECT_STREQ(OpHttpGet(api.handle, L"", L"", 5000), L"");                    // 空 URL
    EXPECT_STREQ(OpHttpGet(api.handle, L"not-a-url", L"", 5000), L"");           // 非法 URL
    EXPECT_STREQ(OpHttpPost(api.handle, L"http://127.0.0.1:1/x", L"{}", L"", 5000), L""); // 拒连
    EXPECT_EQ(OpHttpDownloadFile(api.handle, L"", L"x.bin", 5000), 0);
    EXPECT_EQ(OpHttpDownloadFile(api.handle, L"http://127.0.0.1:1/f", L"", 5000), 0);
}

TEST(HttpRequestTest, NullHandleReturnsFailure) {
    EXPECT_STREQ(OpHttpGet(nullptr, L"http://127.0.0.1/", L"", 5000), L"");
    EXPECT_STREQ(OpHttpPost(nullptr, L"http://127.0.0.1/", L"{}", L"", 5000), L"");
    EXPECT_EQ(OpHttpDownloadFile(nullptr, L"http://127.0.0.1/f", L"x.bin", 5000), 0);
}

} // namespace
