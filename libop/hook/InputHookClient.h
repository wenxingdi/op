#pragma once

#include <Windows.h>

namespace op::hook::input_hook_client {

long Bind(HWND hwnd, int mode);
long UnBind(HWND hwnd);
long LockInput(HWND hwnd, int lock);
// 下发 dx 输入通道开关（DX_ATTR_* 位掩码）到目标进程内的 Hook。
long SetInputAttr(HWND hwnd, int attrs);
bool GetCursorShape(HWND hwnd, unsigned long long &hash, unsigned long long &meta);

} // namespace op::hook::input_hook_client
