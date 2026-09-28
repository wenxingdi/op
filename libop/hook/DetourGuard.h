#pragma once

// H2：解绑与在途 detour 的竞态。
//
// DisplayHook::release() 会调用 MH_RemoveHook，而 MinHook 在该调用里释放 detour 的
// trampoline —— 也就是 old_address 指向的跳板内存。若此刻渲染线程已经进入 detour，
// 但还没执行到 ((Present_t)old_address)(...)，解冻后就会跳到已释放的内存，目标进程崩溃。
//
// MH_DisableHook / MH_RemoveHook 内部会冻结/解冻其他线程，所以窗口很窄、非必现；
// 但 bb98e92 修的正是输入侧同类"解绑顺序"问题，显示侧需要对齐同一策略：
//
//   停捕获 -> MH_DisableHook -> 等在途计数归零 -> MH_RemoveHook
//
// 计数与等待逻辑独立成类，便于单测覆盖「正常退出 / 超时 / 无在途」三种路径。

#include <atomic>
#include <chrono>
#include <thread>

namespace op::hook {

class DetourGuard {
  public:
    static void enter() {
        s_inflight.fetch_add(1, std::memory_order_acq_rel);
    }

    static void leave() {
        s_inflight.fetch_sub(1, std::memory_order_acq_rel);
    }

    static int inflight() {
        return s_inflight.load(std::memory_order_acquire);
    }

    // 自旋等待在途 detour 全部退出。超时返回 false —— 调用方必须留证，
    // 但仍要继续拆钩（否则 hook 泄漏 + MinHook 引用计数失衡，代价更大）。
    static bool wait_idle(int timeout_ms) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (s_inflight.load(std::memory_order_acquire) != 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

  private:
    inline static std::atomic<int> s_inflight{0};
};

// RAII：必须构造在 detour 函数体的最外层，覆盖到对 old_address 跳板的调用，
// 否则 wait_idle 无法保证"没有线程还在用跳板"。
class DetourScope {
  public:
    DetourScope() {
        DetourGuard::enter();
    }
    ~DetourScope() {
        DetourGuard::leave();
    }
    DetourScope(const DetourScope &) = delete;
    DetourScope &operator=(const DetourScope &) = delete;
};

} // namespace op::hook
