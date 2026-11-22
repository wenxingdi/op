// 时间轴层：进程级输入事件总线（批 E 2026-11-22）。
//
// 标准库**成组前置**（本仓库硬约定，见 op/OpContext.h 顶部）：
// 2026-11-22 批 A 实测过，把 <atomic>/<chrono> 放在项目头之后会让 MSVC 的
// <chrono>/<filesystem> 解析坏（C2061 / C3861）。下面所有标准库头都在项目头之前。
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <Windows.h>

#include "InputEventBus.h"
#include "mouse/WinMouse.h"
#include "keyboard/KeyboardBackend.h"

#include "../base/AutomationModes.h"
#include "../base/Utils.h"

namespace op::input {

namespace {

thread_local int g_bypass_depth = 0;
thread_local long g_current_instance = -1;

using SteadyClock = std::chrono::steady_clock;

double elapsed_ms(SteadyClock::time_point from, SteadyClock::time_point to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
}

// 主从投递队列上限。输入是边沿事件，积压本身就是错的（迟到的点击会落到别的地方），
// 所以宁可丢也不阻塞主实例。256 条约等于鼠标高速移动半秒的量，够吸收偶发抖动。
constexpr size_t kSyncQueueLimit = 256;

// ---- 异步投递 worker ----
// 从实例可能是**另一个脚本线程**在跑回放/等待，所以投递不能同步做（会卡住主实例）。
// 单个后台线程按时间顺序把事件灌进从实例。
class SyncWorker {
  public:
    ~SyncWorker() {
        stop();
    }

    struct Job {
        long slave_id = 0;
        InputEvent event;
        std::function<long(const InputEvent &)> deliver;
    };

    // 投递成功返回 true；队列满返回 false（调用方计入 dropped）。
    bool post(Job job) {
        bool accepted = false;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (stopping_)
                return false;
            if (jobs_.size() < kSyncQueueLimit) {
                jobs_.push_back(std::move(job));
                accepted = true;
            }
        }
        if (!accepted)
            return false;
        ensure_thread();
        cond_.notify_one();
        return true;
    }

    // 阻塞等待队列排空（含正在执行的那一条）。测试与"停止前收尾"用。
    bool drain(int timeout_ms) {
        const auto deadline = SteadyClock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            {
                std::lock_guard<std::mutex> lk(mutex_);
                if (jobs_.empty() && !busy_)
                    return true;
            }
            if (SteadyClock::now() >= deadline)
                return false;
            ::Sleep(1);
        }
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (stopping_ && !thread_.joinable())
                return;
            stopping_ = true;
        }
        cond_.notify_all();
        std::lock_guard<std::mutex> thread_lk(thread_mutex_);
        if (thread_.joinable())
            thread_.join();
    }

  private:
    void ensure_thread() {
        std::lock_guard<std::mutex> thread_lk(thread_mutex_);
        if (thread_.joinable())
            return;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (stopping_)
                return;
        }
        thread_ = std::thread([this] { run(); });
    }

    void run() {
        // 从实例可能走 IN_WINDOWS/DX 路径发窗口消息：COM 初始化让 OLE 相关的
        // 消息派发保持一致行为（与 OpDraw 的 overlay 线程同一做法）。
        ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lk(mutex_);
                cond_.wait(lk, [this] { return stopping_ || !jobs_.empty(); });
                if (stopping_)
                    break;
                job = std::move(jobs_.front());
                jobs_.erase(jobs_.begin());
                busy_ = true;
            }
            try {
                if (job.deliver)
                    job.deliver(job.event);
            } catch (...) {
                setlog("input sync: slave delivery threw (slave=%ld type=%s)", job.slave_id,
                       input_event_type_name(job.event.type));
            }
            {
                std::lock_guard<std::mutex> lk(mutex_);
                busy_ = false;
            }
        }
        ::CoUninitialize();
    }

    mutable std::mutex mutex_;
    std::mutex thread_mutex_;
    std::condition_variable cond_;
    std::vector<Job> jobs_;
    std::thread thread_;
    bool stopping_ = false;
    bool busy_ = false;
};

std::wstring default_record_path() {
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    wchar_t name[80];
    _snwprintf_s(name, 80, L"input_record_%04d%02d%02d_%02d%02d%02d.jsonl", st.wYear, st.wMonth, st.wDay, st.wHour,
                 st.wMinute, st.wSecond);

    std::wstring dir;
    wchar_t buf[MAX_PATH] = {};
    if (::GetCurrentDirectoryW(MAX_PATH, buf) > 0)
        dir = buf;
    if (dir.empty())
        return name;
    if (dir.back() != L'\\' && dir.back() != L'/')
        dir.push_back(L'\\');
    return dir + name;
}

// 逐行读取 JSONL（UTF-8，无 BOM；BOM 也容错跳过）。
bool load_events(const std::wstring &path, std::vector<InputEvent> &out, std::wstring &error) {
    out.clear();
    error.clear();

    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"无法打开录制文件（错误码 " + std::to_wstring(::GetLastError()) + L"）";
        return false;
    }

    std::string bytes;
    char buf[8192];
    for (;;) {
        DWORD read = 0;
        const BOOL ok = ::ReadFile(file, buf, sizeof(buf), &read, nullptr);
        if (!ok) {
            error = L"读取录制文件失败（错误码 " + std::to_wstring(::GetLastError()) + L"）";
            ::CloseHandle(file);
            return false;
        }
        if (read == 0)
            break;
        bytes.append(buf, read);
    }
    ::CloseHandle(file);

    if (bytes.empty())
        return true;

    const int wide_len = ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (wide_len <= 0)
        return true;
    std::wstring text(static_cast<size_t>(wide_len), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), wide_len);
    if (!text.empty() && text[0] == 0xFEFF)
        text.erase(text.begin());

    size_t begin = 0;
    long bad_lines = 0;
    while (begin <= text.size()) {
        size_t end = text.find(L'\n', begin);
        if (end == std::wstring::npos)
            end = text.size();
        std::wstring line = text.substr(begin, end - begin);
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ' || line.back() == L'\t'))
            line.pop_back();
        if (line.find_first_not_of(L" \t") != std::wstring::npos) {
            InputEvent ev;
            if (parse_input_event(line, ev))
                out.push_back(ev);
            else
                ++bad_lines;
        }
        if (end >= text.size())
            break;
        begin = end + 1;
    }
    if (bad_lines > 0) {
        // 坏行不致命（人工编辑过的文件常见），但必须报出来——静默跳过等于
        // 回放出"少了几个点击"的诡异现象。
        error = L"跳过 " + std::to_wstring(bad_lines) + L" 行无法解析的事件";
        setlog("input playback: skipped %ld malformed line(s) in %s", bad_lines, path.c_str());
    }
    return true;
}

// 把一条事件投到某个输入后端。移动统一走 WinMouse::MoveTo（绝对客户区坐标），
// 按键/滚轮按 WIN 后端语义发。
//
// 为什么回放直接调后端而不走 op::Op：
//   op::Op 层有防抖/拟人/随机偏移（MoveToEx 会加随机落点），回放必须**确定性**；
//   而 LeftClick 等合成方法会把"按一下"拆成 down+up，录制里已经有这两条，
//   再走合成就重复了。
//
// 已知限制（如实记录）：DX 绑定模式下 WinMouse::MoveTo 返回 0，因为 DX 后端的
// 移动只能在 DxMouse 上发。回放这里用 WIN 后端对象是**有意**的：DX/GL 注入需要
// 目标进程里的载入器，而录制文件跨进程通用；用 WIN 语义回放至少能保证 IN_NORMAL
// 与 IN_WINDOWS 两种模式正确。DX 模式下的回放支持见五模块总结报告的"未做"一节。
long replay_with_backends(WinMouse *mouse, KeyboardBackend *keyboard, const InputEvent &ev) {
    if (!mouse || !keyboard)
        return 0;

    switch (ev.type) {
    case InputEventType::MouseMove:
        return mouse->MoveTo(static_cast<int>(ev.x), static_cast<int>(ev.y));
    case InputEventType::MouseWheel:
        if (ev.flags == MOUSEEVENTF_HWHEEL)
            return mouse->HWheel(static_cast<int>(ev.wheel));
        return mouse->Wheel(static_cast<int>(ev.wheel));
    case InputEventType::MouseButton: {
        // flags 与 Win32 原语一一对应，见 InputEvent.h 顶部注释
        switch (ev.flags) {
        case MOUSEEVENTF_LEFTDOWN:
            return mouse->LeftDown();
        case MOUSEEVENTF_LEFTUP:
            return mouse->LeftUp();
        case MOUSEEVENTF_RIGHTDOWN:
            return mouse->RightDown();
        case MOUSEEVENTF_RIGHTUP:
            return mouse->RightUp();
        case MOUSEEVENTF_MIDDLEDOWN:
            return mouse->MiddleDown();
        case MOUSEEVENTF_MIDDLEUP:
            return mouse->MiddleUp();
        case MOUSEEVENTF_XDOWN:
            return ev.wheel == XBUTTON1 ? mouse->XButton1Down() : mouse->XButton2Down();
        case MOUSEEVENTF_XUP:
            return ev.wheel == XBUTTON1 ? mouse->XButton1Up() : mouse->XButton2Up();
        default:
            return 0;
        }
    }
    case InputEventType::Key:
        return (ev.flags & KEYEVENTF_KEYUP) ? keyboard->KeyUp(ev.vk) : keyboard->KeyDown(ev.vk);
    case InputEventType::KeyChar:
        // x 字段复用为字符码（见 InputEvent.h）
        return keyboard->InputChar(static_cast<wchar_t>(ev.x));
    }
    return 0;
}

} // namespace

// ---- 实例上下文 ----

long current_instance_id() {
    return g_current_instance;
}

void set_current_instance_id(long id) {
    g_current_instance = id;
}

InstanceInputBinder::InstanceInputBinder(long instance_id) : previous_(g_current_instance) {
    if (instance_id >= 0)
        g_current_instance = instance_id;
}

InstanceInputBinder::~InstanceInputBinder() {
    g_current_instance = previous_;
}

// ---- RecordBypass ----

RecordBypass::RecordBypass() {
    ++g_bypass_depth;
}

RecordBypass::~RecordBypass() {
    if (g_bypass_depth > 0)
        --g_bypass_depth;
}

bool in_record_bypass() {
    return g_bypass_depth > 0;
}

// ---- Impl ----

struct InputEventBus::Impl {
    std::atomic<bool> enabled{false};

    std::mutex monitor_mutex;
    InputMonitorCallback monitor = nullptr;
    void *monitor_user = nullptr;
    std::atomic<long> monitor_errors{0};

    // 录制
    mutable std::mutex record_mutex;
    FILE *record_file = nullptr;
    std::wstring record_path;
    long record_count = 0;
    SteadyClock::time_point record_origin;

    // 回放
    mutable std::mutex playback_mutex;
    std::vector<InputEvent> playback_events;
    std::atomic<bool> playback_stop_request{false};
    std::atomic<bool> playback_running{false};
    std::atomic<long> playback_counter{0}; // 已重放条数（跨 loop 累计），诊断用

    // 主从
    mutable std::mutex sync_mutex;
    std::map<long, long> link; // master -> slave
    std::atomic<long> sync_dropped{0};
    std::unique_ptr<SyncWorker> worker;

    // 实例 -> 输入后端。回放与主从投递都从这里找"往哪儿发"。
    // 指针不拥有后端（所有权在 BindingSession 的 op::Op 实例里），注销在
    // op::Op::InputSyncEnable / 实例销毁时发生。
    mutable std::mutex backend_mutex;
    std::map<long, std::pair<WinMouse *, KeyboardBackend *>> backends;
    // 最近一次注册的实例：owner_id<0（"不指定实例"）时用它，语义是"当前这个实例"。
    long last_registered = -1;

    SyncWorker &ensure_worker() {
        std::lock_guard<std::mutex> lk(sync_mutex);
        if (!worker)
            worker = std::make_unique<SyncWorker>();
        return *worker;
    }
};

InputEventBus::InputEventBus() : impl_(new Impl()) {
}

InputEventBus::~InputEventBus() {
    {
        std::lock_guard<std::mutex> lk(impl_->record_mutex);
        if (impl_->record_file) {
            std::fclose(impl_->record_file);
            impl_->record_file = nullptr;
        }
    }
    {
        std::lock_guard<std::mutex> lk(impl_->sync_mutex);
        if (impl_->worker)
            impl_->worker->stop();
    }
    delete impl_;
}

InputEventBus &InputEventBus::instance() {
    return shared_input_event_bus();
}

// 进程唯一的输入事件总线。
//
// ## 为什么不能用函数内静态变量（2026-11-22 实测踩坑）
//
// `static InputEventBus bus;` 是"每个模块一份"。测试进程同时载入 op_com.dll 与
// op_c_api_x64.dll，而 op_test.exe 又直接编进了 WinMouse.cpp / InputEventBus.cpp，
// 于是进程里出现了三份 instance()：录制钩子跑在 exe 那份上，OpInputPlayback 通过
// op_c_api.dll 操作的是另一份 → 实测表现为"START 看得见事件、STOP 永远看不见，
// 2s 的间隔照走完"。发布环境里这是同一个错（COM 入口与 C API 入口各录各的）。
//
// ## 修法：单例的**状态**放进进程级命名共享内存
//
// 三个 DLL/exe 都是 /MT（各自一份静态 CRT），跨模块共享状态只能靠内核对象。
// 这里用 `CreateFileMapping`（名字带 PID，避免跨进程串用）+ `MapViewOfFile`，
// 首次创建者做 placement-new，后来的模块直接复用同一块内存。
// `InputEventBus` 本身只持有指向该块的指针，所以"每模块一个 InputEventBus 对象"
// 无害 —— 它们操作的是同一份状态。
//
// 验证方式见 tests/input_timeline_batch_test.cpp 的 PlaybackStopInterruptsLongWait：
// 若退化成每模块一份，那条用例会因为 STOP 看不见而失败（本次就是这么抓到的）。
namespace {

// 共享块布局：一个哨兵 + 指向真实 Impl 的指针。
// 指针指向**首个模块内**的堆内存，其生命周期跟随该模块 —— 对 op_com.dll / 
// op_c_api_x64.dll 这两个与进程同寿命的模块足够；
// 若将来出现会卸载的模块持有它，需要改成"首个模块永不卸载"的显式登记。
struct SharedBusSlot {
    unsigned long magic;
    void *impl;
};

constexpr unsigned long kBusMagic = 0x4F504255u; // 'OPBU'

SharedBusSlot *shared_slot() {
    static SharedBusSlot *cached = []() -> SharedBusSlot * {
        wchar_t name[128];
        _snwprintf_s(name, 128, L"Local\\op_input_event_bus_slot_%lu", ::GetCurrentProcessId());
        HANDLE map = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SharedBusSlot), name);
        if (!map)
            return nullptr;
        auto *view = static_cast<SharedBusSlot *>(::MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedBusSlot)));
        if (!view) {
            ::CloseHandle(map);
            return nullptr;
        }
        // 故意不 CloseHandle(map)：句柄随进程结束回收，关掉会让名字对象消失。
        return view;
    }();
    return cached;
}

} // namespace

InputEventBus &shared_input_event_bus() {
    SharedBusSlot *slot = shared_slot();
    if (!slot) {
        // 命名共享内存建不出来（极端受限环境）：退化为模块内单例。
        // 这时是"各模块各录各的"——比崩溃好，但必须在日志里明说。
        static InputEventBus fallback;
        static std::atomic<bool> warned{false};
        bool expect = false;
        if (warned.compare_exchange_strong(expect, true)) {
            setlog("input bus: shared slot unavailable, falling back to per-module singleton "
                   "(recording/monitoring will NOT see events produced through another DLL)");
        }
        return fallback;
    }

    InputEventBus *bus = static_cast<InputEventBus *>(slot->impl);
    if (bus)
        return *bus;

    // 首个到达的模块创建实例。用 InterlockedCompareExchange 保证只有一个赢家。
    static InputEventBus owner;
    void *expected = nullptr;
    if (::InterlockedCompareExchangePointer(&slot->impl, static_cast<void *>(&owner), expected) == nullptr) {
        slot->magic = kBusMagic;
        return owner;
    }
    return *static_cast<InputEventBus *>(slot->impl);
}

void InputEventBus::set_enabled(bool on) {
    impl_->enabled.store(on, std::memory_order_release);
}

bool InputEventBus::enabled() const {
    return impl_->enabled.load(std::memory_order_acquire);
}

void InputEventBus::set_monitor(InputMonitorCallback cb, void *user) {
    std::lock_guard<std::mutex> lk(impl_->monitor_mutex);
    impl_->monitor = cb;
    impl_->monitor_user = user;
}

long InputEventBus::record_start(const std::wstring &path, long *ret) {
    std::lock_guard<std::mutex> lk(impl_->record_mutex);
    if (impl_->record_file) {
        // 已在录制：拒绝，而不是悄悄截断前一段。脚本误写成两次 Start 时，
        // 静默重开会让"为什么只录到后半段"成为极难定位的问题。
        setlog("input record: already recording to %s", impl_->record_path.c_str());
        if (ret)
            *ret = 0;
        return 0;
    }

    const std::wstring target = path.empty() ? default_record_path() : path;

    FILE *file = _wfopen(target.c_str(), L"wb");
    if (!file) {
        setlog("input record: cannot open %s for writing", target.c_str());
        if (ret)
            *ret = 0;
        return 0;
    }

    impl_->record_file = file;
    impl_->record_path = target;
    impl_->record_count = 0;
    impl_->record_origin = SteadyClock::now();
    impl_->enabled.store(true, std::memory_order_release);
    setlog("input record: started -> %s", target.c_str());
    if (ret)
        *ret = 1;
    return 1;
}

long InputEventBus::record_stop(std::wstring &out_path) {
    std::lock_guard<std::mutex> lk(impl_->record_mutex);
    out_path = impl_->record_path;
    if (!impl_->record_file)
        return 0;

    std::fflush(impl_->record_file);
    std::fclose(impl_->record_file);
    impl_->record_file = nullptr;
    const long count = impl_->record_count;
    setlog("input record: stopped, %ld event(s) -> %s", count, impl_->record_path.c_str());
    return count;
}

bool InputEventBus::recording() const {
    std::lock_guard<std::mutex> lk(impl_->record_mutex);
    return impl_->record_file != nullptr;
}

std::wstring InputEventBus::record_path() const {
    std::lock_guard<std::mutex> lk(impl_->record_mutex);
    return impl_->record_path;
}

long InputEventBus::record_count() const {
    std::lock_guard<std::mutex> lk(impl_->record_mutex);
    return impl_->record_count;
}

long InputEventBus::playback_load(const std::wstring &path, long *ret) {
    std::vector<InputEvent> events;
    std::wstring error;
    if (!load_events(path, events, error)) {
        setlog("input playback: load failed: %s", error.c_str());
        if (ret)
            *ret = 0;
        return -1;
    }

    std::lock_guard<std::mutex> lk(impl_->playback_mutex);
    impl_->playback_events = std::move(events);
    impl_->playback_counter.store(0, std::memory_order_relaxed);
    // LOAD 是"重新开始一次回放"的边界：这里清掉上一轮 STOP 的残留置位。
    // 放在 LOAD 而不是 START，就是为了避开 START/STOP 抢跑的那个坑。
    impl_->playback_stop_request.store(false, std::memory_order_release);
    if (ret)
        *ret = static_cast<long>(impl_->playback_events.size());
    return static_cast<long>(impl_->playback_events.size());
}

long InputEventBus::playback_loaded_count() const {
    std::lock_guard<std::mutex> lk(impl_->playback_mutex);
    return static_cast<long>(impl_->playback_events.size());
}

bool InputEventBus::playback_running() const {
    return impl_->playback_running.load(std::memory_order_acquire);
}

void InputEventBus::playback_stop() {
    // 只置标志，不在这里 join：Stop 可能由 Monitor 回调里（同一线程）调用。
    impl_->playback_stop_request.store(true, std::memory_order_release);
}

bool InputEventBus::playback_stop_requested() const {
    return impl_->playback_stop_request.load(std::memory_order_acquire);
}

const void *InputEventBus::identity() const {
    return impl_;
}

long InputEventBus::playback_run(double speed, long loop_count, long owner_id, long *ret) {
    if (ret)
        *ret = 0;

    // 同一时刻只允许一个回放在跑：两个回放线程同时往同一实例打输入，
    // 事件序列会交错成"谁都不认识"的东西。
    bool expected = false;
    if (!impl_->playback_running.compare_exchange_strong(expected, true)) {
        setlog("input playback: already running");
        return 0;
    }

    std::vector<InputEvent> events;
    {
        std::lock_guard<std::mutex> lk(impl_->playback_mutex);
        events = impl_->playback_events;
    }
    if (events.empty()) {
        impl_->playback_running.store(false);
        return 0; // 未 Load（或空文件）：返回 0，调用方可从 ret=0 判出没东西可放
    }

    if (!(speed > 0.0))
        speed = 1.0;
    const long loops = loop_count <= 0 ? 1 : loop_count;

    // ⚠ 这里**不能**清零 playback_stop_request。
    // 2026-11-22 实测踩到：START 与 STOP 抢跑（STOP 已落地、START 才进到这里）时，
    // 清零会把 STOP 抹掉，于是 2s 的间隔照走完 —— 表现为"STOP 没生效"。
    // 正确顺序是：STOP 置位 → START 看到置位立刻收工。
    // 上一轮回放结束后的残留置位由 InputPlayback(STOP) 之后的 START 自行消化：
    // 首次检查就会命中并直接返回 0，语义上等价于"已被停止"，不会静默重放。
    //
    // 回放线程自身产生的事件不进录制/监听/主从：否则边放边录会自激放大。
    RecordBypass guard;

    // owner_id < 0 表示"投给当前实例"：在**本线程**上固化一次，不要等到每条事件
    // 再去查线程局部（后台投递线程里那个值是 -1，会退化成"最近注册的实例"）。
    long target = owner_id;
    if (target < 0)
        target = current_instance_id();
    if (target < 0)
        target = impl_->last_registered;

    long played = 0;
    bool aborted = false;
    for (long loop = 0; loop < loops && !aborted; ++loop) {
        const auto origin = SteadyClock::now();
        for (size_t i = 0; i < events.size(); ++i) {            if (impl_->playback_stop_request.load(std::memory_order_acquire)) {
                aborted = true;
                break;
            }
            const InputEvent &ev = events[i];
            // 时间轴按 t 等比缩放：目标时刻 = t / speed。
            // 用"从回放起点累加的绝对时间"而不是"相邻差值累加"，避免长录音里
            // 每步的调度误差累积成秒级漂移。
            const double target_ms = ev.t / speed;
            for (;;) {
                const double now_ms = elapsed_ms(origin, SteadyClock::now());
                const double remain = target_ms - now_ms;
                if (remain <= 0.0)
                    break;
                // 分片睡，保证 Stop 在 5ms 内生效（Stop 不该等一整段长间隔）
                const DWORD slice = static_cast<DWORD>(remain > 5.0 ? 5 : (remain < 1.0 ? 1.0 : remain));
                ::Sleep(slice);
                if (impl_->playback_stop_request.load(std::memory_order_acquire)) {
                    aborted = true;
                    break;
                }
            }
            if (aborted)
                break;

            replay_on_instance(target, ev);
            ++played;
            impl_->playback_counter.fetch_add(1, std::memory_order_relaxed);
        }
    }

    impl_->playback_running.store(false);
    if (ret)
        *ret = played;
    setlog("input playback: %ld event(s) replayed (speed=%.3f loop=%ld, instance=%ld)%s", played, speed, loops, target,
           aborted ? " [stopped]" : "");
    return played;
}

std::wstring InputEventBus::sync_status(long master_id, long slave_id) const {
    long as_master_of = -1;
    long as_slave_of = -1;
    {
        std::lock_guard<std::mutex> lk(impl_->sync_mutex);
        const auto it = impl_->link.find(master_id);
        if (it != impl_->link.end())
            as_master_of = it->second;
        for (const auto &kv : impl_->link) {
            if (kv.second == slave_id) {
                as_slave_of = kv.first;
                break;
            }
        }
    }

    wchar_t buf[256];
    _snwprintf_s(buf, 256,
                 L"{\"ok\":1,\"enabled\":%d,\"master\":%ld,\"slave\":%ld,\"as_master_of\":%ld,"
                 L"\"as_slave_of\":%ld,\"async_dropped\":%ld,\"queue\":%ld}",
                 as_master_of >= 0 ? 1 : 0, master_id, as_master_of, as_master_of, as_slave_of,
                 impl_->sync_dropped.load(std::memory_order_relaxed),
                 impl_->worker ? impl_->worker->drain(0) ? 0L : 1L : 0L);
    return buf;
}

long InputEventBus::sync_link(long master_id, long slave_id, long enable, long *ret) {
    if (enable == 0) {
        std::lock_guard<std::mutex> lk(impl_->sync_mutex);
        const auto it = impl_->link.find(master_id);
        // 关闭时同时校验从实例：避免"关错了另一条链路"却返回成功
        if (it == impl_->link.end() || (slave_id != 0 && it->second != slave_id)) {
            if (ret)
                *ret = 0;
            return 0;
        }
        impl_->link.erase(it);
        if (ret)
            *ret = 1;
        return 1;
    }

    if (master_id == slave_id) {
        // 自己投给自己会形成自我放大（一条输入变两条，再变四条）
        setlog("input sync: refusing to link instance %ld to itself", master_id);
        if (ret)
            *ret = 0;
        return 0;
    }

    {
        std::lock_guard<std::mutex> lk(impl_->sync_mutex);
        // 一个实例只能有一个从属目标；重新 Link 视为改绑（便于脚本热改配置）
        impl_->link[master_id] = slave_id;
    }
    ensure_worker_running();
    impl_->enabled.store(true, std::memory_order_release);
    setlog("input sync: master %ld -> slave %ld", master_id, slave_id);
    if (ret)
        *ret = 1;
    return 1;
}

long InputEventBus::sync_unlink_all(long *ret) {
    SyncWorker *worker = nullptr;
    long removed = 0;
    {
        std::lock_guard<std::mutex> lk(impl_->sync_mutex);
        removed = static_cast<long>(impl_->link.size());
        impl_->link.clear();
        worker = impl_->worker.get();
    }
    if (worker) {
        // 收尾：先把已入队的事件放完，再停线程——"刚录完就切走"不该丢尾巴
        worker->drain(2000);
        worker->stop();
    }
    if (ret)
        *ret = removed;
    return removed;
}

void InputEventBus::ensure_worker_running() {
    impl_->ensure_worker();
}

void InputEventBus::register_backends(long instance_id, WinMouse *mouse, KeyboardBackend *keyboard) {
    if (instance_id < 0)
        return;
    std::lock_guard<std::mutex> lk(impl_->backend_mutex);
    impl_->backends[instance_id] = std::make_pair(mouse, keyboard);
    impl_->last_registered = instance_id;
}

void InputEventBus::unregister_backends(long instance_id) {
    std::lock_guard<std::mutex> lk(impl_->backend_mutex);
    impl_->backends.erase(instance_id);
    if (impl_->last_registered == instance_id)
        impl_->last_registered = -1;
}

long InputEventBus::backends_registered() const {
    std::lock_guard<std::mutex> lk(impl_->backend_mutex);
    return static_cast<long>(impl_->backends.size());
}

long InputEventBus::replay_on_instance(long owner_id, const InputEvent &event) {
    WinMouse *mouse = nullptr;
    KeyboardBackend *keyboard = nullptr;
    long resolved = -1;
    {
        std::lock_guard<std::mutex> lk(impl_->backend_mutex);
        long want = owner_id;
        if (want < 0) {
            // "-1 = 任意一个"：优先用当前线程正在服务的实例（脚本常见形态就是
            // "自己录、自己放"），否则退到最近注册的那个。
            want = impl_->last_registered;
            const auto self = impl_->backends.find(current_instance_id());
            if (current_instance_id() >= 0 && self != impl_->backends.end())
                want = current_instance_id();
        }
        const auto it = impl_->backends.find(want);
        if (it != impl_->backends.end()) {
            mouse = it->second.first;
            keyboard = it->second.second;
            resolved = it->first;
        }
    }
    if (resolved < 0) {
        // 静默丢弃会让"回放跑了但画面没动"变成玄学，必须留痕（只打一次太多行，
        // 这里限流：每秒最多一条）。
        static std::atomic<long> last_warn{0};
        const long now_s = static_cast<long>(::GetTickCount64() / 1000);
        long expect = last_warn.load(std::memory_order_relaxed);
        if (now_s > expect && last_warn.compare_exchange_strong(expect, now_s))
            setlog("input replay: no input backend registered for instance %ld (idle drop)", owner_id);
        return -1;
    }
    return replay_with_backends(mouse, keyboard, event) ? resolved : -1;
}

// ---- 事件分发 ----

void InputEventBus::emit(long owner_id, InputEvent event) {
    // 最外层快速退出：绝大多数脚本没开采集，热路径只花一次原子读。
    if (!impl_->enabled.load(std::memory_order_acquire))
        return;

    // owner_id<0：后端只知道"当前线程在服务哪个实例"（见 InstanceInputBinder）
    const long owner = owner_id >= 0 ? owner_id : current_instance_id();

    const auto now = SteadyClock::now();

    // 1) 录制（旁路判定：回放/回调里产生的事件不再录，否则边放边录会自激）
    if (!in_record_bypass()) {
        std::lock_guard<std::mutex> lk(impl_->record_mutex);
        if (impl_->record_file) {
            event.t = elapsed_ms(impl_->record_origin, now);
            if (event.t < 0.0)
                event.t = 0.0;
            const std::wstring line = event.to_jsonl();
            const std::string utf8 = [](const std::wstring &text) {
                std::string out;
                const int len = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                                      nullptr, 0, nullptr, nullptr);
                if (len > 0) {
                    out.resize(static_cast<size_t>(len));
                    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), len,
                                          nullptr, nullptr);
                }
                return out;
            }(line);
            // 逐行 fflush：录制进程可能被脚本强杀，缓冲在崩溃时全丢。
            // 输入量级（每秒几百条）远没到需要批量写的程度。
            if (std::fwrite(utf8.data(), 1, utf8.size(), impl_->record_file) == utf8.size()) {
                std::fputc('\n', impl_->record_file);
                std::fflush(impl_->record_file);
                ++impl_->record_count;
            }
        }
    }

    // 2) 监听回调（调用方线程同步触发，与 OLA InputMonitorCallback 同构）
    InputMonitorCallback cb = nullptr;
    void *user = nullptr;
    {
        std::lock_guard<std::mutex> lk(impl_->monitor_mutex);
        cb = impl_->monitor;
        user = impl_->monitor_user;
    }
    if (cb && !in_record_bypass()) {
        // 回调里再调输入 API 会重入 emit：旁路护栏把"回调触发的输入"挡在
        // 录制/监听/投递之外，否则一次回调能放大成指数级递归输入。
        RecordBypass guard;
        try {
            cb(&event, user);
        } catch (...) {
            impl_->monitor_errors.fetch_add(1, std::memory_order_relaxed);
            setlog("input monitor: callback threw");
        }
    }

    // 3) 主从投递（异步、可丢弃）
    if (in_record_bypass())
        return;

    long slave = -1;
    SyncWorker *worker = nullptr;
    {
        std::lock_guard<std::mutex> lk(impl_->sync_mutex);
        const auto it = impl_->link.find(owner);
        if (it != impl_->link.end()) {
            slave = it->second;
            worker = impl_->worker.get();
        }
    }
    if (slave < 0 || !worker)
        return;

    SyncWorker::Job job;
    job.slave_id = slave;
    job.event = event;
    // 投递线程里不能读"当前实例"的线程局部（那里是 -1），所以 slave 用**显式 id**。
    // 注意不能用 this 捕获：worker 的生命周期独立于本次调用。
    InputEventBus *self = this;
    job.deliver = [self, slave](const InputEvent &ev) -> long { return self->replay_on_instance(slave, ev); };
    if (!worker->post(std::move(job)))
        impl_->sync_dropped.fetch_add(1, std::memory_order_relaxed);
}

void emit_input_event(long owner_id, InputEvent event) {
    InputEventBus::instance().emit(owner_id, std::move(event));
}

} // namespace op::input
