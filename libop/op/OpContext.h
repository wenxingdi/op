#pragma once
#ifndef OP_OP_CONTEXT_H_
#define OP_OP_CONTEXT_H_

// 标准库必须**先于**本项目头文件引入。
// 2026-11-22 实测：本文件引入 <atomic> 后，若把它放在 PathTools.h/BindingSession.h 之后，
// 编译 OpContext.cpp 会把 MSVC 的 <chrono>/<filesystem>/<__msvc_tzdb.hpp> 解析坏
// （"error C2061: 标识符 true_type"、C3861 is_same_v）。根因是本项目这些头会先把
// <chrono>/<filesystem> 拉进来，等 <atomic> 再来时 MSVC 标准库已经处于不自洽状态。
// 把标准库整体前置即可稳定（与 OpCrypto.cpp 的踩坑是同一类：标准库要成组、且最先）。
#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "algorithm/PathTools.h"
#include "binding/BindingSession.h"
#include "image/ImageSearchService.h"
#include "window/WindowService.h"

namespace op::internal {

struct OpContext {
    explicit OpContext(int client_id);
    // atomic 成员不可拷贝/移动（本来 OpContext 也从不被拷贝）
    OpContext(const OpContext &) = delete;
    OpContext &operator=(const OpContext &) = delete;

    // Windows API helpers
    WindowService window_service;
    // Window binding and capture/input state
    op::binding::BindingSession bkproc;
    // Image processing module
    op::image::ImageSearchService image_proc;
    // work path
    std::wstring curr_path;

    std::map<std::wstring, long> vkmap;
    std::vector<unsigned char> screenData;
    std::vector<unsigned char> screenDataBmp;
    // 寻路障碍位图会话状态（SetAStarMap / OpSetAStarMapData 设置，供 AStarFindPathBM/
    // AStarFindPathWay/SmoothPathByLOS/IsLineBlocked 共用）
    op::pathtools::AStarMapState astar_map;
    std::wstring opPath;
    long screen_data_mode = 0;
    int id = 0;

    // ---- 并发可观测性（批 A 2026-11-22）----
    // 单实例**不是**线程安全的（见 libop.h 顶部约定）。本批不把它改造成线程安全
    // （348 个方法逐个加锁会显著掉性能且有死锁风险，也和大漠语义不一致），
    // 而是把"同实例并发"从**静默污染**变成**可观测事件**：
    //   in_flight        当前在本实例上执行的调用数（同一时刻 >1 即并发使用）
    //   peak_in_flight   历史峰值并发度（>1 说明脚本确实踩了这条线）
    //   reentrant_hits   检测到同实例并发使用的累计次数（>0 时结果不再可复现）
    //   calls_total      累计调用次数
    // 只做原子增减，不加锁，热路径开销约 2 次 fetch_add。
    //
    // 注：这里的计数量来自**门面层**（op_c_api.cpp 的 CallScope）而不是 Op 的每个方法，
    // 因此对 C API / COM 调用有效，C++ 直接调用 op::Op 不计数。
    std::atomic<int> in_flight{0};
    std::atomic<int> peak_in_flight{0};
    std::atomic<long> reentrant_hits{0};
    std::atomic<long> calls_total{0};
};

} // namespace op::internal

#endif // OP_OP_CONTEXT_H_
