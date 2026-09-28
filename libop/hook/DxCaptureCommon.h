#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <dxgiformat.h>

namespace op::hook {

// Map/Unmap 必须成对出现，封成小对象能避免失败路径漏掉 Unmap。
class D3D11TextureMap {
  public:
    D3D11TextureMap(ID3D11DeviceContext *context, ID3D11Resource *resource)
        : context_(context), resource_(resource) {
    }

    ~D3D11TextureMap() {
        if (mapped_) {
            context_->Unmap(resource_, 0);
        }
    }

    D3D11TextureMap(const D3D11TextureMap &) = delete;
    D3D11TextureMap &operator=(const D3D11TextureMap &) = delete;

    HRESULT map(D3D11_MAPPED_SUBRESOURCE *mapped) {
        if (!context_ || !resource_ || !mapped) {
            return E_POINTER;
        }
        const HRESULT hr = context_->Map(resource_, 0, D3D11_MAP_READ, 0, mapped);
        if (SUCCEEDED(hr)) {
            mapped_ = true;
        }
        return hr;
    }

  private:
    ID3D11DeviceContext *context_;
    ID3D11Resource *resource_;
    bool mapped_ = false;
};

DXGI_FORMAT NormalizeDxgiFormat(DXGI_FORMAT format);
// 返回 IBF_B8G8R8A8 / IBF_R8G8B8A8，或 IBF_UNSUPPORTED（HDR/10bit/浮点等未列入白名单的格式）。
// 调用方必须显式处理 IBF_UNSUPPORTED：记日志 + DisplayHook::set_capture_enabled(false)，
// 不要退回默认值继续捕获——那会产生"尺寸颜色都对不上但谁也不报错"的静默错帧。
int GetImageBufferFormat(DXGI_FORMAT format);

// H10: staging 纹理的复用判据。设备相同的前提下，这四项任一变化都必须重建。
// 其中"尺寸不变但格式变了"最容易漏（HDR 开关、sRGB 切换）：复用旧格式的 staging
// 会让 CopyResource 失败或产出错色帧，而且全程静默。
struct StagingDescKey {
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT width = 0;
    UINT height = 0;
    UINT sampleCount = 0;

    bool matches(const StagingDescKey &other) const {
        return format == other.format && width == other.width && height == other.height &&
               sampleCount == other.sampleCount;
    }
};

// N1: staging 缓存是否需要重建。
//
// 判据不能只看描述（key）：**设备换了必须连带重建整套缓存**，哪怕尺寸/格式/采样数一字未变。
// 为什么：D3D11 的缓存里除了 staging/resolve 纹理还有 ID3D11DeviceContext。设备被重建
// （驱动 TDR 恢复、切换独显/核显、独占全屏重建 D3D 栈）之后，旧 context 绑的是已销毁的设备，
// 用它去 CopyResource 新设备的纹理属于跨设备调用；而 ID3D11DeviceContext::CopyResource /
// ResolveSubresource 返回 **void** —— 失败既无返回值也无日志，随后 Map 照样成功，
// 于是把未初始化的 staging 当帧写进共享内存：静默错帧。更糟的是缓存是进程级 static，
// 重新绑定不会清它，必须重启目标进程才能恢复。
//
// 注意 device_changed 必须**独立于 key**：换设备时交换链描述常常一字未变（分辨率没变、
// 格式没变），只看 key 会误判为"可复用"。
//
// 反向验证：把 device_changed 从判断里去掉，StagingCacheRebuildTest 立即 FAIL。
inline bool StagingCacheNeedsRebuild(bool has_staging, bool device_changed, const StagingDescKey &cached,
                                     const StagingDescKey &want) {
    return !has_staging || device_changed || !cached.matches(want);
}

} // namespace op::hook
