// 时间轴层（批 E 2026-11-22）：录制 / 回放 / 监听 / 主从同步。
//
// 设计主线见 input/InputEventBus.h 顶部：
//   * 录制钩子挂在**每个输入原语 override 的出口**（LeftDown/LeftUp/MoveTo/KeyDown/…），
//     不挂在 op::Op 的合成方法上——合成方法会把"一次点击"藏成一个调用，回放不出来。
//   * 回放走"事件 → 后端原语"的直投，不经过 op 层的拟人化/随机落点，保证确定性。
//
// 本文件只做三件事：参数校验、把实例 id 与后端注册进总线、拼返回 JSON。
//
// ## 坐标系与绑定（最容易被误用的地方）
//   录制只存**客户区坐标**（与 Mouse 各 API 同口径）。所以"录的时候绑了什么窗口，
//   放的时候就得绑同一个窗口"，否则坐标会落到别处。这条不做静默兜底：
//   InputPlayback(LOAD) 的返回里带 "bound":0/1，脚本能自查。
//
// ## 实例归属（thread_local）
//   后端对象（WinMouse 等）不知道自己属于哪个 op 实例，而主从同步必须知道
//   "这条事件是谁发的"。用线程局部记录当前线程正在服务哪个实例：
//   脚本是单线程顺序调用 API 的，调用期间这个值始终正确。
//
// 标准库成组前置（仓库硬约定，见 op/OpContext.h 顶部）。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "OpContext.h"
#include "OpResult.h"

#include "base/Utils.h"
#include "input/InputEventBus.h"

#include <libop.h>

#include <Windows.h>

namespace {

using namespace op::input;

// 把实例 id 写进线程局部：后端产生事件时靠它认领"这是哪个实例的输入"。
// RAII 保证异常/提前 return 时恢复上一个值（嵌套调用时是栈式的）。
class InstanceScope {
  public:
    explicit InstanceScope(long id) : binder_(id) {
    }

  private:
    InstanceInputBinder binder_;
};

std::wstring num(long v) {
    return std::to_wstring(v);
}

// 路径要转义反斜杠与引号：Windows 路径必然带 '\'，不转义会让外部 JSON 解析器
// 把 \A 之类当成非法转义而整段解析失败。
std::wstring json_escape(const std::wstring &text) {
    std::wstring out;
    out.reserve(text.size() + 8);
    for (wchar_t ch : text) {
        if (ch == L'\\' || ch == L'"')
            out.push_back(L'\\');
        out.push_back(ch);
    }
    return out;
}

std::wstring playback_load_json(long loaded, long bound, const std::wstring &path, const std::wstring &error) {
    std::wstring out = L"{\"ok\":" + num(error.empty() ? 1 : 0) + L",\"events\":" + num(loaded) +
                       L",\"bound\":" + num(bound) + L",\"path\":\"" + json_escape(path) + L"\"";
    if (!error.empty())
        out += L",\"error\":\"" + json_escape(error) + L"\"";
    out += L"}";
    return out;
}

// 总线内部事件 -> 公开视图（回调桥接用）。
op::op_input_event_view to_view(const InputEvent &ev) {
    op::op_input_event_view view{};
    view.t = ev.t;
    view.type = static_cast<int>(ev.type);
    view.vk = ev.vk;
    view.x = ev.x;
    view.y = ev.y;
    view.wheel = ev.wheel;
    view.flags = ev.flags;
    return view;
}

// 公开回调签名 + 用户指针。总线只支持一个函数指针 + void*，所以把二者打包
// 成一个小结构体作为 user 传进去（生命周期由本文件的静态槽管理）。
struct MonitorBridge {
    op::Op::InputMonitorCallback callback = nullptr;
    void *user = nullptr;
    std::mutex mutex;
};

MonitorBridge &monitor_bridge() {
    static MonitorBridge bridge;
    return bridge;
}

// 总线回调（签名是内部 InputEvent）。桥接到公开的 op_input_event_view。
void monitor_trampoline(const InputEvent *event, void *user) {
    auto *bridge = static_cast<MonitorBridge *>(user);
    if (!bridge || !event)
        return;
    op::Op::InputMonitorCallback cb = nullptr;
    void *user_data = nullptr;
    {
        std::lock_guard<std::mutex> lk(bridge->mutex);
        cb = bridge->callback;
        user_data = bridge->user;
    }
    if (!cb)
        return;
    const op::op_input_event_view view = to_view(*event);
    cb(&view, user_data);
}

} // namespace

void op::Op::InputSyncEnable(long enable, long *ret) {
    InstanceScope scope(m_context->id);
    auto &bus = InputEventBus::instance();

    if (enable == 0) {
        // 关闭采集时连同录制一起收尾：否则文件句柄一直开着，用户下次 Start
        // 会被"已在录制"拒绝，而他不记得自己还开着。
        if (bus.recording()) {
            std::wstring path;
            const long written = bus.record_stop(path);
            setlog("input sync: disabled while recording, flushed %ld event(s) to %s", written, path.c_str());
        }
        bus.set_enabled(false);
        internal::set_result(ret, 1L);
        return;
    }

    // 注册本实例的输入后端，回放/主从才知道"往哪儿发"。
    bus.register_backends(m_context->id, m_context->bkproc._mouse.get(), m_context->bkproc._keyboard.get());
    bus.set_enabled(true);
    setlog("input sync: enabled for instance %d (backends registered: %ld)", m_context->id,
           bus.backends_registered());
    internal::set_result(ret, 1L);
}

void op::Op::SetInputMonitor(op::Op::InputMonitorCallback callback, void *user, long *ret) {
    InstanceScope scope(m_context->id);
    auto &bus = InputEventBus::instance();

    MonitorBridge &bridge = monitor_bridge();
    {
        std::lock_guard<std::mutex> lk(bridge.mutex);
        bridge.callback = callback;
        bridge.user = user;
    }
    bus.set_monitor(callback ? &monitor_trampoline : nullptr, callback ? &bridge : nullptr);

    if (callback) {
        bus.register_backends(m_context->id, m_context->bkproc._mouse.get(), m_context->bkproc._keyboard.get());
        bus.set_enabled(true);
    }
    internal::set_result(ret, 1L);
}

void op::Op::InputRecord(long action, const wchar_t *path, std::wstring &retpath, long *ret) {
    InstanceScope scope(m_context->id);
    auto &bus = InputEventBus::instance();
    bus.register_backends(m_context->id, m_context->bkproc._mouse.get(), m_context->bkproc._keyboard.get());
    // 不论 Start 还是 Stop 都确保采集开着：Stop 之前若被 InputSyncEnable(0) 关了，
    // 这段录制里的事件其实一条都没进来。显式打开，让"录到 0 条"只可能是真没输入。
    bus.set_enabled(true);

    if (action == 0) {
        // START
        internal::set_result(ret, bus.record_start(path ? path : L"", nullptr));
        retpath = bus.record_path();
        return;
    }
    if (action == 1) {
        // STOP
        std::wstring stopped;
        const long written = bus.record_stop(stopped);
        retpath = stopped;
        internal::set_result(ret, written); // ret = 实际写入的事件条数
        return;
    }
    setlog("input record: unknown action %ld (expect 0=START 1=STOP)", action);
    retpath.clear();
    internal::set_result(ret, 0L);
}

void op::Op::InputRecordGetPath(std::wstring &retpath) {
    retpath = InputEventBus::instance().record_path();
}

long op::Op::InputPlayback(long action, const wchar_t *path, double speed, long loop_count, std::wstring &retjson,
                           long *ret) {
    InstanceScope scope(m_context->id);
    auto &bus = InputEventBus::instance();

    switch (action) {
    case 0: { // LOAD
        const long loaded = bus.playback_load(path ? path : L"", nullptr);
        const long bound = m_context->bkproc.IsBind() ? 1 : 0;
        if (loaded < 0) {
            internal::set_result(ret, 0L);
            retjson = playback_load_json(0, bound, path ? path : L"", L"无法打开或读取录制文件");
            return 0;
        }
        // Load 成功后立刻注册后端：脚本常见顺序是 load -> start，
        // 中间不会再调 InputSyncEnable。
        bus.register_backends(m_context->id, m_context->bkproc._mouse.get(), m_context->bkproc._keyboard.get());
        internal::set_result(ret, loaded);
        retjson = playback_load_json(loaded, bound, path ? path : L"", L"");
        // 没绑定窗口也能放（坐标落到整个屏幕），但这是**不同语义**，必须让调用方看见。
        if (!bound)
            setlog("input playback: no window bound, coordinates replay as screen coordinates");
        return loaded;
    }
    case 1: { // START
        const long played = bus.playback_run(speed, loop_count, m_context->id, nullptr);
        // ret 的语义：1 = 放完了；0 = 没东西可放（未 LOAD / 空文件 / 已被 STOP）
        internal::set_result(ret, played > 0 ? 1L : 0L);
        retjson = L"{\"ok\":1,\"played\":" + num(played) + L"}";
        return played > 0 ? 1 : 0;
    }
    case 2: { // STOP
        bus.playback_stop();
        internal::set_result(ret, 1L);
        retjson = L"{\"ok\":1,\"stopped\":1}";
        return 1;
    }
    case 3: { // FREE
        bus.playback_stop();
        const long loaded = bus.playback_loaded_count();
        internal::set_result(ret, 1L);
        retjson = L"{\"ok\":1,\"freed\":" + num(loaded) + L"}";
        return 1;
    }
    default:
        setlog("input playback: unknown action %ld (expect 0=LOAD 1=START 2=STOP 3=FREE)", action);
        internal::set_result(ret, 0L);
        retjson = L"{\"ok\":0,\"error\":\"unknown action\"}";
        return 0;
    }
}

void op::Op::InputSyncLink(long master, long slave, long enable, long *ret) {
    InstanceScope scope(m_context->id);
    auto &bus = InputEventBus::instance();
    // 只登记**本实例**的后端。master/slave 指向的其它实例必须由它们自己调任意
    // 时间轴 API（InputSyncEnable 等）登记各自的后端——这里拿本实例的后端指针
    // 去登记对方 id 会覆盖对方正确的登记，投递会打到错误的窗口（2026-11-25 修）。
    bus.register_backends(m_context->id, m_context->bkproc._mouse.get(), m_context->bkproc._keyboard.get());
    internal::set_result(ret, bus.sync_link(master, slave, enable, nullptr));
}

void op::Op::InputSyncStatus(std::wstring &retjson) {
    InstanceScope scope(m_context->id);
    auto &bus = InputEventBus::instance();
    bus.register_backends(m_context->id, m_context->bkproc._mouse.get(), m_context->bkproc._keyboard.get());
    retjson = bus.sync_status(m_context->id, m_context->id);
}
