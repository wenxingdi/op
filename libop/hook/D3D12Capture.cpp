// Copyright 2022 Eugen Hartmann.
// Licensed under the MIT License (MIT).

#include "D3D12Capture.h"

#include "DisplayHook.h"
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

D3D12Capture *D3D12Capture::Get() {
    static D3D12Capture hook;
    return &hook;
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
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain3;
    hr = swapChain->QueryInterface(__uuidof(IDXGISwapChain3), &swapChain3);
    if (FAILED(hr)) {
        return;
    }

    Microsoft::WRL::ComPtr<ID3D12Device> device;
    hr = swapChain->GetDevice(__uuidof(ID3D12Device), &device);
    if (FAILED(hr)) {
        return;
    }

    // 首次调用时自建 DIRECT 拷贝队列与围栏（拷贝是独立队列，无需游戏原 queue）。
    if (!copyQueue_) {
        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&copyQueue_));
        if (FAILED(hr)) {
            return;
        }
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
        if (FAILED(hr)) {
            return;
        }
    }

    // 等待上一帧提交的 GPU 拷贝完成，再读 readback（防读到半写帧）。
    if (fenceValue_ > 0 && fence_) {
        if (fence_->GetCompletedValue() < fenceValue_) {
            if (fenceEvent_) {
                fence_->SetEventOnCompletion(fenceValue_, fenceEvent_);
                ::WaitForSingleObject(fenceEvent_, INFINITE);
            }
        }
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    hr = swapChain->GetBuffer(swapChain3->GetCurrentBackBufferIndex(), __uuidof(ID3D12Resource), &resource);
    if (FAILED(hr)) {
        return;
    }

    D3D12_RESOURCE_DESC desc = resource->GetDesc();

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
                return;
            }
        }

        UINT frameRowPitch = readbackDataPitch_;
        UINT frameWidth = readbackDataWidth_;
        UINT frameHeight = readbackDataHeight_;

        SharedMemory mem;
        ProcessMutex mutex;
        static int cnt = 10;
        int fmt = IBF_R8G8B8A8;
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
            int tf = IBF_R8G8B8A8;

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
            return;
        }

        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator_));
        if (FAILED(hr)) {
            return;
        }
    }

    // command allocator 每次记录命令前必须 Reset（上一帧 ExecuteCommandLists 已由围栏等待完成）。
    if (!commandAllocator_) {
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator_));
        if (FAILED(hr)) {
            return;
        }
    }
    hr = commandAllocator_->Reset();
    if (FAILED(hr)) {
        return;
    }

    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> copyCommandList;
    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator_.Get(), nullptr,
                                   IID_PPV_ARGS(&copyCommandList));
    if (FAILED(hr)) {
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
    typedef long(__stdcall * Present_t)(IDXGISwapChain * pswapchain, UINT x1, UINT x2);
    // DXGI_PRESENT_TEST 不提交真实帧，D3D12 路径也保持和 D3D10/11 一致。
    if (DisplayHook::capture_enabled() && !(Flags & DXGI_PRESENT_TEST))
        dx12_capture(thiz);
    return ((Present_t)DisplayHook::old_address)(thiz, SyncInterval, Flags);
}

} // namespace op::hook
