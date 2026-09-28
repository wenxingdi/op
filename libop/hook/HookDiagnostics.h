#pragma once

#include <atomic>

namespace op::hook {

// "每个 hook 会话只做一次"的诊断开关（典型用途：首帧交换链格式日志）。
//
// H20 的背景：原来这类标志写成函数内 `static bool first = true`，随进程活到结束。
// 于是第二次 Bind 之后永远不再打印 —— 用户换了个游戏、换了交换链格式，手里却没有任何
// 证据能对比。抽成显式对象后，DisplayHook::release() 能在每次拆钩时复位它。
//
// 用 atomic 是因为写它的线程是目标进程的渲染线程，而复位它的线程是宿主的绑定/解绑线程。
class OncePerSession {
  public:
    // 首次调用返回 true，其后返回 false。
    bool consume() {
        return !used_.exchange(true, std::memory_order_acq_rel);
    }

    bool used() const {
        return used_.load(std::memory_order_acquire);
    }

    // 复位：下一次 consume() 重新返回 true。
    void reset() {
        used_.store(false, std::memory_order_release);
    }

  private:
    std::atomic<bool> used_{false};
};

} // namespace op::hook
