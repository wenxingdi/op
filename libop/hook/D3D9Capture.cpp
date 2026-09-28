#include "D3D9Capture.h"

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

namespace op::hook {

using ATL::CComPtr;
using op::capture::FrameInfo;

class D3D9TextureLock {
  public:
    explicit D3D9TextureLock(IDirect3DTexture9 *texture) : texture_(texture) {
    }

    ~D3D9TextureLock() {
        if (locked_) {
            texture_->UnlockRect(0);
        }
    }

    D3D9TextureLock(const D3D9TextureLock &) = delete;
    D3D9TextureLock &operator=(const D3D9TextureLock &) = delete;

    HRESULT lock(D3DLOCKED_RECT *lockedRect) {
        if (!texture_ || !lockedRect) {
            return E_POINTER;
        }
        HRESULT hr = texture_->LockRect(0, lockedRect, nullptr, D3DLOCK_READONLY);
        if (hr >= 0) {
            locked_ = true;
        }
        return hr;
    }

  private:
    IDirect3DTexture9 *texture_;
    bool locked_ = false;
};

HRESULT dx9_capture(LPDIRECT3DDEVICE9 pDevice) {
    // 失败路径统一：setlog 留证 + 关闭捕获。原实现只 return，既不关捕获也无日志，
    // 于是每帧重复付 GetBackBuffer/CreateTexture 的代价，失败原因完全不可见。
    HRESULT hr = S_OK;
    CComPtr<IDirect3DSurface9> pSurface;
    hr = pDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pSurface);
    if (FAILED(hr)) {
        setlog("dx9 GetBackBuffer failed hr=%X, disable capture", hr);
        DisplayHook::set_capture_enabled(false);
        return hr;
    }

    D3DSURFACE_DESC surface_Desc;
    hr = pSurface->GetDesc(&surface_Desc);
    if (FAILED(hr)) {
        setlog("dx9 surface->GetDesc failed hr=%X, disable capture", hr);
        DisplayHook::set_capture_enabled(false);
        return hr;
    }

    CComPtr<IDirect3DTexture9> pTex;
    CComPtr<IDirect3DSurface9> pTexSurface;
    hr = pDevice->CreateTexture(surface_Desc.Width, surface_Desc.Height, 1, 0, surface_Desc.Format,
                                D3DPOOL_SYSTEMMEM, // 必须为这个
                                &pTex, NULL);
    if (hr < 0) {
        setlog("dx9 CreateTexture failed hr=%X, disable capture", hr);
        DisplayHook::set_capture_enabled(false);
        return hr;
    }
    hr = pTex->GetSurfaceLevel(0, &pTexSurface);
    if (hr < 0) {
        setlog("dx9 GetSurfaceLevel failed hr=%X, disable capture", hr);
        DisplayHook::set_capture_enabled(false);
        return hr;
    }
    // H3: MSAA 后备缓冲不能直接 GetRenderTargetData（恒返 D3DERR_INVALIDCALL=8876086C），
    // 必须先把多重采样面 resolve 成单采样面。D3D10/11 一直有 SampleDesc.Count>1 分支，
    // D3D9 缺失 —— 表现是"绑得上、一帧也截不到"，且旧代码连日志都没有。
    // 这里补上：StretchRect 到同尺寸同格式的非 MSAA RenderTarget（D3DPOOL_DEFAULT，
    // 与后备缓冲同池，StretchRect 才允许），再对结果取 RenderTargetData。
    // 调用时机是原 EndScene 已返回之后，不在 Begin/EndScene 之间，StretchRect 合法。
    IDirect3DSurface9 *copySource = pSurface;
    CComPtr<IDirect3DSurface9> pResolveSurface;
    if (surface_Desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
        hr = pDevice->CreateRenderTarget(surface_Desc.Width, surface_Desc.Height, surface_Desc.Format,
                                         D3DMULTISAMPLE_NONE, 0, FALSE, &pResolveSurface, NULL);
        if (FAILED(hr)) {
            setlog("dx9 CreateRenderTarget(resolve) failed hr=%X msaa=%d, disable capture", hr,
                   static_cast<int>(surface_Desc.MultiSampleType));
            DisplayHook::set_capture_enabled(false);
            return hr;
        }
        hr = pDevice->StretchRect(pSurface, NULL, pResolveSurface, NULL, D3DTEXF_NONE);
        if (FAILED(hr)) {
            setlog("dx9 StretchRect(msaa resolve) failed hr=%X msaa=%d, disable capture", hr,
                   static_cast<int>(surface_Desc.MultiSampleType));
            DisplayHook::set_capture_enabled(false);
            return hr;
        }
        copySource = pResolveSurface;
    }

    hr = pDevice->GetRenderTargetData(copySource, pTexSurface);
    if (FAILED(hr)) {
        setlog("dx9 GetRenderTargetData failed hr=%X msaa=%d, disable capture", hr,
               static_cast<int>(surface_Desc.MultiSampleType));
        DisplayHook::set_capture_enabled(false);
        return hr;
    }

    D3DLOCKED_RECT lockedRect = {};

    D3D9TextureLock textureLock(pTex);
    hr = textureLock.lock(&lockedRect);
    if (FAILED(hr)) {
        setlog("dx9 LockRect failed hr=%X, disable capture", hr);
        DisplayHook::set_capture_enabled(false);
        return hr;
    }
    // 取像素
    SharedMemory mem;
    ProcessMutex mutex;
    if (mem.open(DisplayHook::shared_res_name) && mutex.open(DisplayHook::mutex_name)) {
        mutex.lock();
        uchar *pshare = mem.data<byte>();
        if (SharedFrameHasCapacity(mem, surface_Desc.Width, surface_Desc.Height)) {
            reinterpret_cast<FrameInfo *>(pshare)->format(DisplayHook::render_hwnd, surface_Desc.Width,
                                                          surface_Desc.Height);
            CopyImageData(reinterpret_cast<char *>(pshare + sizeof(FrameInfo)),
                          reinterpret_cast<const char *>(lockedRect.pBits), surface_Desc.Height, surface_Desc.Width,
                          lockedRect.Pitch, IBF_B8G8R8A8);
        } else {
            WriteSharedFrameHeader(mem, DisplayHook::render_hwnd, surface_Desc.Width, surface_Desc.Height);
        }
        mutex.unlock();
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE dx9_hkEndScene(IDirect3DDevice9 *thiz) {
    // H2: 必须构造在最外层 —— 它要覆盖下面那次跳板调用，release 才敢释放 trampoline。
    DetourScope guard;
    typedef long(__stdcall * EndScene)(LPDIRECT3DDEVICE9);
    auto ret = ((EndScene)DisplayHook::old_address.load(std::memory_order_acquire))(thiz);
    if (DisplayHook::capture_enabled())
        dx9_capture(thiz);

    return ret;
}

} // namespace op::hook
