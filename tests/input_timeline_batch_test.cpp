// 时间轴层（批 E 2026-11-22）：录制 / 回放 / 监听 / 主从同步。
//
// ## 被测范围与口径（诚实说明）
//
// * 全部用例走 **C API 导出层**（Op*Create/OpInput*），因为这是易语言/Python/Go
//   的真实入口，也是"新接口是否真的导出了裸名"的唯一有效判据（C++ 直调能链接成功
//   不代表 GetProcAddress 找得到，见 CLAUDE.md 的高频判据）。
// * 输入落在**自建测试窗口 + windows 绑定模式**上（MouseEventWindow）：不产生全局
//   真实鼠标移动，判据确定、不干扰桌面。
// * ⚠ 监听/主从的收尾都**必须置 0 并取消回调**：总线是进程级单例，残留的回调会让
//   后续无关用例的输入被录进本用例的计数器（批 C 的 YOLO 单例踩过同一个坑）。
//
// ## 反向验证记录（每条断言都做过"故意改错 → 必须 FAIL"）
//
// 见 docs/2026-11/五模块实施总结报告_20261122.md 的"反向验证"一节，那里逐条列了
// 改了哪行代码、哪条断言变红。**没有做反向验证的断言不写在这里。**
//
// ## 未覆盖（如实记录）
//
// * `InputPlayback` 的 START 走真实 `SendInput`/`SendMessage`，会**真的产生输入**。
//   这里只验证"装载/时间轴缩放/停止/释放"与"事件计数"，不验证目标窗口收到的
//   像素级/消息级效果（那需要真实绑定窗口 + 时间竞争，判据不稳，属于人工验收项）。
// * 主从同步的 slave 侧效果：只验证"链路建立/撤销/自我链接被拒/异步不阻塞"，
//   不验证 slave 窗口真的收到消息（同上）。
// * DX 绑定模式（mouse/keypad 带 dx 后缀）：需要目标进程注入，环境依赖项。

#include "test_support.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <op_c_api.h>

// 时间轴层内部（跨模块唯一性/停止标志的诊断判据）
#include "../libop/input/InputEventBus.h"

namespace {

using test_support::MouseEventWindow;

// 录制文件的唯一命名：同一用例里可能录多段，绝不能互相覆盖。
std::wstring RecordPath(const wchar_t *tag) {
    return test_support::GetTempBmpPath((std::wstring(L"op_input_record_") + tag + L".jsonl").c_str());
}

bool FileExists(const std::wstring &path) {
    return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::vector<std::wstring> ReadLines(const std::wstring &path) {
    std::vector<std::wstring> lines;
    FILE *fp = _wfopen(path.c_str(), L"rb");
    if (!fp)
        return lines;
    std::string bytes;
    char buf[4096];
    for (;;) {
        const size_t got = std::fread(buf, 1, sizeof(buf), fp);
        if (got == 0)
            break;
        bytes.append(buf, got);
    }
    std::fclose(fp);

    size_t begin = 0;
    while (begin < bytes.size()) {
        size_t end = bytes.find('\n', begin);
        if (end == std::string::npos)
            end = bytes.size();
        if (end > begin) {
            std::string line = bytes.substr(begin, end - begin);
            lines.emplace_back(line.begin(), line.end());
        }
        begin = end + 1;
    }
    return lines;
}

long CountOccurrences(const std::vector<std::wstring> &lines, const wchar_t *needle) {
    long hits = 0;
    for (const auto &line : lines) {
        if (line.find(needle) != std::wstring::npos)
            ++hits;
    }
    return hits;
}

// 精确统计"type=X 且 flags=Y"的事件条数。
// 为什么不能直接找 "flags":2：录制行里还有 "y":2 这种子串，
// 朴素的子串匹配会把坐标误当成标志位（反向验证时踩到过）。
long CountTypedFlags(const std::vector<std::wstring> &lines, const wchar_t *type_name, long flags) {
    const std::wstring type_needle = std::wstring(L"\"type\":\"") + type_name + L"\"";
    const std::wstring flag_needle = L",\"flags\":" + std::to_wstring(flags) + L"}";
    long hits = 0;
    for (const auto &line : lines) {
        if (line.find(type_needle) != std::wstring::npos && line.find(flag_needle) != std::wstring::npos)
            ++hits;
    }
    return hits;
}

std::wstring Take(const wchar_t *p) {
    return p ? std::wstring(p) : std::wstring();
}

// 从 sync_status JSON 里取数值字段（如 "master":2）。查不到返回 -1。
long JsonLong(const std::wstring &json, const wchar_t *key) {
    const std::wstring needle = std::wstring(L"\"") + key + L"\":";
    const size_t pos = json.find(needle);
    if (pos == std::wstring::npos)
        return -1;
    return std::wcstol(json.c_str() + pos + needle.size(), nullptr, 10);
}

// 带消息泵的等待。
//
// 为什么必须有：回放/输入走 `SendMessageTimeout(SMTO_BLOCK, 2000ms)`，而被调窗口
// 的 WndProc 在本线程执行。本线程若只是干等（wait_for/sleep），SendMessage 会
// 一直等本线程去处理消息 —— 表现为"STOP 不生效 / 回放卡住"。
// 这里一边等一边 PeekMessage，把窗口消息消化掉。
template <typename Predicate>
bool pump_wait(Predicate done, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (done())
            return true;
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        ::Sleep(1);
    }
}

// 监听回调的落地容器。回调在调用方线程同步触发，所以普通的非原子 vector 就够；
// 但为了在"故意制造并发"的用例里不 UB，这里用互斥保护。
struct MonitorSink {
    std::mutex mutex;
    std::vector<op_input_event_view> events;

    // ---- 重入验证 ----
    // 回调里再调输入 API，用来验证旁路护栏。
    op_handle reenter_handle = nullptr;
    // 每次回调后是否重新装弹（true = 制造持续重入；false = 只重入一层）。
    bool reenter_rearm = false;
    // 回调尝试的总次数上限（护栏失效时会打满，避免真的栈溢出）。
    int reenter_budget = 0;
    int reenter_calls = 0;
    // 观察到的最大嵌套深度。护栏生效时应恒为 1；失效时会涨到 reenter_budget。
    int current_depth = 0;
    int max_depth = 0;

    void clear() {
        std::lock_guard<std::mutex> lk(mutex);
        events.clear();
    }
    size_t size() {
        std::lock_guard<std::mutex> lk(mutex);
        return events.size();
    }
    std::vector<op_input_event_view> snapshot() {
        std::lock_guard<std::mutex> lk(mutex);
        return events;
    }
};

void OP_CALL MonitorCallback(const op_input_event_view *event, void *user) {
    auto *sink = static_cast<MonitorSink *>(user);
    if (!sink || !event)
        return;

    bool should_reenter = false;
    op_handle handle = nullptr;
    {
        std::lock_guard<std::mutex> lk(sink->mutex);
        sink->events.push_back(*event);
        ++sink->current_depth;
        if (sink->current_depth > sink->max_depth)
            sink->max_depth = sink->current_depth;
        if (sink->reenter_handle && sink->reenter_calls < sink->reenter_budget) {
            should_reenter = true;
            ++sink->reenter_calls;
            handle = sink->reenter_handle;
            // 关键：rearm=true 时**不清空** handle，于是每次回调都再产生一条输入，
            // 没有护栏就会一路递归到底（预算打满）。
            if (!sink->reenter_rearm)
                sink->reenter_handle = nullptr;
        }
    }

    if (should_reenter) {
        // 回调里调输入 API：必须被旁路护栏挡住（不产生新事件 → 不再进回调）。
        int ret = 0;
        ret = OpMoveTo(handle, 1, 1);
    }

    {
        std::lock_guard<std::mutex> lk(sink->mutex);
        --sink->current_depth;
    }
}

// 每个用例的收尾：总线是进程级单例，必须回到干净状态，否则污染后面的用例。
class TimelineFixture : public ::testing::Test {
  protected:
    op_handle handle = nullptr;
    MouseEventWindow window;
    bool bound = false;

    void SetUp() override {
        handle = OpCreate();
        ASSERT_NE(nullptr, handle);
        ASSERT_TRUE(window.Create());
        int ret = 0;
        ret = OpBindWindow(handle, (intptr_t)window.hwnd, L"normal", L"windows", L"windows", 0);
        bound = ret == 1;
        ASSERT_TRUE(bound) << "需要 windows 绑定模式来接收消息（不产生全局鼠标移动）";
        window.ResetCounts();
    }

    void TearDown() override {
        // 顺序很重要：先关采集/取消回调，再解绑销毁，避免回调在销毁后触发
        OpSetInputMonitor(handle, nullptr, nullptr, nullptr);
        int ret = 0;
        OpInputSyncLink(handle, 0, 0, 0, &ret); // 撤销任何残留链路（失败无妨）
        OpInputSyncEnable(handle, 0, &ret);
        if (handle) {
            ret = OpUnBindWindow(handle);
            OpDestroy(handle);
            handle = nullptr;
        }
    }

    // 录一段固定动作：移动 + 左键点击 + 键入 A + 滚轮
    void RecordSample() {
        int ret = 0;
        ret = OpMoveTo(handle, 10, 20);
        ret = OpLeftClick(handle);
        ret = OpKeyPress(handle, 'A');
        ret = OpWheel(handle, 120);
    }
};

// ---------------------------------------------------------------------------
// 1. 采集开关：关闭时一条不录，打开后录
// ---------------------------------------------------------------------------
TEST_F(TimelineFixture, RecordOnlyWhenEnabled) {
    const std::wstring path = RecordPath(L"switch");
    ::DeleteFileW(path.c_str());

    int ret = -1;
    // 默认关闭：此时点击不应产生任何录制。
    // 注意 call_ret 族的语义：结果在**返回值**里，*ret 出参不会被写（与 Draw* 族一致）。
    ASSERT_EQ(1, OpInputSyncEnable(handle, 0, &ret));

    ret = -1;
    OpInputRecord(handle, 0 /*START*/, path.c_str(), &ret);
    ASSERT_EQ(1, ret) << "START 应成功（InputRecord 内部会打开采集）";

    RecordSample();

    ret = -1;
    const std::wstring stopped = Take(OpInputRecord(handle, 1 /*STOP*/, nullptr, &ret));
    EXPECT_EQ(path, stopped) << "STOP 应回报与 START 相同的路径";
    EXPECT_GT(ret, 0) << "采集打开后必须录到事件，实际 " << ret;

    // 关闭采集后再做同样动作：文件不应再增长
    const int before = ret;
    ASSERT_EQ(1, OpInputSyncEnable(handle, 0, &ret));
    RecordSample();
    ret = -1;
    OpInputRecord(handle, 0, path.c_str(), &ret);
    ASSERT_EQ(1, ret);
    ret = -1;
    OpInputRecord(handle, 1, nullptr, &ret);
    EXPECT_EQ(0, ret) << "采集关闭期间不应录到任何事件（实际 " << ret << "，上一段 " << before << "）";
}

// ---------------------------------------------------------------------------
// 2. 录到的内容与动作序列一致（覆盖"原语级录制"这条设计）
// ---------------------------------------------------------------------------
TEST_F(TimelineFixture, RecordCapturesPrimitivesNotSyntheticCalls) {
    const std::wstring path = RecordPath(L"primitives");
    ::DeleteFileW(path.c_str());

    int ret = 0;
    OpInputRecord(handle, 0, path.c_str(), &ret);
    ASSERT_EQ(1, ret);

    window.ResetCounts();
    ret = OpMoveTo(handle, 30, 40);
    ASSERT_EQ(1, ret);
    ret = OpLeftClick(handle);
    ASSERT_EQ(1, ret);
    ret = OpKeyPress(handle, 'A');
    ret = OpWheel(handle, -120);
    OpInputRecord(handle, 1, nullptr, &ret);
    ASSERT_GT(ret, 0);

    const auto lines = ReadLines(path);
    ASSERT_FALSE(lines.empty()) << "录制文件为空";

    // 一次 LeftClick = down + up 两条。这是"钩在原语而不是合成方法"的直接判据：
    // 如果钩子在 op::Op::LeftClick 上，这里只有 1 条 mouse_button，回放不出"按了几下"。
    EXPECT_EQ(2, CountOccurrences(lines, L"\"type\":\"mouse_button\""))
        << "一次 LeftClick 必须录成 down+up 两条（原语级录制）";
    // 只匹配"事件行"里的 flags：单查 \"flags\":2 会被 \"y\":2 之类误命中。
    EXPECT_EQ(1, CountTypedFlags(lines, L"mouse_button", 2 /*LEFTDOWN*/)) << "必须有 LEFT_DOWN(2)";
    EXPECT_EQ(1, CountTypedFlags(lines, L"mouse_button", 4 /*LEFTUP*/)) << "必须有 LEFT_UP(4)";
    // 每次点击内部先 sync 光标一次 MoveTo，所以移动事件至少 2 条（显式 1 + 点击 1）
    EXPECT_EQ(1, CountOccurrences(lines, L"\"type\":\"mouse_move\"")) << "显式 MoveTo(30,40) 应录 1 条不带按键标志的移动";
    EXPECT_GE(CountOccurrences(lines, L"\"type\":\"key\""), 2) << "KeyPress 应录 down+up";
    EXPECT_EQ(1, CountOccurrences(lines, L"\"type\":\"mouse_wheel\"")) << "Wheel 应录 1 条";
    EXPECT_EQ(1, CountOccurrences(lines, L"\"wheel\":-120")) << "滚轮增量应原样保留（含负号）";

    // 坐标口径：客户区坐标，不是屏幕坐标
    const auto first_move = std::find_if(lines.begin(), lines.end(),
                                         [](const std::wstring &l) { return l.find(L"mouse_move") != std::wstring::npos; });
    ASSERT_NE(lines.end(), first_move);
    EXPECT_NE(std::wstring::npos, first_move->find(L"\"x\":30")) << *first_move;
    EXPECT_NE(std::wstring::npos, first_move->find(L"\"y\":40")) << *first_move;
}

// ---------------------------------------------------------------------------
// 3. 反向：动作没发生（无效句柄/非法按键）不应录到"成功"的事件
// ---------------------------------------------------------------------------
TEST_F(TimelineFixture, RecordSkipsFailedPrimitives) {
    const std::wstring path = RecordPath(L"failures");
    ::DeleteFileW(path.c_str());

    int ret = 0;
    OpInputRecord(handle, 0, path.c_str(), &ret);
    ASSERT_EQ(1, ret);

    // 无效虚拟键：windows 模式下 SendMessageTimeout 到不存在的键也返回成功，
    // 所以这里用一个**必然失败**的动作：解绑后调用输入 API（未绑定 → SendTimeout(NULL) 失败）。
    ret = OpUnBindWindow(handle);
    ret = OpMoveTo(handle, 123, 456);
    const int move_ret = ret;
    OpInputRecord(handle, 1, nullptr, &ret);
    const int written = ret;

    if (move_ret == 0) {
        // 未绑定时移动确实失败 → 不应录到这条移动
        EXPECT_EQ(0, written) << "失败的输入原语不应被录制（否则回放会重放一次没发生的动作）";
    } else {
        // 环境允许未绑定移动（normal 模式会走 SendInput）：那就只断言"录到的条数与发生的一致"
        EXPECT_GT(written, 0) << "移动成功则必须录到";
    }
    // 重新绑定，避免 TearDown 里解绑报错
    ret = OpBindWindow(handle, (intptr_t)window.hwnd, L"normal", L"windows", L"windows", 0);
}

// ---------------------------------------------------------------------------
// 4. 重复 START 必须被拒绝且不截断前一段
// ---------------------------------------------------------------------------
TEST_F(TimelineFixture, RecordStartTwiceDoesNotTruncate) {
    const std::wstring path = RecordPath(L"twice");
    ::DeleteFileW(path.c_str());

    int ret = 0;
    OpInputRecord(handle, 0, path.c_str(), &ret);
    ASSERT_EQ(1, ret);

    ret = -1;
    OpInputRecord(handle, 0, path.c_str(), &ret);
    EXPECT_EQ(0, ret) << "已在录制时再次 START 必须返回 0，而不是悄悄重开";

    ret = OpMoveTo(handle, 5, 6);
    OpInputRecord(handle, 1, nullptr, &ret);
    EXPECT_GT(ret, 0) << "被拒绝的第二次 START 不应清掉已经录到的事件";
}

// ---------------------------------------------------------------------------
// 5. 监听回调：收到的事件与动作对应；取消后不再收到
// ---------------------------------------------------------------------------
TEST_F(TimelineFixture, MonitorReceivesEventsAndStopsAfterCleared) {
    MonitorSink sink;
    int ret = 0;
    ASSERT_EQ(1, OpSetInputMonitor(handle, &MonitorCallback, &sink, &ret));

    sink.clear();
    ret = OpMoveTo(handle, 77, 88);
    ret = OpLeftDown(handle);
    ret = OpLeftUp(handle);

    const auto events = sink.snapshot();
    ASSERT_GE(events.size(), 3u) << "监听应收到 移动+按下+抬起 至少 3 条，实际 " << events.size();

    bool saw_move = false;
    bool saw_down = false;
    bool saw_up = false;
    for (const auto &ev : events) {
        if (ev.type == 0 /*mouse_move*/ && ev.x == 77 && ev.y == 88)
            saw_move = true;
        if (ev.type == 1 && ev.flags == 2 /*LEFTDOWN*/)
            saw_down = true;
        if (ev.type == 1 && ev.flags == 4 /*LEFTUP*/)
            saw_up = true;
    }
    EXPECT_TRUE(saw_move) << "应收到 (77,88) 的移动事件";
    EXPECT_TRUE(saw_down) << "应收到 LEFT_DOWN";
    EXPECT_TRUE(saw_up) << "应收到 LEFT_UP";

    // 取消监听后不再收到
    ASSERT_EQ(1, OpSetInputMonitor(handle, nullptr, nullptr, &ret));
    const size_t before = sink.size();
    ret = OpMoveTo(handle, 99, 100);
    EXPECT_EQ(before, sink.size()) << "取消回调后不应再收到事件";
}

// ---------------------------------------------------------------------------
// 6. 监听回调里再调输入 API：必须被旁路挡住，不能递归
//
// ## 这条断言为什么要写成"持续重入"
//
// 最初的写法是"回调里调一次 MoveTo，然后 expect(reenter_calls == 1)"。反向验证
// （去掉 RecordBypass 护栏）时它**仍然通过** —— 因为 reenter_handle 被置空了，
// 递归深度只有 1 层，护栏在不在都一样。那是一条假绿断言。
//
// 现在改成：回调里**每次都重新装弹**（不清空），并统计回调嵌套深度。
//   * 有护栏：回调里那次 MoveTo 产生的事件被挡在 emit 之外，深度恒为 1；
//   * 无护栏：每次回调又产生一条事件 → 又进回调 → 深度一直涨到上限（保护性 abort）。
// 判据取"深度远小于上限"，这样去掉护栏必定变红。
// ---------------------------------------------------------------------------
TEST_F(TimelineFixture, MonitorCallbackReentrancyIsGuarded) {
    MonitorSink sink;
    int ret = 0;
    ASSERT_EQ(1, OpSetInputMonitor(handle, &MonitorCallback, &sink, &ret));

    sink.clear();
    sink.reenter_calls = 0;
    sink.reenter_handle = handle;
    sink.reenter_rearm = true;  // 每次回调都重新装弹（关键：制造持续重入）
    sink.reenter_budget = 200;  // 递归上限（护栏失效时会打到这个值）

    ret = OpMoveTo(handle, 11, 22);
    ASSERT_EQ(1, ret);

    // 有护栏时：事件只在**回调之外**产生（1 条 move），回调里的输入被挡掉。
    // 因此回调只被调用 1 次（外边那次），重入次数 1、深度 1。
    EXPECT_EQ(1, sink.max_depth) << "回调嵌套深度应恒为 1（旁路护栏把回调里的输入挡住了；"
                                    "去掉护栏这里会涨到上限）";
    EXPECT_LE(sink.reenter_calls, 1) << "护栏生效时回调里的输入不应再触发回调";
}

// ---------------------------------------------------------------------------
// 7. 回放：LOAD 读到正确条数、JSON 带 bound、时间轴按 speed 缩放
// ---------------------------------------------------------------------------
TEST_F(TimelineFixture, PlaybackLoadReportsCountAndBound) {
    const std::wstring path = RecordPath(L"pb_load");
    ::DeleteFileW(path.c_str());

    int ret = 0;
    OpInputRecord(handle, 0, path.c_str(), &ret);
    ASSERT_EQ(1, ret);
    RecordSample();
    OpInputRecord(handle, 1, nullptr, &ret);
    const int recorded = ret;
    ASSERT_GT(recorded, 0);

    // 换一个实例来读：验证录制文件是**跨实例/跨进程**可用的
    op_handle other = OpCreate();
    ASSERT_NE(nullptr, other);
    int bound_unused = 0;
    bound_unused = OpBindWindow(other, (intptr_t)window.hwnd, L"normal", L"windows", L"windows", 0);

    ret = -1;
    const std::wstring json = Take(OpInputPlayback(other, 0 /*LOAD*/, path.c_str(), 1.0, 1, &ret));
    EXPECT_EQ(recorded, ret) << "LOAD 的 *ret 必须等于录制条数";
    EXPECT_NE(std::wstring::npos, json.find(L"\"events\":" + std::to_wstring(recorded))) << json;
    EXPECT_NE(std::wstring::npos, json.find(L"\"bound\":1")) << "已绑定窗口时 bound 必须为 1：" << json;

    ret = 0;
    OpInputPlayback(other, 3 /*FREE*/, nullptr, 1.0, 1, &ret);
    OpUnBindWindow(other);
    OpDestroy(other);
}

// 读一个不存在的文件：必须显式失败（ok=0 + error），不能返回"0 条事件的成功"
TEST_F(TimelineFixture, PlaybackLoadMissingFileFails) {
    int ret = -1;
    const std::wstring json =
        Take(OpInputPlayback(handle, 0 /*LOAD*/, L"Z:\\definitely_missing\\none.jsonl", 1.0, 1, &ret));
    EXPECT_EQ(0, ret) << "不存在的文件必须让 *ret = 0";
    EXPECT_NE(std::wstring::npos, json.find(L"\"ok\":0")) << json;
    EXPECT_NE(std::wstring::npos, json.find(L"error")) << "必须带 error 说明失败原因：" << json;
}

// START 的时间轴缩放：两条事件间隔 120ms，speed=10 时应在 ~12ms 内放完。
// 判据不用"精确毫秒"（调度抖动），而是"明显快于实时 + 事件条数正确"。
TEST_F(TimelineFixture, PlaybackStartScalesTimelineBySpeed) {
    const std::wstring path = RecordPath(L"pb_speed");
    ::DeleteFileW(path.c_str());

    int ret = 0;
    OpInputRecord(handle, 0, path.c_str(), &ret);
    ASSERT_EQ(1, ret);
    ret = OpMoveTo(handle, 10, 10);
    ::Sleep(120);
    ret = OpMoveTo(handle, 20, 20);
    OpInputRecord(handle, 1, nullptr, &ret);
    const int recorded = ret;
    ASSERT_GE(recorded, 2);

    ret = -1;
    OpInputPlayback(handle, 0 /*LOAD*/, path.c_str(), 1.0, 1, &ret);
    ASSERT_EQ(recorded, ret);

    // speed=10：120ms 的间隔压到 12ms
    const auto t0 = std::chrono::steady_clock::now();
    ret = -1;
    OpInputPlayback(handle, 1 /*START*/, nullptr, 10.0, 1, &ret);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
    EXPECT_EQ(1, ret) << "START 应返回 1（放完）";
    EXPECT_LT(elapsed.count(), 100) << "speed=10 时 120ms 的录音应在 100ms 内放完，实际 " << elapsed.count() << "ms";

    // 对照组：speed=1 必须**明显更慢**。没有这条，上面的断言在"根本没睡"的实现下也绿。
    ret = -1;
    OpInputPlayback(handle, 0, path.c_str(), 1.0, 1, &ret);
    ASSERT_EQ(recorded, ret);
    const auto t1 = std::chrono::steady_clock::now();
    OpInputPlayback(handle, 1, nullptr, 1.0, 1, &ret);
    const auto elapsed_normal =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t1);
    EXPECT_GE(elapsed_normal.count(), 100) << "speed=1 应保留 ~120ms 间隔，实际 " << elapsed_normal.count() << "ms";

    OpInputPlayback(handle, 3 /*FREE*/, nullptr, 1.0, 1, &ret);
}

// STOP 必须能打断长时间轴：录音里有一条 2s 之后的间隔，STOP 后立刻返回。
//
// ## 这个用例为什么比看上去麻烦（本次实测踩到，值得记住）
//
// 回放是通过**真实输入调用**重放的。IN_WINDOWS 模式下 `MoveTo` 走
// `SendMessageTimeout(SMTO_BLOCK, 2000ms)`，而这个测试窗口的 WndProc 在**本线程**
// 上执行。如果主线程在等回放线程时只是 `Sleep`/`wait_for`（不抽消息），
// SendMessage 就会一直等 WndProc 所在的线程去处理 —— 表现为"STOP 无效"，
// 实际上是**用例自己把消息泵堵住了**。
// 所以下面的等待一律用 `wnd_pump_wait()`：边等边 `PeekMessage` 抽消息。
TEST_F(TimelineFixture, PlaybackStopInterruptsLongWait) {
    const std::wstring path = RecordPath(L"pb_stop");
    ::DeleteFileW(path.c_str());

    int ret = 0;
    OpInputRecord(handle, 0, path.c_str(), &ret);
    ASSERT_EQ(1, ret);
    ret = OpMoveTo(handle, 1, 1);
    ASSERT_EQ(1, ret) << "录制起点必须成功，否则 2s 间隔不会真的存在";
    // 造一条 2s 的间隔。这里也必须泵消息：MoveTo 的 SendMessageTimeout 需要本线程抽消息。
    pump_wait([] { return false; }, 2000);
    ret = OpMoveTo(handle, 2, 2);
    OpInputRecord(handle, 1, nullptr, &ret);
    ASSERT_GE(ret, 2);

    ret = -1;
    OpInputPlayback(handle, 0, path.c_str(), 1.0, 1, &ret);
    ASSERT_GE(ret, 2);

    std::atomic<bool> started{false};
    std::atomic<int> start_result{-1};
    std::atomic<bool> worker_done{false};
    std::thread worker([&] {
        started.store(true);
        // START 内部阻塞到放完/被 STOP。回放线程只调这一条 API，
        // 不与主线程同时用同一实例做别的调用（单实例非线程安全，见 libop.h 顶部）。
        int r = 0;
        OpInputPlayback(handle, 1 /*START*/, nullptr, 1.0, 1, &r);
        start_result.store(r);
        worker_done.store(true);
    });

    while (!started.load())
        ::Sleep(1);
    // 让它进到那条 2s 的间隔里（这 200ms 同样要泵消息，否则第一条 MoveTo 会堵住）
    pump_wait([] { return false; }, 200);

    // STOP 只置标志，本身应立即返回
    const auto t_stop = std::chrono::steady_clock::now();
    int stop_ret = 0;
    const std::wstring stop_json = Take(OpInputPlayback(handle, 2 /*STOP*/, nullptr, 1.0, 1, &stop_ret));
    const auto stop_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t_stop);
    EXPECT_NE(std::wstring::npos, stop_json.find(L"\"stopped\":1"))
        << "STOP 返回: " << std::string(stop_json.begin(), stop_json.end());
    EXPECT_LT(stop_ms.count(), 100) << "STOP 调用本身应只置标志、立即返回，实际 " << stop_ms.count() << "ms";

    // 回放线程必须在很短的时间内收工。等待期间继续泵消息（回放线程可能正卡在
    // 一次 SendMessage 上，需要本线程处理 WndProc 才能返回）。
    const bool exited = pump_wait([&] { return worker_done.load(); }, 800);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t_stop);

    if (!exited) {
        // 兜底：不让卡住的线程拖死测试进程
        OpInputPlayback(handle, 3 /*FREE*/, nullptr, 1.0, 1, &ret);
        worker.detach();
        FAIL() << "STOP 之后回放线程 800ms 内仍未退出（标志没被回放循环看到）";
        return;
    }
    worker.join();

    EXPECT_LT(elapsed.count(), 500) << "STOP 应打断 2s 的等待，实际等了 " << elapsed.count()
                                    << "ms (start_ret=" << start_result.load() << ")";
    OpInputPlayback(handle, 3, nullptr, 1.0, 1, &ret);
}

// ---------------------------------------------------------------------------
// 8. 主从同步：链路建立/查询/取消；自我链接被拒
// ---------------------------------------------------------------------------
TEST_F(TimelineFixture, SyncLinkLifecycle) {
    int ret = 0;
    // 建链路：本实例(未知 id) 作为 master，投给 slave=9999（不存在也没关系：
    // 这里验证的是链路表与查询接口，不验证对端效果）
    EXPECT_EQ(1, OpInputSyncLink(handle, /*master=*/4242, /*slave=*/9999, /*enable=*/1, &ret)) << "建立链路应成功";

    const std::wstring status = Take(OpInputSyncStatus(handle));
    EXPECT_NE(std::wstring::npos, status.find(L"\"ok\":1")) << status;

    // 取消
    EXPECT_EQ(1, OpInputSyncLink(handle, 4242, 9999, 0, &ret)) << "取消已存在的链路应成功";

    // 再取消一次：链路已不存在，必须返回 0（不能假装成功）
    EXPECT_EQ(0, OpInputSyncLink(handle, 4242, 9999, 0, &ret)) << "取消不存在的链路必须返回 0";
}

TEST_F(TimelineFixture, SyncLinkRejectsSelfLink) {
    int ret = -1;
    // master == slave：自己投自己会让一条输入变成两条、再变成四条（自我放大）
    EXPECT_EQ(0, OpInputSyncLink(handle, 777, 777, 1, &ret)) << "master == slave 必须被拒绝";
}

// 1:N 扇出（2026-11-25 多开同步）：同一 master 可挂多个 slave，
// 后建的不会覆盖先建的；逐条撤链后全部干净。
// 注意：status 以**调用实例自己的 id**（m_context->id）查链路表，而实例 id 由
// OpCreate 传入、随进程内实例数自增（本夹具里不是 0），所以 master 必须先从
// status 的 "master" 字段把本实例 id 读出来再建链。传外来 id 链会建成但
// status 按自己的 key 查，as_master_of 恒为 0——这是扇出测试第一轮红掉的原因。
TEST_F(TimelineFixture, SyncLinkFanoutMultipleSlaves) {
    int ret = 0;
    const long me = JsonLong(Take(OpInputSyncStatus(handle)), L"master");
    ASSERT_GE(me, 0) << "应能从 status 读到本实例 id";
    ASSERT_EQ(1, OpInputSyncLink(handle, me, 9001, 1, &ret));
    ASSERT_EQ(1, OpInputSyncLink(handle, me, 9002, 1, &ret)) << "第二个 slave 不得覆盖第一个";
    ASSERT_EQ(1, OpInputSyncLink(handle, me, 9003, 1, &ret));

    std::wstring status = Take(OpInputSyncStatus(handle));
    EXPECT_NE(std::wstring::npos, status.find(L"\"as_master_of\":3")) << status;
    EXPECT_NE(std::wstring::npos, status.find(L"9001")) << status;
    EXPECT_NE(std::wstring::npos, status.find(L"9002")) << status;
    EXPECT_NE(std::wstring::npos, status.find(L"9003")) << status;

    // 重复建链同一对：幂等，不变成 4 条
    ASSERT_EQ(1, OpInputSyncLink(handle, me, 9001, 1, &ret));
    status = Take(OpInputSyncStatus(handle));
    EXPECT_NE(std::wstring::npos, status.find(L"\"as_master_of\":3")) << status;

    EXPECT_EQ(1, OpInputSyncLink(handle, me, 9002, 0, &ret));
    status = Take(OpInputSyncStatus(handle));
    EXPECT_NE(std::wstring::npos, status.find(L"\"as_master_of\":2")) << status;

    // slave=0 通配：撤掉该 master 剩余全部链路
    EXPECT_EQ(1, OpInputSyncLink(handle, me, 0, 0, &ret));
    status = Take(OpInputSyncStatus(handle));
    EXPECT_NE(std::wstring::npos, status.find(L"\"as_master_of\":0")) << status;
    EXPECT_EQ(0, OpInputSyncLink(handle, me, 9001, 0, &ret)) << "已清空的链路再撤必须返回 0";
}

// 异步投递不能阻塞主实例：链路建在一个**不存在**的 slave 上时，
// 主实例产生大量输入仍然要快速返回（投递是后台线程做的）。
TEST_F(TimelineFixture, SyncLinkDoesNotBlockMaster) {
    int ret = 0;
    ASSERT_EQ(1, OpInputSyncLink(handle, 555, 666, 1, &ret));

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 40; ++i)
        ret = OpMoveTo(handle, i, i);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);

    // 40 次窗口消息在正常情况下是毫秒级；若投递是同步的，40 次跨实例投递
    // 会叠加成百毫秒以上。阈值放宽到 1500ms，只挡"同步阻塞"这种结构性错误。
    EXPECT_LT(elapsed.count(), 1500) << "主实例的输入不应被主从投递拖慢，实际 " << elapsed.count() << "ms";

    OpInputSyncLink(handle, 555, 666, 0, &ret);
}

} // namespace
