// Copyright 2022 Eugen Hartmann.
// Licensed under the MIT License (MIT).

#include "D3D12Capture.h"

#include "DisplayHook.h"
#include "DetourGuard.h"
#include "DxCaptureCommon.h"
#include "SharedFrame.h"
#include <directx/d3dx12.h>
#include "../capture/FrameInfo.h"
#include "../ipc/ProcessMutex.h"
#include "../ipc/SharedMemory.h"
#include "../base/AutomationModes.h"
#include "../base/Utils.h"

#define DEBUG_HOOK 0

namespace op::hook {

using op::capture::FrameInfo;

namespace {

// H11: 等围栏的 CPU 侧上限。GPU hang / 驱动 TDR 时，INFINITE 等待会把游戏主线程
// （Present 内部）永久钉死 —— 表现是"游戏卡住不动"。正常一帧拷贝在毫秒级，
// 200ms 已远超合理值；超时只跳过本帧读回，不影响后续帧。
constexpr DWORD kFenceWaitTimeoutMs = 200;

} // namespace

D3D12Capture *D3D12Capture::Get() {
    // H15: 原实现是函数级 static 对象，进程卸载（DllMain / loader lock）时会析构并
    // Release 一批 COM 对象，与 17574a0 修的 ORT 静态析构同类风险。
    // 改 leak-by-design：进程即将退出，泄漏无实际代价（与 OCR/YOLO 一致）。
    static D3D12Capture *hook = new D3D12Capture();
    return hook;
}

D3D12Capture::D3D12Capture() {
    fenceEvent_ = ::CreateEvent(nullptr, FALSE, FALSE, nullptr);
}

D3D12Capture::~D3D12Capture() {
    if (fenceEvent_) {
        ::CloseHandle(fenceEvent_);
        fenceEvent_ = NULL;
    }
}

HRESULT D3D12Capture::CaptureFrames(HWND windowHandleToCapture, std::wstring_view folderToSaveFrames, int maxFrames) {
    if (windowHandleToCapture_ != NULL) {
        return HRESULT_FROM_WIN32(ERROR_BUSY);
    }
    windowHandleToCapture_ = windowHandleToCapture;
    folderToSaveFrames_ = std::wstring(folderToSaveFrames);
    if (folderToSaveFrames_.size() && *folderToSaveFrames_.rbegin() != '\\' && *folderToSaveFrames_.rbegin() != '/') {
        folderToSaveFrames_ += '\\';
    }
    maxFrames_ = maxFrames;
    return S_OK;
}

void D3D12Capture::CaptureFrame(IDXGISwapChain *swapChain) {
    HRESULT hr;

    // D3D12 当前实现沿用原逻辑：本次 present 发起 GPU 拷贝，下次调用读取上一帧 readback。
    // 本函数所有失败路径统一：setlog 留证 + 关闭捕获。原先只 return，导致每帧重复
    // 付 CreateCommandQueue/Fence/Resource 等昂贵操作，且失败原因完全不可见。
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain3;
    hr = swapChain->QueryInterface(__uuidof(IDXGISwapChain3), &swapChain3);
    if (FAILED(hr)) {
        setlog("d3d12 QueryInterface<IDXGISwapChain3> hr=%X", hr);
        DisplayHook::set_capture_enabled(false);
        return;
    }

    Microsoft::WRL::ComPtr<ID3D12Device> device;
    hr = swapChain->GetDevice(__uuidof(ID3D12Device), &device);
    if (FAILED(hr)) {
        setlog("d3d12 swapchain->GetDevice hr=%X", hr);
        DisplayHook::set_capture_enabled(false);
        return;
    }

    // 首次调用时自建 DIRECT 拷贝队列与围栏（拷贝是独立队列，无需游戏原 queue）。
    if (!copyQueue_) {
        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&copyQueue_));
        if (FAILED(hr)) {
            setlog("d3d12 CreateCommandQueue hr=%X", hr);
            DisplayHook::set_capture_enabled(false);
            return;
        }
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
        if (FAILED(hr)) {
            setlog("d3d12 CreateFence hr=%X", hr);
            DisplayHook::set_capture_enabled(false);
            return;
        }
    }

    // 等待上一帧提交的 GPU 拷贝完成，再读 readback（防读到半写帧）。
    // H11: 这里必须是有界等待。INFINITE 在 GPU hang / 驱动 TDR 时会把游戏主线程
    // （Present 内）永久钉死 —— 用户侧表现就是"游戏卡死"。超时放弃本帧读回即可，
    // 下一帧继续等，不会丢 hook 也不会读到半写帧。
    if (fenceValue_ > 0 && fence_) {
        if (fence_->GetCompletedValue() < fenceValue_) {
            if (fenceEvent_) {
                fence_->SetEventOnCompletion(fenceValue_, fenceEvent_);
                const DWORD wait = ::WaitForSingleObject(fenceEvent_, kFenceWaitTimeoutMs);
                if (wait != WAIT_OBJECT_0) {
                    if (!fenceTimeoutLogged_) {
                        setlog("d3d12 fence wait timeout(%lums) completed=%llu expect=%llu, skip readback",
                               static_cast<unsigned long>(kFenceWaitTimeoutMs),
                               static_cast<unsigned long long>(fence_->GetCompletedValue()),
                               static_cast<unsigned long long>(fenceValue_));
                        fenceTimeoutLogged_ = true;
                    }
                    return;
                }
                fenceTimeoutLogged_ = false;
            }
        }
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    hr = swapChain->GetBuffer(swapChain3->GetCurrentBackBufferIndex(), __uuidof(ID3D12Resource), &resource);
    if (FAILED(hr)) {
        setlog("d3d12 swapchain->GetBuffer hr=%X", hr);
        DisplayHook::set_capture_enabled(false);
        return;
    }

    D3D12_RESOURCE_DESC desc = resource->GetDesc();

    // 像素格式必须取自交换链真实格式：原实现硬编码 int fmt = IBF_R8G8B8A8，而 D3D12 交换链
    // 常见 B8G8R8A8 -> CopyImageData 走 RGBA 分支把 B/R 互换 -> 红蓝互换且全程静默。
    // 未支持格式（HDR/10bit/浮点）与 D3D10/11 同款处理：停捕获并留证，不猜格式。
    const int fmt = GetImageBufferFormat(desc.Format);
    if (fmt == IBF_UNSUPPORTED) {
        setlog("d3d12 unsupported swapchain format=%d, disable capture", static_cast<int>(desc.Format));
        DisplayHook::set_capture_enabled(false);
        return;
    }

    UINT64 sizeInBytes;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &sizeInBytes);
    const UINT backBufferWidth = static_cast<UINT>(desc.Width);
    const UINT backBufferHeight = static_cast<UINT>(desc.Height);

    if (readbackResource_.Get() &&
        (readbackDataWidth_ != backBufferWidth || readbackDataHeight_ != backBufferHeight ||
         readbackDataPitch_ != footprint.Footprint.RowPitch)) {
        if (readbackData_) {
            readbackResource_->Unmap(0, nullptr);
            readbackData_ = nullptr;
        }
        readbackResource_.Reset();
        commandAllocator_.Reset();
    }

    if (readbackResource_.Get()) {
        if (readbackData_ == nullptr) {
            hr = readbackResource_->Map(0, nullptr, &readbackData_);
            if (FAILED(hr)) {
                setlog("d3d12 readbackResource_->Map hr=%X", hr);
                DisplayHook::set_capture_enabled(false);
                return;
            }
        }

        UINT frameRowPitch = readbackDataPitch_;
        UINT frameWidth = readbackDataWidth_;
        UINT frameHeight = readbackDataHeight_;

        SharedMemory mem;
        ProcessMutex mutex;
        static int cnt = 10;
        if (mem.open(DisplayHook::shared_res_name) && mutex.open(DisplayHook::mutex_name)) {
            mutex.lock();
            uchar *pshare = mem.data<byte>();
            if (SharedFrameHasCapacity(mem, frameWidth, frameHeight)) {
                reinterpret_cast<FrameInfo *>(pshare)->format(DisplayHook::render_hwnd, frameWidth, frameHeight);
                static_assert(sizeof(FrameInfo) == 28);

                CopyImageData((char *)pshare + sizeof(FrameInfo), (char *)readbackData_, frameHeight, frameWidth,
                              frameRowPitch, fmt);
            } else {
                WriteSharedFrameHeader(mem, DisplayHook::render_hwnd, frameWidth, frameHeight);
            }
            mutex.unlock();
        } else {
#if DEBUG_HOOK
            setlog(L"!mem.open(DisplayHook::%s)&&mutex.open(DisplayHook::%s)", DisplayHook::shared_res_name.c_str(),
                   DisplayHook::mutex_name.c_str());
#endif // DEBUG_HOOK
        }
        static bool first = true;
        if (first) {
            int tf = static_cast<int>(desc.Format);

            setlog("textDesc.Format= %d,fmt=%d textDesc.Height=%d\n textDesc.Width=%d\n  mapSubres.DepthPitch=%d\n "
                   "mapSubres.RowPitch=%d\n",
                   tf, fmt, frameHeight, frameWidth, 0, frameRowPitch);
            first = false;
        }
        if (frameIndex_ >= maxFrames_) {
            windowHandleToCapture_ = NULL;
        }

    } else {
        readbackDataWidth_ = backBufferWidth;
        readbackDataHeight_ = backBufferHeight;
        readbackDataPitch_ = footprint.Footprint.RowPitch;

        auto readbackResourceDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeInBytes);
        auto readbackHeapDesc = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);

        hr = device->CreateCommittedResource(&readbackHeapDesc, D3D12_HEAP_FLAG_NONE, &readbackResourceDesc,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readbackResource_));
        if (FAILED(hr)) {
            setlog("d3d12 CreateCommittedResource(readback) hr=%X", hr);
            DisplayHook::set_capture_enabled(false);
            return;
        }

        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator_));
        if (FAILED(hr)) {
            setlog("d3d12 CreateCommandAllocator(first) hr=%X", hr);
            DisplayHook::set_capture_enabled(false);
            return;
        }
    }

    // command allocator 每次记录命令前必须 Reset（上一帧 ExecuteCommandLists 已由围栏等待完成）。
    if (!commandAllocator_) {
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator_));
        if (FAILED(hr)) {
            setlog("d3d12 CreateCommandAllocator(rebuild) hr=%X", hr);
            DisplayHook::set_capture_enabled(false);
            return;
        }
    }
    hr = commandAllocator_->Reset();
    if (FAILED(hr)) {
        setlog("d3d12 commandAllocator_->Reset hr=%X", hr);
        DisplayHook::set_capture_enabled(false);
        return;
    }

    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> copyCommandList;
    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator_.Get(), nullptr,
                                   IID_PPV_ARGS(&copyCommandList));
    if (FAILED(hr)) {
        setlog("d3d12 CreateCommandList hr=%X", hr);
        DisplayHook::set_capture_enabled(false);
        return;
    }

    D3D12_TEXTURE_COPY_LOCATION dst = CD3DX12_TEXTURE_COPY_LOCATION(readbackResource_.Get(), footprint);
    D3D12_TEXTURE_COPY_LOCATION src = CD3DX12_TEXTURE_COPY_LOCATION(resource.Get(), 0);
    copyCommandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    copyCommandList->Close();

    ID3D12CommandList *commandLists[] = {copyCommandList.Get()};
    copyQueue_->ExecuteCommandLists(ARRAYSIZE(commandLists), commandLists);

    // 本帧拷贝入队完成后推进围栏，下帧 CPU 等待其完成再读 readback。
    ++fenceValue_;
    copyQueue_->Signal(fence_.Get(), fenceValue_);
}

void dx12_capture(IDXGISwapChain *swapChain) {
    D3D12Capture *pinst = D3D12Capture::Get();
    pinst->CaptureFrame(swapChain);
}

HRESULT __stdcall dx12_hkPresent(IDXGISwapChain *thiz, UINT SyncInterval, UINT Flags) {
    // H2: 覆盖下方跳板调用，release 靠该计数确认跳板可安全释放。
    DetourScope guard;
    typedef long(__stdcall * Present_t)(IDXGISwapChain * pswapchain, UINT x1, UINT x2);
    // DXGI_PRESENT_TEST 不提交真实帧，D3D12 路径也保持和 D3D10/11 一致。
    if (DisplayHook::capture_enabled() && !(Flags & DXGI_PRESENT_TEST))
        dx12_capture(thiz);
    return ((Present_t)DisplayHook::old_address.load(std::memory_order_acquire))(thiz, SyncInterval, Flags);
}

} // namespace op::hook
