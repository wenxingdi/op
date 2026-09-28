#pragma once

// 寻路/路径工具集：障碍位图地图状态 + LOS + RDP 抽稀 + 点串解析 + 多边形判定。
// 坐标约定：世界坐标（可为负，含 offset）<-> 网格坐标（[0,grid_w) x [0,grid_h)）。
//   grid = (world - offset) / scale（floor 除）；world = offset + grid*scale + scale/2（格中心）。
// 障碍判定：32bpp BGRA 像素亮度 (r+g+b)/3 < 128 = 障碍；降采样时 scale*scale 块内
//   任一障碍像素 -> 整格障碍（保守策略，宁可绕路不穿墙）。

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace op::pathtools {

struct AStarMapState {
    int scale = 1;
    int offset_x = 0;
    int offset_y = 0;
    int grid_w = 0;
    int grid_h = 0;
    std::vector<uint8_t> blocked; // grid_w*grid_h，1=障碍

    bool valid() const {
        return grid_w > 0 && grid_h > 0 && blocked.size() == static_cast<size_t>(grid_w) * grid_h;
    }

    void clear() {
        scale = 1;
        offset_x = offset_y = 0;
        grid_w = grid_h = 0;
        blocked.clear();
    }

    bool in_grid(int gx, int gy) const {
        return gx >= 0 && gy >= 0 && gx < grid_w && gy < grid_h;
    }

    bool is_blocked(int gx, int gy) const {
        if (!in_grid(gx, gy))
            return true; // 界外视为墙
        return blocked[static_cast<size_t>(gy) * grid_w + gx] != 0;
    }

    int world_to_grid_x(long wx) const {
        return static_cast<int>(std::floor((wx - offset_x) / static_cast<double>(scale)));
    }

    int world_to_grid_y(long wy) const {
        return static_cast<int>(std::floor((wy - offset_y) / static_cast<double>(scale)));
    }

    long grid_to_world_x(int gx) const {
        return offset_x + static_cast<long>(gx) * scale + scale / 2;
    }

    long grid_to_world_y(int gy) const {
        return offset_y + static_cast<long>(gy) * scale + scale / 2;
    }
};

// 从 32bpp BGRA 位图构建降采样障碍网格。scale<1 视为 1；scale>64 视为 64。
// 位图尺寸必须能覆盖至少 1 个网格；返回 false 表示输入非法。
inline bool build_astar_map_from_bgra(const unsigned char *bgra, int img_w, int img_h, int scale,
                                      AStarMapState &out) {
    out.clear();
    if (!bgra || img_w <= 0 || img_h <= 0)
        return false;
    if (scale < 1)
        scale = 1;
    if (scale > 64)
        scale = 64;

    const int grid_w = (img_w + scale - 1) / scale;
    const int grid_h = (img_h + scale - 1) / scale;
    const size_t cells = static_cast<size_t>(grid_w) * grid_h;
    if (cells == 0 || cells > (64ull << 20))
        return false; // 与 AStar::kMaxCells 对齐

    out.scale = scale;
    out.grid_w = grid_w;
    out.grid_h = grid_h;
    out.blocked.assign(cells, 0);

    for (int y = 0; y < img_h; ++y) {
        const int gy = y / scale;
        const unsigned char *row = bgra + static_cast<size_t>(y) * img_w * 4;
        for (int x = 0; x < img_w; ++x) {
            const unsigned char b = row[static_cast<size_t>(x) * 4 + 0];
            const unsigned char g = row[static_cast<size_t>(x) * 4 + 1];
            const unsigned char r = row[static_cast<size_t>(x) * 4 + 2];
            const int luminance = (b + g + r) / 3;
            if (luminance < 128)
                out.blocked[static_cast<size_t>(gy) * grid_w + (x / scale)] = 1;
        }
    }
    return true;
}

// 网格两点的 Bresenham 视线判定：路径上任一格为障碍则不可视。两端点各自判定。
inline bool line_of_sight(const AStarMapState &m, int gx1, int gy1, int gx2, int gy2) {
    if (m.is_blocked(gx1, gy1) || m.is_blocked(gx2, gy2))
        return false;
    int x = gx1, y = gy1;
    const int dx = std::abs(gx2 - gx1), sx = gx1 < gx2 ? 1 : -1;
    const int dy = -std::abs(gy2 - gy1), sy = gy1 < gy2 ? 1 : -1;
    int err = dx + dy;
    while (true) {
        if (m.is_blocked(x, y))
            return false;
        if (x == gx2 && y == gy2)
            return true;
        const int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y += sy;
        }
    }
}

using PathPoint = std::pair<double, double>;

// 解析 "x,y|x,y|..." 点串。任一段非法则整体失败（返回 false），与 AStarFindPath 的容错语义不同：
// 路径串是我们自己生成的，出现非法段说明调用方传错数据，静默截断更容易掩盖问题。
inline bool parse_points(const std::wstring &s, std::vector<PathPoint> &out) {
    out.clear();
    size_t begin = 0;
    while (begin <= s.size()) {
        const size_t end = s.find(L'|', begin);
        const size_t stop = end == std::wstring::npos ? s.size() : end;
        if (stop > begin) {
            const size_t comma = s.find(L',', begin);
            if (comma == std::wstring::npos || comma >= stop)
                return false;
            try {
                const double x = std::stod(s.substr(begin, comma - begin));
                const double y = std::stod(s.substr(comma + 1, stop - comma - 1));
                out.emplace_back(x, y);
            } catch (...) {
                return false;
            }
        }
        if (end == std::wstring::npos)
            break;
        begin = end + 1;
    }
    return !out.empty();
}

inline std::wstring points_to_string(const std::vector<PathPoint> &pts, bool integral = true) {
    std::wstring s;
    wchar_t buf[64];
    for (size_t i = 0; i < pts.size(); ++i) {
        if (i)
            s.push_back(L'|');
        if (integral)
            swprintf(buf, 64, L"%.0f,%.0f", pts[i].first, pts[i].second);
        else
            swprintf(buf, 64, L"%.2f,%.2f", pts[i].first, pts[i].second);
        s += buf;
    }
    return s;
}

// Ramer-Douglas-Peucker 抽稀。epsilon<=0 时原样返回。
inline void rdp_simplify(const std::vector<PathPoint> &pts, double epsilon, std::vector<PathPoint> &out) {
    out.clear();
    if (pts.size() <= 2 || epsilon <= 0) {
        out = pts;
        return;
    }
    const auto perp_dist = [](const PathPoint &p, const PathPoint &a, const PathPoint &b) {
        const double dx = b.first - a.first, dy = b.second - a.second;
        const double len2 = dx * dx + dy * dy;
        if (len2 == 0)
            return std::hypot(p.first - a.first, p.second - a.second);
        const double t = ((p.first - a.first) * dx + (p.second - a.second) * dy) / len2;
        const double cx = a.first + t * dx, cy = a.second + t * dy;
        return std::hypot(p.first - cx, p.second - cy);
    };
    // 递归深度受点数限制，改显式栈避免极端长路径栈溢出。
    struct Range {
        size_t first, last;
    };
    std::vector<Range> stack;
    std::vector<bool> keep(pts.size(), false);
    keep[0] = keep[pts.size() - 1] = true;
    stack.push_back({0, pts.size() - 1});
    while (!stack.empty()) {
        const auto [first, last] = stack.back();
        stack.pop_back();
        if (last <= first + 1)
            continue;
        double max_dist = 0;
        size_t max_idx = first;
        for (size_t i = first + 1; i < last; ++i) {
            const double d = perp_dist(pts[i], pts[first], pts[last]);
            if (d > max_dist) {
                max_dist = d;
                max_idx = i;
            }
        }
        if (max_dist > epsilon) {
            keep[max_idx] = true;
            stack.push_back({first, max_idx});
            stack.push_back({max_idx, last});
        }
    }
    for (size_t i = 0; i < pts.size(); ++i) {
        if (keep[i])
            out.push_back(pts[i]);
    }
}

// 射线法点在多边形判定（even-odd）。顶点数 <3 返回 false。
inline bool point_in_polygon(double px, double py, const std::vector<PathPoint> &poly) {
    if (poly.size() < 3)
        return false;
    bool inside = false;
    const size_t n = poly.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const double xi = poly[i].first, yi = poly[i].second;
        const double xj = poly[j].first, yj = poly[j].second;
        if (((yi > py) != (yj > py)) &&
            (px < (xj - xi) * (py - yi) / (yj - yi + 1e-12) + xi)) {
            inside = !inside;
        }
    }
    return inside;
}

} // namespace op::pathtools
