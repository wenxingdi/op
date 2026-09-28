#include "OpContext.h"
#include "OpResult.h"

#include "algorithm/AStar.h"
#include "base/Utils.h"
#include "image/Image.h"

#include <libop.h>

#include <cmath>
#include <cwchar>
#include <list>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

void op::Op::AStarFindPath(long mapWidth, long mapHeight, const wchar_t *disable_points, long beginX, long beginY,
                          long endX, long endY, std::wstring &path) {
    path.clear();
    if (mapWidth <= 0 || mapHeight <= 0) {
        setlog(L"AStarFindPath: 非法地图尺寸 %ldx%ld，返回空路径", mapWidth, mapHeight);
        return;
    }
    AStar as;
    using Vec2i = AStar::Vec2i;
    std::vector<Vec2i> walls;
    std::vector<wstring> vstr;
    Vec2i tp;
    split(disable_points, vstr, L"|");
    for (auto &it : vstr) {
        if (swscanf(it.c_str(), L"%d,%d", &tp.x, &tp.y) != 2) {
            setlog(L"AStarFindPath: disable_points 存在非法项 \"%s\"，该项及其后的点已全部忽略", it.c_str());
            break;
        }
        walls.push_back(tp);
    }
    std::list<Vec2i> paths;

    if (!as.set_map(mapWidth, mapHeight, walls)) {
        setlog(L"AStarFindPath: 地图尺寸 %ldx%ld 超出单图上限，拒绝寻路", mapWidth, mapHeight);
        return;
    }
    as.findpath(beginX, beginY, endX, endY, paths);
    for (auto it = paths.rbegin(); it != paths.rend(); ++it) {
        auto v = *it;
        path += std::to_wstring(v.x);
        path.push_back(L',');
        path += std::to_wstring(v.y);
        path.push_back(L'|');
    }
    if (!path.empty())
        path.pop_back();
}

void op::Op::FindNearestPos(const wchar_t *all_pos, long type, long x, long y, std::wstring &ret) {
    double old = 1e9;
    long rx = -1, ry = -1;
    std::wstring best_name;
    std::wstring s = std::regex_replace(all_pos, std::wregex(L","), L" ");
    std::vector<std::wstring> items;
    split(s, items, L"|");
    for (const auto &item : items) {
        long x2, y2;
        bool ok = false;
        std::wstring name;
        std::wistringstream iss(item);
        if (type == 1) {
            if (iss >> x2 >> y2) {
                ok = true;
            }
        } else {
            if (iss >> name >> x2 >> y2) {
                ok = true;
            }
        }
        if (ok) {
            double compareDis = (x - x2) * (x - x2) + (y - y2) * (y - y2);
            if (compareDis < old) {
                rx = x2;
                ry = y2;
                old = compareDis;
                best_name = name;
            }
        }
    }
    if (!best_name.empty()) {
        ret = best_name + L"," + std::to_wstring(rx) + L"," + std::to_wstring(ry);
    } else if (type == 1 && rx != -1) {
        ret = std::to_wstring(rx) + L"," + std::to_wstring(ry);
    } else {
        ret.clear();
    }
}

namespace {

using op::pathtools::AStarMapState;
using op::pathtools::PathPoint;
using AStar = op::AStar;

// 内部：在会话地图上跑一段 A*，返回世界坐标路径（起点->终点）。失败返回空。
std::vector<PathPoint> astar_segment(const AStarMapState &map, long bx, long by, long ex, long ey) {
    std::vector<PathPoint> result;
    const int gx1 = map.world_to_grid_x(bx);
    const int gy1 = map.world_to_grid_y(by);
    const int gx2 = map.world_to_grid_x(ex);
    const int gy2 = map.world_to_grid_y(ey);
    if (!map.in_grid(gx1, gy1) || !map.in_grid(gx2, gy2)) {
        setlog(L"AStar: 起点或终点 (%ld,%ld)->(%ld,%ld) 超出障碍地图范围", bx, by, ex, ey);
        return result;
    }
    if (map.is_blocked(gx1, gy1) || map.is_blocked(gx2, gy2)) {
        setlog(L"AStar: 起点或终点落在障碍格上 (%ld,%ld)->(%ld,%ld)", bx, by, ex, ey);
        return result;
    }

    AStar as;
    std::list<AStar::Vec2i> paths;
    if (!as.set_map_grid(map.grid_w, map.grid_h, map.blocked)) {
        setlog(L"AStar: 障碍网格 %dx%d 建图失败", map.grid_w, map.grid_h);
        return result;
    }
    as.findpath(gx1, gy1, gx2, gy2, paths);
    for (auto it = paths.rbegin(); it != paths.rend(); ++it) {
        const long wx = map.grid_to_world_x(it->x);
        const long wy = map.grid_to_world_y(it->y);
        result.push_back(PathPoint{static_cast<double>(wx), static_cast<double>(wy)});
    }
    return result;
}

} // namespace

// 设置寻路障碍地图（文件版）。bitmap_file 为空串时清空当前地图。
void op::Op::SetAStarMap(const wchar_t *bitmap_file, long scale, long offset_x, long offset_y, long *ret) {
    internal::set_result(ret, 0L);
    std::wstring file = bitmap_file ? bitmap_file : L"";
    if (file.empty()) {
        m_context->astar_map.clear();
        internal::set_result(ret, 1L);
        return;
    }
    std::wstring full;
    if (!Path2GlobalPath(file, m_context->curr_path, full)) {
        setlog(L"SetAStarMap: 路径解析失败 \"%s\"", file.c_str());
        return;
    }
    Image img;
    if (!img.read(full.c_str()) || img.empty()) {
        setlog(L"SetAStarMap: 位图读取失败 \"%s\"", full.c_str());
        return;
    }
    if (!pathtools::build_astar_map_from_bgra(img.pdata, img.width, img.height, static_cast<int>(scale),
                                              m_context->astar_map)) {
        setlog(L"SetAStarMap: 障碍网格构建失败（位图 %dx%d scale=%ld）", img.width, img.height, scale);
        return;
    }
    m_context->astar_map.offset_x = static_cast<int>(offset_x);
    m_context->astar_map.offset_y = static_cast<int>(offset_y);
    internal::set_result(ret, 1L);
}

// 内存版障碍地图（32bpp BGRA，top-down）。C API 专用。
void op::Op::SetAStarMapData(long width, long height, void *bgra_data, long size, long scale, long offset_x,
                             long offset_y, long *ret) {
    internal::set_result(ret, 0L);
    if (!bgra_data || size <= 0) {
        setlog(L"SetAStarMapData: 非法输入");
        return;
    }
    const size_t need = static_cast<size_t>(width) * height * 4;
    if (width <= 0 || height <= 0 || static_cast<size_t>(size) < need) {
        setlog(L"SetAStarMapData: 尺寸不符（%ldx%ld 需 %zu 字节，传入 %ld）", width, height, need, size);
        return;
    }
    if (!pathtools::build_astar_map_from_bgra(static_cast<const unsigned char *>(bgra_data), width, height,
                                              static_cast<int>(scale), m_context->astar_map)) {
        setlog(L"SetAStarMapData: 障碍网格构建失败（%ldx%ld scale=%ld）", width, height, scale);
        return;
    }
    m_context->astar_map.offset_x = static_cast<int>(offset_x);
    m_context->astar_map.offset_y = static_cast<int>(offset_y);
    internal::set_result(ret, 1L);
}

// 障碍位图 A*：用会话地图寻路，输出世界坐标路径 "x,y|x,y"（起点->终点，格中心）。
void op::Op::AStarFindPathBM(long beginX, long beginY, long endX, long endY, std::wstring &path) {
    path.clear();
    if (!m_context->astar_map.valid()) {
        setlog(L"AStarFindPathBM: 未设置障碍地图，请先 SetAStarMap/SetAStarMapData");
        return;
    }
    auto pts = astar_segment(m_context->astar_map, beginX, beginY, endX, endY);
    if (pts.empty())
        return;
    path = pathtools::points_to_string(pts);
}

// 多点途经寻路："x,y|x,y|..." 首点为起点，逐段寻路拼接（衔接点去重）。任一段失败整体返回空。
void op::Op::AStarFindPathWay(const wchar_t *points, std::wstring &path) {
    path.clear();
    if (!m_context->astar_map.valid()) {
        setlog(L"AStarFindPathWay: 未设置障碍地图，请先 SetAStarMap/SetAStarMapData");
        return;
    }
    std::vector<PathPoint> waypoints;
    if (!pathtools::parse_points(points ? points : L"", waypoints) || waypoints.size() < 2) {
        setlog(L"AStarFindPathWay: 途经点串非法或不足 2 点 \"%s\"", points ? points : L"");
        return;
    }

    std::vector<PathPoint> full;
    for (size_t i = 1; i < waypoints.size(); ++i) {
        auto seg = astar_segment(m_context->astar_map, static_cast<long>(waypoints[i - 1].first),
                                 static_cast<long>(waypoints[i - 1].second), static_cast<long>(waypoints[i].first),
                                 static_cast<long>(waypoints[i].second));
        if (seg.empty()) {
            setlog(L"AStarFindPathWay: 第 %zu 段 (%.0f,%.0f)->(%.0f,%.0f) 不可达", i, waypoints[i - 1].first,
                   waypoints[i - 1].second, waypoints[i].first, waypoints[i].second);
            return;
        }
        if (!full.empty() && !seg.empty())
            full.pop_back(); // 去掉与上一段重复的衔接点
        full.insert(full.end(), seg.begin(), seg.end());
    }
    path = pathtools::points_to_string(full);
}

// 视线拉直：相邻可直视的路径点连线合并（贪心最远可见跳点），路径更自然、点击不抖。
// 地图未设置时原样返回（不修改）。保留输入点精度。
void op::Op::SmoothPathByLOS(const wchar_t *path, std::wstring &out) {
    std::vector<PathPoint> pts;
    if (!pathtools::parse_points(path ? path : L"", pts) || pts.size() <= 2 || !m_context->astar_map.valid()) {
        if (!m_context->astar_map.valid())
            setlog(L"SmoothPathByLOS: 未设置障碍地图，原样返回");
        out = path ? path : L"";
        return;
    }

    std::vector<PathPoint> result;
    size_t i = 0;
    result.push_back(pts.front());
    while (i < pts.size() - 1) {
        // 从最远的候选点往回找第一个可直视的。
        size_t reach = i + 1;
        for (size_t j = pts.size() - 1; j > i; --j) {
            if (pathtools::line_of_sight(m_context->astar_map, m_context->astar_map.world_to_grid_x(
                                                                   static_cast<long>(pts[i].first)),
                                         m_context->astar_map.world_to_grid_y(static_cast<long>(pts[i].second)),
                                         m_context->astar_map.world_to_grid_x(static_cast<long>(pts[j].first)),
                                         m_context->astar_map.world_to_grid_y(static_cast<long>(pts[j].second)))) {
                reach = j;
                break;
            }
        }
        result.push_back(pts[reach]);
        i = reach;
    }
    out = pathtools::points_to_string(result);
}

// RDP 抽稀："x,y|..." -> 拐点数大幅减少，epsilon 为最大垂直偏差（世界坐标单位）。
void op::Op::SimplifyPath(const wchar_t *path, double epsilon, std::wstring &out) {
    std::vector<PathPoint> pts;
    if (!pathtools::parse_points(path ? path : L"", pts)) {
        out.clear();
        return;
    }
    std::vector<PathPoint> simplified;
    pathtools::rdp_simplify(pts, epsilon, simplified);
    out = pathtools::points_to_string(simplified);
}

// 两点视线遮挡判定（世界坐标）：**三态** 1=被挡 0=通 -1=未设置地图。
// -1 不是"负错误码泄漏"：调用方按「非 0 = 走 A* 绕路」判定，-1 与 1 行为等价，
// 且 -1 保留了"忘记 SetAStarMap"的可诊断信息。既有用例
// AlgorithmTest.AStarBMUnreachableOrNoMapReturnsEmpty 钉住此值，**勿改为 0**
// （改 0 会让未设图被误判为"畅通"，直线撞墙）。
void op::Op::IsLineBlocked(long x1, long y1, long x2, long y2, long *ret) {
    if (!m_context->astar_map.valid()) {
        setlog(L"IsLineBlocked: 未设置障碍地图，请先 SetAStarMap/SetAStarMapData");
        internal::set_result(ret, -1L);
        return;
    }
    const bool sight = pathtools::line_of_sight(m_context->astar_map, m_context->astar_map.world_to_grid_x(x1),
                                                m_context->astar_map.world_to_grid_y(y1),
                                                m_context->astar_map.world_to_grid_x(x2),
                                                m_context->astar_map.world_to_grid_y(y2));
    internal::set_result(ret, sight ? 0L : 1L);
}

// 点到路径最近点：输出 0 基索引与最近点坐标。path 非法返回 0。
void op::Op::FindNearestPathPoint(const wchar_t *path, long x, long y, long *index, long *nx, long *ny, long *ret) {
    internal::set_result(ret, 0L);
    internal::set_result(index, 0L);
    internal::set_result(nx, 0L);
    internal::set_result(ny, 0L);
    std::vector<PathPoint> pts;
    if (!pathtools::parse_points(path ? path : L"", pts))
        return;
    double best = 1e18;
    size_t best_idx = 0;
    for (size_t i = 0; i < pts.size(); ++i) {
        const double dx = pts[i].first - x;
        const double dy = pts[i].second - y;
        const double d2 = dx * dx + dy * dy;
        if (d2 < best) {
            best = d2;
            best_idx = i;
        }
    }
    internal::set_result(ret, 1L);
    internal::set_result(index, static_cast<long>(best_idx));
    internal::set_result(nx, static_cast<long>(std::lround(pts[best_idx].first)));
    internal::set_result(ny, static_cast<long>(std::lround(pts[best_idx].second)));
}

// 点在多边形内判定（射线法 even-odd）。point="x,y"，polygon="x,y|x,y|..."（>=3 顶点）。
void op::Op::PointInPolygon(const wchar_t *point, const wchar_t *polygon, long *ret) {
    internal::set_result(ret, 0L);
    std::vector<PathPoint> pt, poly;
    if (!pathtools::parse_points(point ? point : L"", pt) || pt.size() != 1) {
        setlog(L"PointInPolygon: point 非法 \"%s\"", point ? point : L"");
        return;
    }
    if (!pathtools::parse_points(polygon ? polygon : L"", poly) || poly.size() < 3) {
        setlog(L"PointInPolygon: polygon 非法或顶点不足 3 \"%s\"", polygon ? polygon : L"");
        return;
    }
    internal::set_result(ret, pathtools::point_in_polygon(pt[0].first, pt[0].second, poly) ? 1L : 0L);
}

