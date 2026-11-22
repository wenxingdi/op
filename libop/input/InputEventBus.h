#pragma once
#ifndef OP_INPUT_INPUT_EVENT_BUS_H_
#define OP_INPUT_INPUT_EVENT_BUS_H_

// 输入事件总线（时间轴层核心，批 E 2026-11-22）。
//
// ## 为什么挂在后端而不是挂在 op::Op 的方法上
//
// 规划里写的是"找出各输入方法的共同落点，统一走一个 input_event_bus"。读完代码后
// 结论要修正一处：**op::Op 的方法不是共同落点，后端才是**。理由：
//   * `op::Op::LeftClick()` 只做 `bkproc._mouse->LeftClick()` 转发，一行一个方法，
//     在哪一层插钩子都能覆盖到它；
//   * 但后端的合成方法里**包含多次原语**：`LeftClick` = `LeftDown` + `LeftUp`，
//     `LeftDoubleClick` = 两次 `LeftClick`。钩在 Op 层只能看到一个"LeftClick"，
//     **回放时无法还原到底按了几下**；钩在原语（LeftDown/LeftUp）才能如实录到
//     down/up/down/up，回放才有意义。
//   * 三个后端（WinMouse / DxMouse / WinKeyboard / DxKeyboard）各自 override 原语，
//     钩在**每个 override 的出口**是唯一能覆盖全部绑定模式的位置。
//
// ## 录制怎么做到"不录重"
//
// 钩子挂在原语出口后，`LeftDoubleClick` 会经过 LeftDown/LeftUp 两次 → 落 4 条事件。
// 这正是我们要的（真实按下序列）。但如果有人把钩子挪到合成方法上，就会与
// 原语钩子重复。**约定：只在原语 override 里调 Emit，合成方法一律不调。**
//
// ## 回放怎么不与录制互相污染
//
// 回放是通过调用同一批 API 来重放的，如果录制还开着就会把自己录进去（越放越长）。
// 用 **thread_local 深度计数**解决：回放线程里 RecordBypass 生效，录制与主从投递
// 都跳过。用线程局部而不是全局布尔，是因为回放线程与脚本线程可能是两个线程。
//
// ## 主从同步的语义（刻意做成"异步 + 可丢弃"）
//
// 输入是**边沿事件**，迟到即错位（按一个 200ms 前的坐标去点，点的是别处）。
// 所以投递队列满时**丢弃并计数**，而不是阻塞主实例——阻塞会把主实例的输入
// 节奏拖垮，比丢事件更糟。丢弃数通过 `dropped` 报出来，不静默。

#include "InputEvent.h"

#include <Windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace op::input {

class WinMouse;
class KeyboardBackend;

// ---- 单例必须是**进程唯一**（2026-11-22 踩坑，重要）----
//
// 最初把 `InputEventBus::instance()` 做成普通的 Meyer 静态局部变量。它在只加载一个
// DLL 的生产环境里没问题，但**测试进程会同时载入 op_com.dll + op_c_api_x64.dll，
// 而 op_test.exe 自己又直接编进了 WinMouse.cpp / InputEventBus.cpp**：
//   于是进程里存在三份 `instance()`（exe 一份、每个 DLL 各一份），
//   录制钩子在 exe 那份上跑，而 `OpInputPlayback` 通过 op_c_api.dll 操作的是另一份
//   —— 表现为 START 看得见事件、STOP 永远看不到（实测 2s 的间隔照走完）。
//   这个 bug 在发布环境里是"两个 DLL 各自录各自的"，同样错。
//
// 修法：把单例锚在**一个导出函数**上（首个载入的模块赢），其余模块通过它拿同一份。
// 这就是共享 DLL 之间唯一的可靠做法（同一个道理见 hook/ 模块的 SetDisplayHook）。
class InputEventBus; // 前置声明（定义在下面，保持顺序）
struct InputEventBusAccess;

// 进程唯一的输入事件总线。
//
// 2026-11-22 踩坑（重要）：最初写成普通的 Meyers 单例（`static InputEventBus bus;`）。
// 单独跑 DLL 或单独跑 exe 都没问题，但**测试进程同时载入 op_com.dll + op_c_api_x64.dll，
// 而 op_test.exe 又直接编进了 WinMouse.cpp / InputEventBus.cpp**，于是进程里出现了
// 三份 `instance()`：录制钩子在 exe 那份上跑，`OpInputPlayback` 通过 op_c_api.dll
// 操作的是另一份 —— 实测表现为"START 看得见事件、STOP 永远看不到，2s 的间隔照走完"。
// 发布环境里这是同样的错：COM 入口与 C API 入口各录各的。
//
// 修法：单例锚在**一个具名导出函数**上，首个载入的模块赢，其余模块都走它。
// 这与 hook/ 模块的 SetDisplayHook 同一路数（跨 DLL 只能靠导出符号对齐）。
InputEventBus &shared_input_event_bus();

using InputMonitorCallback = void (*)(const InputEvent *event, void *user);

// 主从投递回调（内部用；不使用 InputMonitorCallback 是为了能把"谁发的"带过去）
using InputSyncDeliver = std::function<long(const InputEvent &event)>;

class InputEventBus {
  public:
    static InputEventBus &instance();

    // ---- 采集开关 ----
    // 关闭状态：所有 Emit 立即返回（热路径只有一个原子读）。已有录制在跑时关闭
    // 等于停止写入，不算错误。
    void set_enabled(bool on);
    bool enabled() const;

    // ---- 监听 ----
    // 回调在**调用方线程**同步触发（与 OLA InputMonitorCallback 同构）。
    // 回调里**不要**再调输入 API：会重入 Emit（本实现有 thread_local 护栏，
    // 不会无限递归，但会丢事件）。
    void set_monitor(InputMonitorCallback cb, void *user);

    // ---- 录制 ----
    // path 为空 → 自动生成（工作目录下 input_record_YYYYmmdd_HHMMSS.jsonl）。
    // 已在录制时再次 Start 返回 0（不静默重开把前一段截断）。
    long record_start(const std::wstring &path, long *ret);
    // 返回写入的事件条数；未在录制返回 0。完成时把路径写到 out_path（给
    // InputRecordGetPath 用）。
    long record_stop(std::wstring &out_path);
    bool recording() const;
    std::wstring record_path() const;
    long record_count() const;

    // ---- 回放 ----
    long playback_load(const std::wstring &path, long *ret);
    // 同步回放（本调用会阻塞到回放结束/被 Stop）。speed<=0 视为 1.0。
    // 返回实际重放的事件数；未 Load 返回 -1（->ret=0）。
    // slave_owner_id 指定把事件投给哪个 op 实例（-1 = 任意一个"未绑定主从"的实例）。
    long playback_run(double speed, long loop_count, long owner_id, long *ret);
    void playback_stop();
    long playback_loaded_count() const;
    bool playback_running() const;

    // ---- 诊断（测试与排障用，不进公开 API）----
    // 回放停止标志当前是否为"已请求"。
    bool playback_stop_requested() const;
    // 总线内部状态的标识指针。两个模块拿到同一个指针 ⇒ 它们共享同一条总线；
    // 不同 ⇒ 进程里存在多份单例（跨 DLL 静态变量各一份）。
    // 这条判据比"从 COM 读路径"更直接，也不依赖 tlb 注册。
    const void *identity() const;

    // ---- 主从 ----
    long sync_link(long master_id, long slave_id, long enable, long *ret);
    long sync_unlink_all(long *ret);
    // 当前实例 id 自己的从属关系查询（诊断用）：
    //   {"ok":1,"enabled":1,"master":1,"slave":2,"async_dropped":0}
    std::wstring sync_status(long master_id, long slave_id) const;

    // ---- 事件入口（后端调用）----
    // owner_id = 产生事件的 op 实例 id。
    void emit(long owner_id, InputEvent event);

    // 注册/注销某个 op 实例的输入后端，供回放与主从投递使用。
    // 后端的生命周期与 op::Op 实例一致（BindingSession 持有它们）。
    void register_backends(long instance_id, WinMouse *mouse, KeyboardBackend *keyboard);
    void unregister_backends(long instance_id);

    // 把一条事件重放到指定实例上（回放与主从共用），返回其实例 id（<0 = 找不到）。
    // 说明事件到底打到了哪：调用方拿它做诊断，避免"放完了但没人收到"。
    long replay_on_instance(long owner_id, const InputEvent &event);

    long backends_registered() const;

  private:
    InputEventBus();
    ~InputEventBus();
    InputEventBus(const InputEventBus &) = delete;
    InputEventBus &operator=(const InputEventBus &) = delete;

    // 单例的创建/取用锚在共享槽上（见 .cpp 里的说明），所以这个自由函数
    // 需要能构造实例。
    friend InputEventBus &shared_input_event_bus();

    void ensure_worker_running();

    struct Impl;
    Impl *impl_;
};

// RAII：作用域内该线程产生的事件不录制、不投递主从（回放专用）。
// 用 RAII 而不是显式成对调用，是为了保证异常/提前 return 时不泄漏标志。
class RecordBypass {
  public:
    RecordBypass();
    ~RecordBypass();
    RecordBypass(const RecordBypass &) = delete;
    RecordBypass &operator=(const RecordBypass &) = delete;
};

// 当前线程是否处于旁路状态（测试可见）
bool in_record_bypass();

// 当前的"录制归属实例"（由 op::Op::InputSyncEnable 设置）。
// 后端产生的原语事件本身不知道属于哪个 op 实例（WinMouse 是被绑定的裸对象），
// 而主从同步必须知道"这条事件是谁产生的"才能查 master->slave。所以用
// **线程局部**记录当前线程正在为哪个实例工作：脚本是单线程顺序调用 API 的，
// 这个记录在调用期间始终指向正确实例；多实例并发时各线程各自独立。
long current_instance_id();
void set_current_instance_id(long id);

class InstanceInputBinder {
  public:
    explicit InstanceInputBinder(long instance_id);
    ~InstanceInputBinder();
    InstanceInputBinder(const InstanceInputBinder &) = delete;
    InstanceInputBinder &operator=(const InstanceInputBinder &) = delete;

  private:
    long previous_;
};

// 后端调用入口。owner 是 op 实例 id；bus 关闭时几乎零成本。
void emit_input_event(long owner_id, InputEvent event);

} // namespace op::input

#endif // OP_INPUT_INPUT_EVENT_BUS_H_
