#include "WindowLayout.h"

#include "../base/Utils.h"

#include <dwmapi.h>

namespace op::window_layout {

namespace {
constexpr int kResizeTolerance = 2;

struct Insets {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

struct LayoutSeed {
    Rect rect = {};
    int anchor_width = 0;
    int anchor_height = 0;
    int anchor_offset_x = 0;
    int anchor_offset_y = 0;
};

bool GetWindowSize(HWND hwnd, int &width, int &height) {
    RECT rc = {};
    if (!::GetWindowRect(hwnd, &rc)) {
        return false;
    }

    width = rc.right - rc.left;
    height = rc.bottom - rc.top;
    return true;
}

bool GetWindowRectInfo(HWND hwnd, Rect &rect) {
    RECT rc = {};
    if (!::GetWindowRect(hwnd, &rc)) {
        return false;
    }

    rect.x = rc.left;
    rect.y = rc.top;
    rect.width = rc.right - rc.left;
    rect.height = rc.bottom - rc.top;
    return true;
}

bool GetVisibleWindowRectInfo(HWND hwnd, Rect &rect) {
    RECT rc = {};
    if (::DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rc, sizeof(rc)) != S_OK) {
        return GetWindowRectInfo(hwnd, rect);
    }

    rect.x = rc.left;
    rect.y = rc.top;
    rect.width = rc.right - rc.left;
    rect.height = rc.bottom - rc.top;
    return true;
}

bool GetClientRectInfo(HWND hwnd, Rect &rect) {
    RECT rc = {};
    if (!::GetClientRect(hwnd, &rc)) {
        return false;
    }

    POINT pt = {0, 0};
    if (!::ClientToScreen(hwnd, &pt)) {
        return false;
    }

    rect.x = pt.x;
    rect.y = pt.y;
    rect.width = rc.right - rc.left;
    rect.height = rc.bottom - rc.top;
    return true;
}

bool GetClientSize(HWND hwnd, int &width, int &height) {
    RECT rc = {};
    if (!::GetClientRect(hwnd, &rc)) {
        return false;
    }

    width = rc.right - rc.left;
    height = rc.bottom - rc.top;
    return true;
}

void GetInsets(const Metrics &metrics, Insets &insets) {
    insets.left = metrics.client_rect.x - metrics.window_rect.x;
    insets.top = metrics.client_rect.y - metrics.window_rect.y;
    insets.right = metrics.window_rect.width - insets.left - metrics.client_rect.width;
    insets.bottom = metrics.window_rect.height - insets.top - metrics.client_rect.height;
}

bool IsValidUniformSize(const Options &options) {
    if (options.size_mode != SizeMode::Uniform)
        return true;

    return options.window_width >= kMinWindowWidth && options.window_height >= kMinWindowHeight;
}

bool MatchesTargetSize(HWND hwnd, const Rect &rect) {
    int width = 0;
    int height = 0;
    if (!GetWindowSize(hwnd, width, height)) {
        return false;
    }

    return abs(width - rect.width) <= kResizeTolerance && abs(height - rect.height) <= kResizeTolerance;
}

bool MatchesTargetClientSize(HWND hwnd, const Options &options) {
    if (options.anchor_mode != AnchorMode::Client) {
        return true;
    }

    int width = 0;
    int height = 0;
    if (!GetClientSize(hwnd, width, height)) {
        return false;
    }

    return abs(width - options.window_width) <= kResizeTolerance &&
           abs(height - options.window_height) <= kResizeTolerance;
}

bool MatchesTargetClientRect(HWND hwnd, const Rect &rect, const Metrics &initial_metrics) {
    Metrics current_metrics = {};
    if (!GetWindowMetrics(hwnd, current_metrics)) {
        return false;
    }

    Insets insets = {};
    GetInsets(initial_metrics, insets);

    const int expected_x = rect.x + insets.left;
    const int expected_y = rect.y + insets.top;
    return abs(current_metrics.client_rect.x - expected_x) <= kResizeTolerance &&
           abs(current_metrics.client_rect.y - expected_y) <= kResizeTolerance;
}

bool BuildUniformWindowRect(HWND hwnd, const Options &options, Rect &rect) {
    RECT window_rect = {};
    RECT client_rect = {};
    if (!::GetWindowRect(hwnd, &window_rect) || !::GetClientRect(hwnd, &client_rect)) {
        return false;
    }

    const int frame_width = (window_rect.right - window_rect.left) - (client_rect.right - client_rect.left);
    const int frame_height = (window_rect.bottom - window_rect.top) - (client_rect.bottom - client_rect.top);
    rect.width = options.window_width + frame_width;
    rect.height = options.window_height + frame_height;
    return rect.width > 0 && rect.height > 0;
}

std::vector<LayoutSeed> BuildLayoutSeeds(const std::vector<HWND> &windows, const Options &options) {
    std::vector<LayoutSeed> seeds;
    seeds.reserve(windows.size());

    for (HWND hwnd : windows) {
        LayoutSeed seed = {};
        Metrics metrics = {};
        Insets insets = {};
        const bool has_metrics = GetWindowMetrics(hwnd, metrics);
        if (has_metrics) {
            GetInsets(metrics, insets);
        }

        // 保持原大小时，优先读取窗口当前尺寸。
        if (options.size_mode == SizeMode::Keep) {
            if (!GetWindowSize(hwnd, seed.rect.width, seed.rect.height)) {
                seed.rect.width = options.window_width;
                seed.rect.height = options.window_height;
            }

            if (options.anchor_mode == AnchorMode::Client && has_metrics) {
                seed.anchor_width = metrics.client_rect.width;
                seed.anchor_height = metrics.client_rect.height;
                seed.anchor_offset_x = insets.left;
                seed.anchor_offset_y = insets.top;
            } else {
                seed.anchor_width = seed.rect.width;
                seed.anchor_height = seed.rect.height;
            }
        } else {
            if (!BuildUniformWindowRect(hwnd, options, seed.rect)) {
                seed.rect.width = options.window_width;
                seed.rect.height = options.window_height;
            }

            if (options.anchor_mode == AnchorMode::Client && has_metrics) {
                seed.anchor_width = options.window_width;
                seed.anchor_height = options.window_height;
                seed.anchor_offset_x = insets.left;
                seed.anchor_offset_y = insets.top;
            } else {
                seed.anchor_width = seed.rect.width;
                seed.anchor_height = seed.rect.height;
            }
        }

        seeds.push_back(seed);
    }

    return seeds;
}

} // namespace

bool GetWindowMetrics(HWND hwnd, Metrics &metrics) {
    if (!GetWindowRectInfo(hwnd, metrics.window_rect)) {
        return false;
    }
    if (!GetVisibleWindowRectInfo(hwnd, metrics.visible_rect)) {
        return false;
    }
    if (!GetClientRectInfo(hwnd, metrics.client_rect)) {
        return false;
    }
    return true;
}

std::vector<Rect> Calculate(const std::vector<HWND> &windows, const Options &options) {
    std::vector<LayoutSeed> seeds = BuildLayoutSeeds(windows, options);
    std::vector<Rect> rects;
    rects.reserve(seeds.size());
    if (seeds.empty()) {
        return rects;
    }

    int max_anchor_width = 0;
    int max_anchor_height = 0;
    for (const auto &seed : seeds) {
        if (seed.anchor_width > max_anchor_width)
            max_anchor_width = seed.anchor_width;
        if (seed.anchor_height > max_anchor_height)
            max_anchor_height = seed.anchor_height;
    }

    int current_x = options.start_x;
    int current_y = options.start_y;

    switch (options.type) {
    case Type::Grid: {
        const int columns = options.columns > 0 ? options.columns : 1;
        for (size_t i = 0; i < seeds.size(); ++i) {
            const int col = static_cast<int>(i) % columns;
            const int row = static_cast<int>(i) / columns;
            Rect rect = seeds[i].rect;
            const int anchor_x = options.start_x + col * (max_anchor_width + options.gap_x);
            const int anchor_y = options.start_y + row * (max_anchor_height + options.gap_y);

            // 宫格使用统一单元格，避免原窗口大小不一致时互相重叠。
            rect.x = anchor_x - seeds[i].anchor_offset_x;
            rect.y = anchor_y - seeds[i].anchor_offset_y;
            rects.push_back(rect);
        }
        break;
    }

    case Type::Diagonal: {
        for (const auto &seed : seeds) {
            Rect rect = seed.rect;
            rect.x = current_x - seed.anchor_offset_x;
            rect.y = current_y - seed.anchor_offset_y;
            current_x += seed.anchor_width + options.gap_x;
            current_y += seed.anchor_height + options.gap_y;
            rects.push_back(rect);
        }
        break;
    }

    case Type::Cascade: {
        // 层叠：第 i 个窗口相对起点偏移 i*(gap_x, gap_y)，尺寸取 seed 各自值。
        // gap 此时语义为"层叠步距"，经典值 32（标题栏逐层露出便于点击切换）。
        for (size_t i = 0; i < seeds.size(); ++i) {
            Rect rect = seeds[i].rect;
            rect.x = options.start_x + static_cast<int>(i) * options.gap_x - seeds[i].anchor_offset_x;
            rect.y = options.start_y + static_cast<int>(i) * options.gap_y - seeds[i].anchor_offset_y;
            rects.push_back(rect);
        }
        break;
    }
    }

    return rects;
}

long Apply(const std::vector<HWND> &windows, const std::vector<Rect> &rects, const Options &options) {
    if (windows.empty() || windows.size() != rects.size()) {
        return 0;
    }

    // 阶段1：全量 SetWindowPos（不动 Z 序/不激活），并记录移动前的 insets 供阶段2校验。
    // 阶段2：统一 Delay(100) 后逐窗校验——"某些窗口会吞掉尺寸修改"，校验防误报成功。
    // 两阶段把等待从 N×100ms 降到 1×100ms，校验语义与逐窗交错版严格一致。
    std::vector<Metrics> initial_metrics(windows.size());
    std::vector<bool> has_initial_metrics(windows.size(), false);
    for (size_t i = 0; i < windows.size(); ++i) {
        if (!::IsWindow(windows[i])) {
            setlog(L"LayoutWindows: hwnd[%zu]=%p 不是有效窗口，已取消（前 %zu 个可能已移动）", i,
                   windows[i], i);
            return 0;
        }

        has_initial_metrics[i] = GetWindowMetrics(windows[i], initial_metrics[i]);
        const auto &rect = rects[i];
        if (!::SetWindowPos(windows[i], nullptr, rect.x, rect.y, rect.width, rect.height,
                            SWP_NOZORDER | SWP_NOACTIVATE)) {
            setlog(L"LayoutWindows: SetWindowPos 失败 hwnd[%zu]=%p, err=%lu", i, windows[i],
                   ::GetLastError());
            return 0;
        }
    }

    ::Delay(100);

    for (size_t i = 0; i < windows.size(); ++i) {
        const auto &rect = rects[i];
        if (options.anchor_mode == AnchorMode::Client) {
            if (!MatchesTargetClientSize(windows[i], options)) {
                setlog(L"LayoutWindows: hwnd[%zu]=%p 客户区尺寸校验失败，期望 %ldx%ld", i, windows[i],
                       options.window_width, options.window_height);
                return 0;
            }
            if (has_initial_metrics[i] &&
                !MatchesTargetClientRect(windows[i], rect, initial_metrics[i])) {
                setlog(L"LayoutWindows: hwnd[%zu]=%p 客户区位置校验失败，期望偏移(%d,%d)", i, windows[i],
                       rect.x, rect.y);
                return 0;
            }
        } else if (!MatchesTargetSize(windows[i], rect)) {
            int width = 0;
            int height = 0;
            GetWindowSize(windows[i], width, height);
            setlog(L"LayoutWindows: hwnd[%zu]=%p 尺寸校验失败，期望 %dx%d 实际 %dx%d", i, windows[i],
                   rect.width, rect.height, width, height);
            return 0;
        }
    }

    return 1;
}

long Layout(const std::vector<HWND> &windows, const Options &options) {
    if (windows.empty()) {
        return 0;
    }

    // 统一大小模式下，窗口尺寸不能小于最小阈值。
    if (!IsValidUniformSize(options)) {
        setlog(L"LayoutWindows: 统一大小模式客户区尺寸 %ldx%ld 小于最小阈值 %dx%d", options.window_width,
               options.window_height, kMinWindowWidth, kMinWindowHeight);
        return 0;
    }

    const auto rects = Calculate(windows, options);
    return Apply(windows, rects, options);
}

} // namespace op::window_layout
