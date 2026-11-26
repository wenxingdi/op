// 内存读写测试（本进程自测，免注入、沙箱可跑）
// 覆盖 ReadInt/WriteInt/ReadFloat/WriteFloat/ReadDouble/WriteDouble/
//       ReadString/WriteString/ReadData/WriteData 十条此前零覆盖的路径，
// 外加地址表达式（模块基址 / 多级指针）与 M2/M3 加固行为钉板。
#include <gtest/gtest.h>

#include <op_c_api.h>

#include <Windows.h>

#include <cstdint>
#include <sstream>
#include <string>

namespace {

struct CApiHandle {
    op_handle handle = OpCreate();
    ~CApiHandle() {
        OpDestroy(handle);
    }
};

// 两页已提交内存，缓冲区放在第二页页首：这样 auto-len 的 4096 字节整页读
// 恰好不越界（静态变量缓冲区无法保证读后 4KB 仍已提交，会引入随机失败）。
struct CommittedPage {
    void *base = nullptr;
    CommittedPage() {
        base = ::VirtualAlloc(nullptr, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }
    ~CommittedPage() {
        if (base)
            ::VirtualFree(base, 0, MEM_RELEASE);
    }
    wchar_t *buf() const { return reinterpret_cast<wchar_t *>(static_cast<char *>(base) + 4096); }
};

std::wstring HexOf(const volatile void *p) {
    std::wstringstream ss;
    ss << std::uppercase << std::hex << reinterpret_cast<uintptr_t>(const_cast<void *>(p));
    return ss.str();
}

// ---------- Int 往返（type 0=i32 1=i16 2=i8 3=i64 4=u32 5=u16 6=u8） ----------

TEST(MemoryReadWriteTest, IntRoundTripAllTypes) {
    CApiHandle api;
    static volatile int64_t box = 0; // 静态存储期，测试进程内地址稳定
    const std::wstring addr = HexOf(&box);

    const struct {
        long type;
        int64_t value;
    } cases[] = {
        {0, 0x11223344ll},        // i32
        {1, -12345ll},            // i16
        {2, -100ll},              // i8
        {3, 0x1122334455667788ll},// i64
        {4, 0xFFFFFFFEll},        // u32
        {5, 60000ll},             // u16
        {6, 200ll},               // u8
    };
    for (const auto &c : cases) {
        EXPECT_EQ(OpWriteInt(api.handle, 0, addr.c_str(), c.type, c.value), 1) << "type=" << c.type;
        int64_t back = -1;
        OpReadInt(api.handle, 0, addr.c_str(), c.type, &back);
        EXPECT_EQ(back, c.value) << "type=" << c.type;
    }
}

TEST(MemoryReadWriteTest, ReadIntFailureYieldsZero) {
    CApiHandle api;
    int64_t v = 0x7FFFFFFFFFFFFFFFll;
    OpReadInt(api.handle, 0, L"1", 0, &v); // 地址 1 不可读
    EXPECT_EQ(v, 0);
}

// ---------- Float / Double 往返 ----------

TEST(MemoryReadWriteTest, FloatDoubleRoundTrip) {
    CApiHandle api;
    static volatile float fbox = 0.0f;
    static volatile double dbox = 0.0;

    EXPECT_EQ(OpWriteFloat(api.handle, 0, HexOf(&fbox).c_str(), 3.14159f), 1);
    float f = 0.0f;
    OpReadFloat(api.handle, 0, HexOf(&fbox).c_str(), &f);
    EXPECT_FLOAT_EQ(f, 3.14159f);

    EXPECT_EQ(OpWriteDouble(api.handle, 0, HexOf(&dbox).c_str(), -2.718281828), 1);
    double d = 0.0;
    OpReadDouble(api.handle, 0, HexOf(&dbox).c_str(), &d);
    EXPECT_DOUBLE_EQ(d, -2.718281828);
}

// ---------- String 往返（type 0=ACP 1=UTF-16 2=UTF-8） ----------

TEST(MemoryReadWriteTest, StringRoundTripAllEncodings) {
    CApiHandle api;
    CommittedPage page;
    ASSERT_NE(page.base, nullptr);
    const std::wstring addr = HexOf(page.buf());

    // type=1 UTF-16：写入后按自动长度（len<=0 读到第一个 0）读回
    EXPECT_EQ(OpWriteString(api.handle, 0, addr.c_str(), 1, L"Hello OP"), 1);
    EXPECT_STREQ(OpReadString(api.handle, 0, addr.c_str(), 1, 0), L"Hello OP");

    // type=2 UTF-8：ASCII 内容用同编码/ACP 读回应一致；
    // 注意不能用 type=1 读——UTF-8 字节流按 UTF-16 解必然乱码（'\x6261' 之类），这是正确行为
    EXPECT_EQ(OpWriteString(api.handle, 0, addr.c_str(), 2, L"abc123"), 1);
    EXPECT_STREQ(OpReadString(api.handle, 0, addr.c_str(), 2, 0), L"abc123");
    EXPECT_STREQ(OpReadString(api.handle, 0, addr.c_str(), 0, 0), L"abc123"); // ACP=GBK，ASCII 兼容
}

TEST(MemoryReadWriteTest, ReadStringExplicitLenTrimsAtContent) {
    CApiHandle api;
    CommittedPage page;
    ASSERT_NE(page.base, nullptr);
    const std::wstring addr = HexOf(page.buf());
    OpWriteString(api.handle, 0, addr.c_str(), 1, L"hi");

    // len 给足（128 字节 = 64 宽字符）也只应返回内容本身
    EXPECT_STREQ(OpReadString(api.handle, 0, addr.c_str(), 1, 64), L"hi");
}

TEST(MemoryReadWriteTest, ReadStringHugeLenIsClampedAndFailsGracefully) {
    CApiHandle api;
    CommittedPage page;
    ASSERT_NE(page.base, nullptr);
    const std::wstring addr = HexOf(page.buf());
    // len=64MB 超出 16MB 上限：截断后从单页缓冲区起读必然读不满 → 优雅返回空，不分配 1GB
    EXPECT_STREQ(OpReadString(api.handle, 0, addr.c_str(), 1, 64 * 1024 * 1024), L"");
}

// ---------- Data 十六进制往返 + M2 补零钉板 ----------

TEST(MemoryReadWriteTest, WriteDataReadDataHexRoundTrip) {
    CApiHandle api;
    static volatile unsigned char buf[32] = {0};
    const std::wstring addr = HexOf(const_cast<unsigned char *>(buf));

    // 大写/小写混合 + 无分隔十六进制
    EXPECT_EQ(OpWriteData(api.handle, 0, addr.c_str(), L"deAdBEef00", 5), 1);
    const wchar_t *hex = OpReadData(api.handle, 0, addr.c_str(), 5);
    ASSERT_NE(hex, nullptr);
    EXPECT_STREQ(hex, L"DEADBEEF00"); // 输出恒为大写十六进制
}

TEST(MemoryReadWriteTest, WriteDataPadsZerosWhenSizeExceedsData) {
    CApiHandle api;
    static volatile unsigned char buf[32] = {0};
    const std::wstring addr = HexOf(const_cast<unsigned char *>(buf));

    // M2 钉板：size=4 但 data 只有 2 字节 → 大漠兼容行为补两个零（此行为有日志留痕）
    EXPECT_EQ(OpWriteData(api.handle, 0, addr.c_str(), L"1122", 4), 1);
    EXPECT_STREQ(OpReadData(api.handle, 0, addr.c_str(), 4), L"11220000");
}

TEST(MemoryReadWriteTest, WriteDataRejectsBadInput) {
    CApiHandle api;
    static volatile unsigned char buf[32] = {0};
    const std::wstring addr = HexOf(const_cast<unsigned char *>(buf));
    EXPECT_EQ(OpWriteData(api.handle, 0, addr.c_str(), L"1122", 0), 0);   // size<=0
    EXPECT_EQ(OpWriteData(api.handle, 0, L"[1", L"1122", 2), 0);          // 括号不配对
    EXPECT_STREQ(OpReadData(api.handle, 0, addr.c_str(), 0), L"");        // size<=0 → 空
}

// ---------- 地址表达式：模块基址 + 多级指针 ----------

TEST(MemoryReadWriteTest, ModuleBaseExpressionInAddress) {
    CApiHandle api;
    // <kernel32.dll>+0 即模块基址，应能读出一个非零值（PE 头 MZ 魔数 0x5A4D）
    int64_t v = 0;
    OpReadInt(api.handle, 0, L"<kernel32.dll>", 0, &v);
    EXPECT_EQ(v & 0xFFFF, 0x5A4D) << "PE MZ magic at module base";
}

TEST(MemoryReadWriteTest, MultiLevelPointerExpression) {
    CApiHandle api;
    static volatile int64_t box = 0;
    static volatile void *p1 = &box;
    static volatile void *p2 = &p1;

    EXPECT_EQ(OpWriteInt(api.handle, 0, HexOf(&box).c_str(), 3, 0x1122334455667788ll), 1);
    // [[p2]] → *( *(p2) ) = box 的地址
    const std::wstring expr = L"[[" + HexOf(const_cast<void **>(&p2)) + L"]]";
    int64_t v = 0;
    OpReadInt(api.handle, 0, expr.c_str(), 3, &v);
    EXPECT_EQ(v, 0x1122334455667788ll);
}

// ---------- 远程句柄缓存（提效 4，2026-11-26）----------
// 背景：ProcessMemory 每次 API 调用临时构造，远程路径每次都 OpenProcess/CloseHandle。
// 启用缓存后跨调用复用句柄（10s TTL + 创建时间校验防 pid 复用）。
// 此处用 plain_carrier 独立进程做远程读目标：命中/未命中计数 + 缓存前后读值一致性。

// 最小载体启动器：与 hook_capture_test 的 CarrierProcess 同思路（独立 exe + 回报文件轮询），
// 本测试只需要一个窗口句柄做远程目标，故用 plain_carrier（只依赖 user32/gdi32）。
class PlainCarrier {
  public:
    PlainCarrier() = default;
    ~PlainCarrier() { Stop(); }
    PlainCarrier(const PlainCarrier &) = delete;
    PlainCarrier &operator=(const PlainCarrier &) = delete;

    bool Start() {
        wchar_t self[MAX_PATH] = {};
        if (!GetModuleFileNameW(nullptr, self, MAX_PATH))
            return false;
        std::wstring exe(self);
        const size_t cut = exe.find_last_of(L"\\/");
        if (cut == std::wstring::npos)
            return false;
        exe = exe.substr(0, cut) + L"\\plain_carrier.exe";

        wchar_t dir[MAX_PATH] = {};
        const DWORD dlen = GetTempPathW(MAX_PATH, dir);
        report_ = std::wstring(dir, dlen) + L"op_mem_cache_carrier.txt";
        DeleteFileW(report_.c_str());

        std::wstring cmd = L"\"" + exe + L"\" --seconds 30 --report \"" + report_ + L"\"";
        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                            &pi)) {
            return false;
        }
        process_ = pi.hProcess;
        CloseHandle(pi.hThread);

        for (int i = 0; i < 300; ++i) { // 最长 15s
            hwnd_ = ReadHwndFromReport();
            if (hwnd_)
                return true;
            Sleep(50);
        }
        return false;
    }

    void Stop() {
        if (process_) {
            WaitForSingleObject(process_, 1000);
            if (WaitForSingleObject(process_, 0) != WAIT_OBJECT_0)
                TerminateProcess(process_, 0);
            CloseHandle(process_);
            process_ = nullptr;
        }
        if (!report_.empty())
            DeleteFileW(report_.c_str());
    }

    HWND hwnd() const { return hwnd_; }

  private:
    HWND ReadHwndFromReport() {
        HANDLE f = CreateFileW(report_.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE)
            return nullptr;
        char buf[512] = {};
        DWORD n = 0;
        const BOOL ok = ReadFile(f, buf, sizeof(buf) - 1, &n, nullptr);
        CloseHandle(f);
        if (!ok || n == 0)
            return nullptr;
        const std::string s(buf, n);
        const auto k = s.find("hwnd=0x");
        if (k == std::string::npos)
            return nullptr;
        const auto num = strtoull(s.c_str() + k + 7, nullptr, 16);
        return reinterpret_cast<HWND>(num);
    }

    HANDLE process_ = nullptr;
    HWND hwnd_ = nullptr;
    std::wstring report_;
};

TEST(MemoryReadWriteTest, RemoteHandleCacheHitsAcrossRepeatedReads) {
    PlainCarrier carrier;
    ASSERT_TRUE(carrier.Start());
    CApiHandle api;
    const auto hwnd = reinterpret_cast<intptr_t>(carrier.hwnd());

    // 模块基址必然存在且可读，作为稳定的远程读目标。
    const wchar_t *base_str = OpGetModuleBaseAddr(api.handle, hwnd, L"plain_carrier.exe");
    ASSERT_NE(base_str, nullptr);
    const std::wstring base = base_str;
    ASSERT_FALSE(base.empty());

    // 1) 缓存关闭：两次读应成功且结果一致（基址是 PE 头，MZ 签名 i16 = 0x5A4D）。
    ASSERT_EQ(OpSetMemHandleCacheEnabled(api.handle, 0), 1);
    int64_t v0 = 0, v1 = 0;
    OpReadInt(api.handle, hwnd, base.c_str(), 1, &v0); // i16
    OpReadInt(api.handle, hwnd, base.c_str(), 1, &v1);
    EXPECT_EQ(v0, 0x5A4D);
    EXPECT_EQ(v0, v1);

    // 2) 开启缓存：首次 miss、其后 hit，读值与关闭缓存时完全一致。
    ASSERT_EQ(OpSetMemHandleCacheEnabled(api.handle, 1), 1);
    EXPECT_EQ(OpGetMemHandleCacheHits(api.handle), 0);
    EXPECT_EQ(OpGetMemHandleCacheMisses(api.handle), 0);

    int64_t v2 = 0, v3 = 0;
    OpReadInt(api.handle, hwnd, base.c_str(), 1, &v2);
    EXPECT_EQ(OpGetMemHandleCacheHits(api.handle), 0);
    EXPECT_EQ(OpGetMemHandleCacheMisses(api.handle), 1);

    OpReadInt(api.handle, hwnd, base.c_str(), 1, &v3);
    EXPECT_EQ(OpGetMemHandleCacheHits(api.handle), 1);
    EXPECT_EQ(OpGetMemHandleCacheMisses(api.handle), 1);
    EXPECT_EQ(v2, v0);
    EXPECT_EQ(v3, v0);

    // 3) 关闭缓存：清空句柄并回落逐次 Attach，读仍成功，计数不再增长。
    ASSERT_EQ(OpSetMemHandleCacheEnabled(api.handle, 0), 1);
    int64_t v4 = 0;
    OpReadInt(api.handle, hwnd, base.c_str(), 1, &v4);
    EXPECT_EQ(v4, v0);
    EXPECT_EQ(OpGetMemHandleCacheHits(api.handle), 1);
    EXPECT_EQ(OpGetMemHandleCacheMisses(api.handle), 1);
}

} // namespace
