#pragma once

#include <atomic>
#include <utility>

void *ResolveApi(const char *mod_name, const char *func_name);

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
