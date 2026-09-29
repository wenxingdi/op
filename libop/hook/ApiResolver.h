#pragma once

#include <atomic>
#include <utility>

void *ResolveApi(const char *mod_name, const char *func_name);

// 按需加载（on-demand）策略：模块尚未加载时主动 LoadLibrary 从 System32 取。
//
// 背景：`ResolveApi` 与 kiero 全程只用 `GetModuleHandleA`，**不主动加载**。于是
// "目标进程此刻还没加载 d3d9.dll / dinput8.dll" 与 "目标永远不用这个 API" 被混为一谈——
// dx 定位器（kiero::locate<Implementation_D3D9>）拿到 Error_ModuleNotFound 就返回 0，
// 宿主看到的是"绑不上"，而真相往往只是**时机问题**：游戏启动早期还没建 D3D 设备，
// 或者 DirectInput 要等进场景才创建。
//
// 语义边界（重要，别把它当"万能兜底"）：
// - 只对**系统图形/输入库**（d3d9/d3d10/d3d11/d3d12/dxgi/dinput8/opengl32/libEGL）使用。
//   加载一律走 `LOAD_LIBRARY_SEARCH_SYSTEM32`，**不从目标进程 cwd 找**，杜绝同名 DLL 劫持；
//   该 flag 在缺少 KB2533623 的老系统上会失败，此时才退回普通 `LoadLibraryA` 并记日志。
// - 加载成功 ≠ 该通道一定能拦到东西。目标若根本不走这条 API（例如游戏只用 Raw Input，
//   从不碰 dinput8），hook 装上了也不会被调用 —— 所以日志必须记 `loaded on demand`，
//   让"绑上了但一帧都没有"可被追溯，而不是又一次"谎报成功"。
// - 关闭方式：环境变量 `OP_NO_ONDEMAND_LOAD=1`（注入侧读取，目标进程的环境块）。
//   默认**开启**：因为关闭时的行为是"目标没加载就直接拒绝整个绑定的输入部分"，
//   而 op 的输入通道不止 dinput 一条（win32 键状态 / Raw Input / 窗口过程），
//   为一条通道未就绪而整体失败反而是更差的结果。
bool OnDemandModuleLoadEnabled();
void *LoadApiModule(const char *mod_name);
void *ResolveApiLazy(const char *mod_name, const char *func_name);

// N2: API 指针缓存。
//
// 背景：gl_capture / egl_capture 原先每帧调 3 次 ResolveApi（内部是 GetModuleHandleA +
// GetProcAddress）。而 gl_hkglBegin 挂在 glBegin 上 —— immediate mode 的渲染一帧可能命中
// 这个 detour 数千次，等于每帧上万次系统查询，纯白付。
//
// 关键语义（也是原实现唯一"歪打正着"的地方）：**解析失败不得当终值缓存**。
// 目标进程可能尚未加载 opengl32.dll / libglesv2.dll（延迟加载），首次解析返回 null；
// 若把这次 null 记成"已解析"，捕获就永久失效了。原实现每帧重试恰好兜住了这一点，
// 改成缓存后必须显式保留该语义：只在**解析成功**时写缓存，失败返回 null 且下次重试。
//
// 反向验证：删掉下面的命中短路 -> "成功只解析一次"断言 FAIL；
//           让失败也成终值（once_flag 语义）-> "失败后重试仍能解析到"断言 FAIL。
template <typename Resolver> inline void *CachedResolveApi(std::atomic<void *> &slot, Resolver &&resolve) {
    if (void *cached = slot.load(std::memory_order_relaxed)) {
        return cached;
    }
    void *resolved = resolve();
    if (!resolved) {
        return nullptr;
    }
    slot.store(resolved, std::memory_order_relaxed);
    return resolved;
}

inline void *CachedResolveApi(std::atomic<void *> &slot, const char *mod_name, const char *func_name) {
    return CachedResolveApi(slot, [=] { return ResolveApi(mod_name, func_name); });
}
