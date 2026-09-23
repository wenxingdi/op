#pragma once

#include <Windows.h>

namespace op::hook::input_hook_client {

long Bind(HWND hwnd, int mode);
long UnBind(HWND hwnd);
long LockInput(HWND hwnd, int lock);
// 下发 dx 输入通道开关（DX_ATTR_* 位掩码）到目标进程内的 Hook。
long SetInputAttr(HWND hwnd, int attrs);
// 绑定后钩子活性回环：对已注入 Hook 的窗口做一次轻量 RPC。
// 注入+SetInputHook 返 1 不代表钩子真能应答（异常残留态/权限边界都可能造成静默假成功），
// 返 1 仅表示远端 Hook 当前可正常应答 RPC。
long PingHook(HWND hwnd);
bool GetCursorShape(HWND hwnd, unsigned long long &hash, unsigned long long &meta);

} // namespace op::hook::input_hook_client
