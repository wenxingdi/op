#include "D3D10Capture.h"

#include "DisplayHook.h"
#include "DetourGuard.h"
#include "DxCaptureCommon.h"
#include "SharedFrame.h"
#include "../capture/FrameInfo.h"
#include "../ipc/ProcessMutex.h"
#include "../ipc/SharedMemory.h"
#include "../base/AutomationModes.h"
#include "../base/Utils.h"
#include <atlbase.h>
#include <cstddef>
#include <d3d10.h>
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

// H10: 原来每帧都 CreateTexture2D 建 staging（开 MSAA 时还多建一张 resolve），
// 帧率越高分配/释放越频繁 —— 白付 D3D 资源开销并制造显存碎片。
// 这里按 设备 + 尺寸/格式/采样数 缓存复用；leak-by-design 不做析构，避免进程
// 卸载时在 loader lock 下释放 D3D 资源（与 H15 同一取舍）。
struct D3D10StagingCache {
    CComPtr<ID3D10Device> device;
    CComPtr<ID3D10Texture2D> staging;
    CComPtr<ID3D10Texture2D> resolve;
    StagingDescKey key;
};

D3D10StagingCache &staging_cache() {
    static D3D10StagingCache *cache = new D3D10StagingCache();
    return *cache;
}

} // namespace

class D3D10TextureMap {
  public:
    explicit D3D10TextureMap(ID3D10Texture2D *texture) : texture_(texture) {
    }

    ~D3D10TextureMap() {
        if (mapped_) {
            texture_->Unmap(0);
        }
    }

    D3D10TextureMap(const D3D10TextureMap &) = delete;
    D3D10TextureMap &operator=(const D3D10TextureMap &) = delete;

    HRESULT map(D3D10_MAPPED_TEXTURE2D *mapped) {
        if (!texture_ || !mapped) {
            return E_POINTER;
        }
        HRESULT hr = texture_->Map(0, D3D10_MAP_READ, 0, mapped);
        if (hr >= 0) {
            mapped_ = true;
        }
        return hr;
    }

  private:
    ID3D10Texture2D *texture_;
    bool mapped_ = false;
};

void dx10_capture(IDXGISwapChain *pswapchain) {
    HRESULT hr;
    CComPtr<ID3D10Device> pdevices;
    CComPtr<ID3D10Resource> backbuffer;

    hr = pswapchain->GetBuffer(0, __uuidof(ID3D10Resource), reinterpret_cast<void **>(&backbuffer.p));
    if (hr < 0) {
        setlog("pswapchain->GetBuffer error code=%d", hr);
        DisplayHook::set_capture_enabled(false);
        return;
    }
    backbuffer->GetDevice(&pdevices);

    if (!pdevices) {
        DisplayHook::set_capture_enabled(false);
        return;
    }

    DXGI_SWAP_CHAIN_DESC desc;
    hr = pswapchain->GetDesc(&desc);
    if (hr < 0) {
        setlog("pswapchain->GetDesc error code=%d", hr);
        DisplayHook::set_capture_enabled(false);
        return;
    }
    D3D10_TEXTURE2D_DESC textDesc = {};
    textDesc.Format = NormalizeDxgiFormat(desc.BufferDesc.Format);
    textDesc.Width = desc.BufferDesc.Width;
    textDesc.Height = desc.BufferDesc.Height;
    textDesc.MipLevels = 1;
    textDesc.ArraySize = 1;
    textDesc.SampleDesc.Count = 1;
    textDesc.Usage = D3D10_USAGE_STAGING;
    textDesc.CPUAccessFlags = D3D10_CPU_ACCESS_READ;

    // H10: 复用缓存，仅当 设备/尺寸/格式/采样数 变化时重建（见 D3D10StagingCache 注释）。
    // N1: 与 D3D11 共用同一判据（D3D10 每帧现取 device 当即时上下文，缓存里没有独立
    // context，所以不存在 D3D11 那个"旧 context 操作新纹理"的坑；但设备换了仍必须重建纹理）。
    D3D10StagingCache &cache = staging_cache();
    const StagingDescKey want{textDesc.Format, textDesc.Width, textDesc.Height, desc.SampleDesc.Count};
    if (StagingCacheNeedsRebuild(cache.staging.p != nullptr, cache.device.p != pdevices.p, cache.key, want)) {
        cache.staging.Release();
        cache.resolve.Release();
        hr = pdevices->CreateTexture2D(&textDesc, nullptr, &cache.staging);
        if (hr < 0) {
            setlog("pdevices->CreateTexture2D error code=%d", hr);
            DisplayHook::set_capture_enabled(false);
            return;
        }
        if (desc.SampleDesc.Count > 1) {
            // MSAA 后备缓冲不能直接复制到 staging，先 resolve 成单采样纹理再读回。
            D3D10_TEXTURE2D_DESC resolveDesc = textDesc;
            resolveDesc.Usage = D3D10_USAGE_DEFAULT;
            resolveDesc.CPUAccessFlags = 0;
            resolveDesc.BindFlags = 0;
            resolveDesc.MiscFlags = 0;
            hr = pdevices->CreateTexture2D(&resolveDesc, nullptr, &cache.resolve);
            if (hr < 0) {
                setlog("pdevices->CreateTexture2D resolved error code=%d", hr);
                DisplayHook::set_capture_enabled(false);
                return;
            }
        }
        cache.device = pdevices;
        cache.key = want;
    }

    ID3D10Resource *copySource = backbuffer;
    if (desc.SampleDesc.Count > 1) {
        pdevices->ResolveSubresource(cache.resolve, 0, backbuffer, 0, textDesc.Format);
        copySource = cache.resolve;
    }

    pdevices->CopyResource(cache.staging, copySource);

    D3D10_MAPPED_TEXTURE2D mapText = {0, 0};

    D3D10TextureMap mappedTexture(cache.staging);
    hr = mappedTexture.map(&mapText);
    if (hr < 0) {
        setlog("textDst->Map false,hr=%d", hr);
        DisplayHook::set_capture_enabled(false);
        return;
    }

    const int fmt = GetImageBufferFormat(textDesc.Format);
    if (fmt == IBF_UNSUPPORTED) {
        // 与 D3D11 同款：未支持格式停捕获而非静默按 RGBA8 涂色（HDR/10bit 交换链）。
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
            write_shared_frame(sharedFrame, DisplayHook::render_hwnd, textDesc.Width, textDesc.Height, mapText.pData,
                               textDesc.Height, textDesc.Width, mapText.RowPitch, fmt);
        } else {
            WriteSharedFrameHeader(mem, DisplayHook::render_hwnd, textDesc.Width, textDesc.Height);
        }
        mutex.unlock();
    } else {
#if DEBUG_HOOK
        setlog("mem.open(DisplayHook::shared_res_name) && mutex.open(DisplayHook::mutex_name)");
#endif // DEBUG_HOOK
    }
}

HRESULT STDMETHODCALLTYPE dx10_hkPresent(IDXGISwapChain *thiz, UINT SyncInterval, UINT Flags) {
    // H2: 覆盖下方跳板调用，release 靠该计数确认跳板可安全释放。
    DetourScope guard;
    typedef long(__stdcall * Present_t)(IDXGISwapChain * pswapchain, UINT x1, UINT x2);
    // DXGI_PRESENT_TEST 只是检查 present 是否可行，不代表产生了新画面，跳过可减少无效读回。
    if (DisplayHook::capture_enabled() && !(Flags & DXGI_PRESENT_TEST))
        dx10_capture(thiz);
    return ((Present_t)DisplayHook::old_address.load(std::memory_order_acquire))(thiz, SyncInterval, Flags);
}

} // namespace op::hook
