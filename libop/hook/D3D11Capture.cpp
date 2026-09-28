#include "D3D11Capture.h"

#include "DisplayHook.h"
#include "DetourGuard.h"
#include "DxCaptureCommon.h"
#include "HookDiagnostics.h"
#include "SharedFrame.h"
#include "../capture/FrameInfo.h"
#include "../ipc/ProcessMutex.h"
#include "../ipc/SharedMemory.h"
#include "../base/AutomationModes.h"
#include "../base/Utils.h"
#include <atlbase.h>
#include <cstddef>
#include <d3d11.h>
#include <span>

#define DEBUG_HOOK 0

namespace op::hook {

using ATL::CComPtr;
using op::capture::FrameInfo;

namespace {

std::span<std::byte> make_shared_frame_span(SharedMemory &mem, UINT width, UINT height) {
    const auto pixelBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    return {mem.data<std::byte>(), sizeof(FrameInfo) + pixelBytes};
}

void write_shared_frame(std::span<std::byte> sharedFrame, HWND hwnd, UINT width, UINT height, const void *source,
                        int sourceRows, int sourceCols, int rowPitch, int format) {
    // 使用 span 明确区分帧头和像素区，避免共享内存裸指针偏移散落在捕获逻辑里。
    auto frameInfoBytes = sharedFrame.first(sizeof(FrameInfo));
    auto pixelBytes = sharedFrame.subspan(sizeof(FrameInfo));

    reinterpret_cast<FrameInfo *>(frameInfoBytes.data())->format(hwnd, width, height);
    CopyImageData(reinterpret_cast<char *>(pixelBytes.data()), static_cast<const char *>(source), sourceRows,
                  sourceCols, rowPitch, format);
}

// H10: 原实现每帧都 GetImmediateContext + CreateTexture2D 建 staging（开 MSAA 时还多建
// 一张 resolve）。帧率越高分配/释放越密，白付 D3D 资源开销并制造显存碎片。
// 这里按 设备 + 尺寸/格式/采样数 缓存复用；leak-by-design 不做析构，避免进程卸载时
// 在 loader lock 下释放 D3D 资源（与 H15 同一取舍）。
struct D3D11StagingCache {
    CComPtr<ID3D11Device> device;
    CComPtr<ID3D11DeviceContext> context;
    CComPtr<ID3D11Texture2D> staging;
    CComPtr<ID3D11Texture2D> resolve;
    StagingDescKey key;
};

D3D11StagingCache &staging_cache() {
    static D3D11StagingCache *cache = new D3D11StagingCache();
    return *cache;
}

// H20: 原先是函数内 static bool first，活到进程结束 —— 第二次 Bind 起就不再打印首帧格式诊断，
// 排查"换了游戏/换了交换链格式后行为变了"时手里没有任何证据。
// 改为显式对象 + 由 DisplayHook::release() 每次拆钩时复位。
OncePerSession g_firstFrame;

} // namespace

void dx11_reset_diagnostics() {
    g_firstFrame.reset();
}

void dx11_capture(IDXGISwapChain *swapchain) {
    HRESULT hr = 0;
    CComPtr<IDXGIResource> backbufferptr;
    CComPtr<ID3D11Resource> backbuffer;
    CComPtr<ID3D11Device> device;

    hr = swapchain->GetBuffer(0, __uuidof(IDXGIResource), reinterpret_cast<void **>(&backbufferptr.p));
    if (hr < 0) {
        setlog("pswapchain->GetBuffer,error code=%X", hr);
        DisplayHook::set_capture_enabled(false);
        return;
    }
    hr = backbufferptr->QueryInterface(__uuidof(ID3D11Resource), reinterpret_cast<void **>(&backbuffer.p));
    if (hr < 0) {
        setlog("backbufferptr->QueryInterface,error code=%X", hr);
        DisplayHook::set_capture_enabled(false);
        return;
    }
    hr = swapchain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void **>(&device.p));
    if (hr < 0) {
        setlog("swapchain->GetDevice hr=%X", hr);
        DisplayHook::set_capture_enabled(false);
        return;
    }
    DXGI_SWAP_CHAIN_DESC desc;
    hr = swapchain->GetDesc(&desc);
    if (hr < 0) {
        setlog("swapchain->GetDesc hr=%X", hr);
        DisplayHook::set_capture_enabled(false);
        return;
    }

    D3D11_TEXTURE2D_DESC textDesc = {};
    textDesc.Format = NormalizeDxgiFormat(desc.BufferDesc.Format);
    textDesc.Width = desc.BufferDesc.Width;
    textDesc.Height = desc.BufferDesc.Height;
    textDesc.MipLevels = 1;
    textDesc.ArraySize = 1;
    textDesc.SampleDesc.Count = 1;
    textDesc.Usage = D3D11_USAGE_STAGING;
    textDesc.BindFlags = 0;
    textDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    textDesc.MiscFlags = 0;

    // H10: 复用缓存，仅当 设备/尺寸/格式/采样数 变化时重建（见 D3D11StagingCache 注释）。
    // N1: 设备变化必须**连带失效 context**（判据与理由见 StagingCacheNeedsRebuild 注释）。
    D3D11StagingCache &cache = staging_cache();
    const StagingDescKey want{textDesc.Format, textDesc.Width, textDesc.Height, desc.SampleDesc.Count};
    if (StagingCacheNeedsRebuild(cache.staging.p != nullptr, cache.device.p != device.p, cache.key, want)) {
        // 这一行是 N1 的核心：context 与 staging/resolve 同属"绑在某个设备上"的资源，
        // 设备换了却只重建纹理，就会用已销毁设备的 context 去 CopyResource，静默写错帧。
        cache.context.Release();
        cache.staging.Release();
        cache.resolve.Release();
        hr = device->CreateTexture2D(&textDesc, nullptr, &cache.staging);
        if (hr < 0) {
            setlog("device->CreateTexture2D,error code=%d", hr);
            DisplayHook::set_capture_enabled(false);
            return;
        }
        if (desc.SampleDesc.Count > 1) {
            // MSAA 后备缓冲需要先 resolve 成单采样纹理，否则 CopyResource 到 staging 会失败。
            D3D11_TEXTURE2D_DESC resolveDesc = textDesc;
            resolveDesc.Usage = D3D11_USAGE_DEFAULT;
            resolveDesc.BindFlags = 0;
            resolveDesc.CPUAccessFlags = 0;
            resolveDesc.MiscFlags = 0;
            hr = device->CreateTexture2D(&resolveDesc, nullptr, &cache.resolve);
            if (hr < 0) {
                setlog("device->CreateTexture2D resolved,error code=%d", hr);
                DisplayHook::set_capture_enabled(false);
                return;
            }
        }
        cache.device = device;
        cache.key = want;
    }
    if (!cache.context) {
        device->GetImmediateContext(&cache.context);
        if (!cache.context) {
            setlog("!context");
            DisplayHook::set_capture_enabled(false);
            return;
        }
    }

    ID3D11Resource *copySource = backbuffer;
    if (desc.SampleDesc.Count > 1) {
        cache.context->ResolveSubresource(cache.resolve, 0, backbuffer, 0, textDesc.Format);
        copySource = cache.resolve;
    }

    cache.context->CopyResource(cache.staging, copySource);

    D3D11_MAPPED_SUBRESOURCE mapSubres = {0, 0, 0};

    D3D11TextureMap mappedTexture(cache.context, cache.staging);
    hr = mappedTexture.map(&mapSubres);
    if (hr < 0) {
        setlog("context->Map error code=%d", hr);
        DisplayHook::set_capture_enabled(false);
        return;
    }
    const int fmt = GetImageBufferFormat(textDesc.Format);
    if (fmt == IBF_UNSUPPORTED) {
        // HDR(R10G10B10A2) / ScRGB(R16G16B16A16_FLOAT) 等未支持格式：停捕获并留下证据。
        // 曾经这里是静默降级成 RGBA8，颜色全错但 ret/尺寸/字节数都正常，只能目视发现。
        setlog("unsupported swapchain format=%d, disable capture", static_cast<int>(textDesc.Format));
        DisplayHook::set_capture_enabled(false);
        return;
    }

    SharedMemory mem;
    ProcessMutex mutex;
    if (mem.open(DisplayHook::shared_res_name) && mutex.open(DisplayHook::mutex_name)) {
        mutex.lock();
        if (SharedFrameHasCapacity(mem, textDesc.Width, textDesc.Height)) {
            auto sharedFrame = make_shared_frame_span(mem, textDesc.Width, textDesc.Height);
            write_shared_frame(sharedFrame, DisplayHook::render_hwnd, textDesc.Width, textDesc.Height, mapSubres.pData,
                               textDesc.Height, textDesc.Width, mapSubres.RowPitch, fmt);
        } else {
            WriteSharedFrameHeader(mem, DisplayHook::render_hwnd, textDesc.Width, textDesc.Height);
        }
        static_assert(sizeof(FrameInfo) == 28);
        mutex.unlock();
    } else {
        DisplayHook::set_capture_enabled(false);
#if DEBUG_HOOK
        setlog(L"!mem.open(DisplayHook::%s)&&mutex.open(DisplayHook::%s)", DisplayHook::shared_res_name.c_str(),
               DisplayHook::mutex_name.c_str());
#endif // DEBUG_HOOK
    }
    if (g_firstFrame.consume()) {
        setlog("d3d11 first frame: format=%d fmt=%d height=%u width=%u depthPitch=%u rowPitch=%u",
               static_cast<int>(textDesc.Format), fmt, textDesc.Height, textDesc.Width, mapSubres.DepthPitch,
               mapSubres.RowPitch);
    }
}

HRESULT __stdcall dx11_hkPresent(IDXGISwapChain *thiz, UINT SyncInterval, UINT Flags) {
    // H2: 覆盖下方跳板调用，release 靠该计数确认跳板可安全释放。
    DetourScope guard;
    typedef long(__stdcall * Present_t)(IDXGISwapChain * pswapchain, UINT x1, UINT x2);
    // DXGI_PRESENT_TEST 不提交真实帧，避免把测试调用当成截图帧处理。
    if (DisplayHook::capture_enabled() && !(Flags & DXGI_PRESENT_TEST))
        dx11_capture(thiz);
    return ((Present_t)DisplayHook::old_address.load(std::memory_order_acquire))(thiz, SyncInterval, Flags);
}

} // namespace op::hook
