#pragma once

#include <dxgi.h>

namespace op::hook {

HRESULT __stdcall dx11_hkPresent(IDXGISwapChain *thiz, UINT SyncInterval, UINT Flags);

// H20: 复位"首帧诊断已打印"标志，由 DisplayHook::release() 每次拆钩时调用。
void dx11_reset_diagnostics();

} // namespace op::hook
