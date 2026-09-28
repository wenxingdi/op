// hook 显示捕获的真机冒烟底座
//
// 为什么需要本文件：
//   tests/ 里其余窗口载体都是纯 GDI（FillRect），没有真实 D3D/OpenGL 交换链，
//   于是 DisplayHook 装的 Present / EndScene / wglSwapBuffers detour 在回归里从未被触发过 ——
//   D3D9/10/11/12 + OpenGL 五条通道长期处于"零真机闭环"状态。
//
// 做法：启动 scripts/dx_carrier.cpp 编出的**独立进程**载体（真交换链 + 四象限纯色），
//      走 op 的真实路径 HookCapture::BindEx -> blackbone 注入 -> 远程 SetDisplayHook，
//      再断言取回的像素内容。这也正是 OPTool 绑定 BlueStacks 的同一条路径（跨进程）。
//
// 为什么不只看绑定返回值：捕获失败是**静默**的 —— bind 返回 1、尺寸与字节数全正常，
// 只是内容全黑。所以每条用例都必须断言像素颜色，只断言 ret==1 等于没断言。

#include "test_support.h"

#include "../libop/base/AutomationModes.h"
#include "../libop/hook/DisplayHook.h"   // CopyImageData 声明在此
#include "../libop/hook/DxCaptureCommon.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

struct QuadrantCase {
    const wchar_t *label;
    int x;
    int y;
    const wchar_t *expected_rgb;
};

// 与 scripts/dx_carrier.cpp 的 g_quad 严格一致
const QuadrantCase kQuadrants[] = {
    {L"左上", 60, 60, L"FF0000"},
    {L"右上", 340, 60, L"00FF00"},
    {L"左下", 60, 240, L"0000FF"},
    {L"右下", 340, 240, L"FFFFFF"},
};

// 载体定位：环境变量优先（便于指向预编译副本），否则取 op_test.exe 同目录的 dx_carrier.exe
// —— CMake 把两个目标生成到同一输出目录。
std::wstring CarrierExecutable() {
    wchar_t from_env[1024] = {};
    if (GetEnvironmentVariableW(L"OP_DX_CARRIER_EXE", from_env, 1024) > 0)
        return std::wstring(from_env);

    wchar_t self[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, self, MAX_PATH))
        return std::wstring();
    std::wstring dir(self);
    const size_t cut = dir.find_last_of(L"\\/");
    if (cut == std::wstring::npos)
        return std::wstring();
    return dir.substr(0, cut) + L"\\dx_carrier.exe";
}

// op 注入用的 DLL 与本测试模块同目录（build 树下），从已加载模块反推，避免写死路径。
std::wstring OpModuleDirectory() {
    HMODULE module = GetModuleHandleW(L"op_c_api_x64.dll");
    if (!module)
        module = GetModuleHandleW(L"op_c_api.dll");
    if (!module)
        return std::wstring();
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(module, path, MAX_PATH))
        return std::wstring();
    std::wstring dir(path);
    const size_t cut = dir.find_last_of(L"\\/");
    return cut == std::wstring::npos ? std::wstring() : dir.substr(0, cut);
}

std::wstring TempReportPath() {
    wchar_t dir[MAX_PATH] = {};
    const DWORD length = GetTempPathW(MAX_PATH, dir);
    wchar_t name[128] = {};
    swprintf_s(name, L"op_dx_carrier_%lu_%llu.txt", GetCurrentProcessId(),
               static_cast<unsigned long long>(GetTickCount64()));
    return std::wstring(dir, length) + name;
}

bool ReadWholeAsciiFile(const std::wstring &path, std::wstring *out) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    char buffer[1024] = {};
    DWORD read = 0;
    const BOOL ok = ReadFile(file, buffer, sizeof(buffer) - 1, &read, nullptr);
    CloseHandle(file);
    if (!ok || read == 0)
        return false;
    out->clear();
    out->reserve(read);
    for (DWORD i = 0; i < read; ++i)
        out->push_back(static_cast<wchar_t>(static_cast<unsigned char>(buffer[i])));
    return true;
}

// 载体进程：独立 exe，就绪后把 hwnd 写进 report 文件供轮询。
// 用文件而不是管道：进程尚未就绪时读管道会阻塞，轮询文件没有这个问题。
class CarrierProcess {
  public:
    CarrierProcess() = default;
    ~CarrierProcess() { Stop(); }
    CarrierProcess(const CarrierProcess &) = delete;
    CarrierProcess &operator=(const CarrierProcess &) = delete;

    bool Start(const std::wstring &exe, const wchar_t *backend, const wchar_t *extra) {
        Stop();
        report_path_ = TempReportPath();
        DeleteFileW(report_path_.c_str());

        std::wstring command = L"\"" + exe + L"\" --backend " + backend +
                               L" --seconds 40 --report \"" + report_path_ + L"\"";
        if (extra && *extra)
            command += std::wstring(L" ") + extra;

        STARTUPINFOW startup = {};
        startup.cb = sizeof(startup);
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                            nullptr, &startup, &process_)) {
            error_ = L"CreateProcessW 失败 err=" + std::to_wstring(GetLastError());
            return false;
        }

        const ULONGLONG deadline = GetTickCount64() + 15000;
        std::wstring content;
        while (GetTickCount64() < deadline) {
            if (WaitForSingleObject(process_.hProcess, 0) == WAIT_OBJECT_0) {
                error_ = L"载体进程在建好渲染目标前退出（后端在本机不可用）";
                return false;
            }
            if (ReadWholeAsciiFile(report_path_, &content) && content.find(L"READY") != std::wstring::npos)
                break;
            Sleep(50);
        }
        if (content.find(L"READY") == std::wstring::npos) {
            error_ = L"载体未在 15s 内就绪";
            return false;
        }

        const size_t pos = content.find(L"hwnd=0x");
        if (pos == std::wstring::npos) {
            error_ = L"回报文件缺少 hwnd";
            return false;
        }
        const size_t begin = pos + 7;
        const size_t end = content.find_first_of(L"\r\n", begin);
        const std::wstring hex = content.substr(begin, end - begin);
        hwnd_ = reinterpret_cast<HWND>(static_cast<uintptr_t>(wcstoull(hex.c_str(), nullptr, 16)));
        return hwnd_ != nullptr;
    }

    void Stop() {
        if (process_.hProcess) {
            TerminateProcess(process_.hProcess, 0);
            WaitForSingleObject(process_.hProcess, 2000);
        }
        if (process_.hThread)
            CloseHandle(process_.hThread);
        if (process_.hProcess)
            CloseHandle(process_.hProcess);
        process_ = {};
        if (!report_path_.empty()) {
            DeleteFileW(report_path_.c_str());
            report_path_.clear();
        }
        hwnd_ = nullptr;
    }

    HWND hwnd() const { return hwnd_; }
    const std::wstring &error() const { return error_; }

  private:
    PROCESS_INFORMATION process_ = {};
    HWND hwnd_ = nullptr;
    std::wstring report_path_;
    std::wstring error_;
};

struct CaptureOutcome {
    bool carrier_ok = false;
    bool bound = false;
    long bind_ret = 0;
    std::vector<std::wstring> lines;   // 每个象限一行诊断
    int mismatch = 0;
    std::wstring note;
};

class HookCaptureTest : public ::testing::Test {
  protected:
    void SetUp() override {
        exe_ = CarrierExecutable();
        if (exe_.empty() || GetFileAttributesW(exe_.c_str()) == INVALID_FILE_ATTRIBUTES)
            GTEST_SKIP() << "dx_carrier 载体不存在（需先构建 dx_carrier 目标）";
        dll_dir_ = OpModuleDirectory();
        if (dll_dir_.empty())
            GTEST_SKIP() << "无法定位 op 模块目录，注入路径不可用";
    }

    // 起载体 -> 绑定 -> 逐象限比对 -> 解绑
    CaptureOutcome Capture(const wchar_t *backend, const wchar_t *display, const wchar_t *extra = L"") {
        CaptureOutcome outcome;
        CarrierProcess carrier;
        if (!carrier.Start(exe_, backend, extra)) {
            outcome.note = carrier.error();
            return outcome;
        }
        outcome.carrier_ok = true;
        Trace("carrier ready");

        op::Op op;
        long ret = 0;
        op.SetShowErrorMsg(2, &ret);
        op.SetPath(dll_dir_.c_str(), &ret);
        Trace("op path set");

        ret = 0;
        op.BindWindow(static_cast<long>(reinterpret_cast<intptr_t>(carrier.hwnd())), display, L"windows",
                      L"windows", 0, &ret);
        outcome.bind_ret = ret;
        if (ret != 1) {
            outcome.note = L"BindWindow 返回 0（详见 cwd/__op.log）";
            return outcome;
        }
        outcome.bound = true;
        Trace("bound");

        // 等 hook 装好 + 目标产生若干帧（op 侧单帧等待阈值 200ms）
        Sleep(1200);
        Trace("settled");

        for (const QuadrantCase &quadrant : kQuadrants) {
            std::wstring color;
            op.GetColor(quadrant.x, quadrant.y, color);
            Trace("got color");
            const bool ok = _wcsicmp(color.c_str(), quadrant.expected_rgb) == 0;
            if (!ok)
                ++outcome.mismatch;
            outcome.lines.push_back(std::wstring(quadrant.label) + L"(" + std::to_wstring(quadrant.x) +
                                    L"," + std::to_wstring(quadrant.y) + L")=" + color + L" 期望 " +
                                    quadrant.expected_rgb + (ok ? L" OK" : L" MISMATCH"));
        }

        ret = 0;
        op.UnBindWindow(&ret);
        Trace("unbound");
        return outcome;
    }

    // 把诊断一次性打全：一行里多个 EXPECT 串联会让首个 FAIL 遮住其余
    void ReportOutcome(const CaptureOutcome &outcome) {
        for (const std::wstring &line : outcome.lines)
            ADD_FAILURE() << "  " << line;
    }

    // 埋点：SEH（访问违例）被 gtest 兜住时不打印调用栈，只能靠这行定位最后执行到的阶段
    static void Trace(const char *stage) {
        printf("[hooktest] stage: %s\n", stage);
        fflush(stdout);
    }

    std::wstring exe_;
    std::wstring dll_dir_;
};

// ---------------------------------------------------------------- 正向用例

// D3D11 应用 + dx.d3d11：hook IDXGISwapChain::Present，取回后缓冲内容。
TEST_F(HookCaptureTest, Dx11SwapChainIsCapturedWithQuadrantPixels) {
    const CaptureOutcome outcome = Capture(L"d3d11", L"dx.d3d11");
    if (!outcome.carrier_ok)
        GTEST_SKIP() << "载体不可用：" << outcome.note;
    ASSERT_TRUE(outcome.bound) << outcome.note;
    EXPECT_EQ(outcome.mismatch, 0) << "取到的不是载体渲染的画面（全黑/红蓝互换/错位都会落到这里）";
    if (outcome.mismatch != 0)
        ReportOutcome(outcome);
}

// D3D9 应用 + dx.d3d9：hook IDirect3DDevice9::EndScene。
// 与 D3D11 分列两条用例，因为 op 侧走的是完全不同的 detour 与取帧实现
// （D3D9 缺 MSAA resolve 分支，是本轮扫描里的 H3）。
TEST_F(HookCaptureTest, Dx9EndSceneIsCapturedWithQuadrantPixels) {
    const CaptureOutcome outcome = Capture(L"d3d9", L"dx.d3d9");
    if (!outcome.carrier_ok)
        GTEST_SKIP() << "载体不可用：" << outcome.note;
    ASSERT_TRUE(outcome.bound) << outcome.note;
    EXPECT_EQ(outcome.mismatch, 0) << "D3D9 通道取到的画面不正确";
    if (outcome.mismatch != 0)
        ReportOutcome(outcome);
}

// OpenGL 应用 + opengl：hook wglSwapBuffers。
TEST_F(HookCaptureTest, OpenGlSwapBuffersIsCapturedWithQuadrantPixels) {
    const CaptureOutcome outcome = Capture(L"opengl", L"opengl");
    if (!outcome.carrier_ok)
        GTEST_SKIP() << "载体不可用：" << outcome.note;
    ASSERT_TRUE(outcome.bound) << outcome.note;
    EXPECT_EQ(outcome.mismatch, 0) << "OpenGL 通道取到的画面不正确";
    if (outcome.mismatch != 0)
        ReportOutcome(outcome);
}

// opengl.std hook glBegin、opengl.fi hook glFinish：两条都依赖应用真的调用对应 GL 函数，
// 载体为此专门补了空的 glBegin/glEnd 与 glFinish。若哪天 op 侧改了 hook 点，这里会 FAIL。
TEST_F(HookCaptureTest, OpenGlLegacyHookPointsStillProduceFrames) {
    for (const wchar_t *display : {L"opengl.std", L"opengl.fi"}) {
        const CaptureOutcome outcome = Capture(L"opengl", display);
        if (!outcome.carrier_ok)
            GTEST_SKIP() << "载体不可用：" << outcome.note;
        ASSERT_TRUE(outcome.bound) << display << " " << outcome.note;
        EXPECT_EQ(outcome.mismatch, 0) << display << " 通道取到的画面不正确";
        if (outcome.mismatch != 0)
            ReportOutcome(outcome);
    }
}

// ---------------------------------------------------------------- 反向验证 / 现状固化

// 反向验证：载体渲单色（左上象限恰好也是红）时，四象限判据必须至少报 3 处不符。
// 若这里 mismatch 为 0，说明判据退化成"读到任意画面就算成功"，前三条用例的判别力是假的。
TEST_F(HookCaptureTest, SolidCarrierMustBreakQuadrantExpectation) {
    const CaptureOutcome outcome = Capture(L"d3d11", L"dx.d3d11", L"--solid FF0000");
    if (!outcome.carrier_ok)
        GTEST_SKIP() << "载体不可用：" << outcome.note;
    ASSERT_TRUE(outcome.bound) << outcome.note;
    EXPECT_GE(outcome.mismatch, 3)
        << "单色画面下四象限判据几乎全部通过，说明断言失去判别力（判据必须能识别错误画面）";
    if (outcome.mismatch < 3)
        ReportOutcome(outcome);
}

// 现状固化：display="dx"（RDT_DX_DEFAULT）在 locate_render_method 里与 RDT_DX_D3D9 同分支，
// 因此它只 hook EndScene —— 对 D3D11 应用不会有任何 EndScene 调用，取不到帧（日志会打
// "target produced no present frame"），但 BindWindow 仍返回 1。
// 断言"取不到内容"而非"失败"：这是把当前行为钉住，避免它哪天悄悄变化而无人察觉。
TEST_F(HookCaptureTest, LegacyDxAliasDoesNotCaptureD3D11Application) {
    const CaptureOutcome outcome = Capture(L"d3d11", L"dx");
    if (!outcome.carrier_ok)
        GTEST_SKIP() << "载体不可用：" << outcome.note;
    ASSERT_TRUE(outcome.bound) << "dx 别名在 D3D11 窗口上绑定应成功（当前实现如此）";
    // 载体左上角是红；读到 000000 说明根本没有帧
    EXPECT_EQ(outcome.mismatch, 4)
        << "dx 别名行为发生变化：要么已支持自动适配 D3D11（应更新模式文档与用例），"
           "要么捕获到了错误内容";
    if (outcome.mismatch != 4)
        ReportOutcome(outcome);
}

// 现状固化 + 环境记录：dx.d3d10 走 kiero Implementation_D3D10 locate。
// 本机 locate 失败（日志 error=2）→ 绑定被拒。别的机器若能 locate 成功则必须取到正确画面，
// 所以这里两种结果都不算失败，只把事实记录下来。
TEST_F(HookCaptureTest, D3D10ChannelEitherBindsOrReportsLocateFailure) {
    const CaptureOutcome outcome = Capture(L"d3d11", L"dx.d3d10");
    if (!outcome.carrier_ok)
        GTEST_SKIP() << "载体不可用：" << outcome.note;
    if (!outcome.bound) {
        GTEST_SKIP() << "本机 kiero locate<D3D10> 失败，绑定被拒（记录事实，不作为失败）："
                     << outcome.note;
    }
    EXPECT_EQ(outcome.mismatch, 0) << "dx.d3d10 绑定成功但画面不正确";
    if (outcome.mismatch != 0)
        ReportOutcome(outcome);
}

// ------------------------------------------------ 交换链格式白名单（纯函数，无需载体）
//
// 这一组用例针对的是最隐蔽的一类故障：**静默错色**。
// 捕获侧若把读不懂的交换链格式（HDR / 10bit / 浮点）默认当成 R8G8B8A8，像素会照常写入、
// 尺寸与字节数全正常、Capture 也返回成功，只有颜色是错的 —— 只有人眼看着才发现。
//
// 反向验证：把 DxCaptureCommon.cpp 的 GetImageBufferFormat 改回"default: return IBF_R8G8B8A8"，
// 下面第一条用例立即 FAIL；把 CopyImageData 开头的 IBF_UNSUPPORTED 早退删掉，第二条立即 FAIL。
TEST(HookCaptureFormatTest, UnknownSwapChainFormatIsNotSilentlyTreatedAsRgba8) {
    using op::hook::GetImageBufferFormat;

    // 已知可读的两族格式必须精确区分 —— 若两者被映射成同一个值，BGRA 交换链会红蓝互换。
    EXPECT_EQ(GetImageBufferFormat(DXGI_FORMAT_B8G8R8A8_UNORM), IBF_B8G8R8A8);
    EXPECT_EQ(GetImageBufferFormat(DXGI_FORMAT_B8G8R8X8_UNORM), IBF_B8G8R8A8);
    EXPECT_EQ(GetImageBufferFormat(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB), IBF_B8G8R8A8);
    EXPECT_EQ(GetImageBufferFormat(DXGI_FORMAT_B8G8R8A8_TYPELESS), IBF_B8G8R8A8);
    EXPECT_EQ(GetImageBufferFormat(DXGI_FORMAT_R8G8B8A8_UNORM), IBF_R8G8B8A8);
    EXPECT_EQ(GetImageBufferFormat(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB), IBF_R8G8B8A8);
    EXPECT_NE(GetImageBufferFormat(DXGI_FORMAT_B8G8R8A8_UNORM), GetImageBufferFormat(DXGI_FORMAT_R8G8B8A8_UNORM));

    // 未支持格式必须是显式哨兵，不能被猜成 RGBA8。
    // R10G10B10A2 = HDR10 交换链（Win11 自动 HDR / 不少引擎默认后备缓冲）；
    // R16G16B16A16_FLOAT = ScRGB HDR；R11G11B10_FLOAT = 常见的低带宽 HDR 后备缓冲。
    // 三者都是 4 字节/像素，按 RGBA8 逐通道重排会得到完全错误的颜色而毫无报错。
    EXPECT_EQ(GetImageBufferFormat(DXGI_FORMAT_R10G10B10A2_UNORM), IBF_UNSUPPORTED);
    EXPECT_EQ(GetImageBufferFormat(DXGI_FORMAT_R16G16B16A16_FLOAT), IBF_UNSUPPORTED);
    EXPECT_EQ(GetImageBufferFormat(DXGI_FORMAT_R11G11B10_FLOAT), IBF_UNSUPPORTED);
    EXPECT_EQ(GetImageBufferFormat(DXGI_FORMAT_UNKNOWN), IBF_UNSUPPORTED);
}

// 兜底：即便调用方漏判，CopyImageData 也不许按错误字节布局涂共享内存。
TEST(HookCaptureFormatTest, CopyImageDataRefusesUnsupportedFormat) {
    unsigned char dst[16];
    unsigned char src[16];
    std::memset(dst, 0xAB, sizeof(dst));
    std::memset(src, 0x11, sizeof(src));

    op::hook::CopyImageData(reinterpret_cast<char *>(dst), reinterpret_cast<const char *>(src), 1, 4, 16,
                            IBF_UNSUPPORTED);

    for (size_t i = 0; i < sizeof(dst); ++i)
        EXPECT_EQ(dst[i], 0xAB) << "未支持格式不应写入任何像素（第 " << i << " 字节被改写）";
}

} // namespace
