#pragma once

#include <cmath>
#include <limits>
#include <list>
#include <queue>
#include <vector>

namespace op {

class AStar {
  public:
    struct Vec2i {
        int x = 0;
        int y = 0;

        bool operator==(const Vec2i &rhs) const {
            return x == rhs.x && y == rhs.y;
        }

        Vec2i operator+(const Vec2i &rhs) const {
            return {x + rhs.x, y + rhs.y};
        }
    };

    // 单张地图的格子数上限（64M 格，约 576MB 临时数组）。超出后 set_map 拒绝，findpath 返回空路径。
    static constexpr size_t kMaxCells = 64ull * 1024 * 1024;

    // 有效地图范围是 [0, w) x [0, h)，越界障碍点会被忽略。
    // 返回 false 表示尺寸非法或超出 kMaxCells（此时 _walls 置空 = 全图视为墙，findpath 一律返回空路径）。
    bool set_map(int w, int h, const std::vector<Vec2i> &wall) {
        size_t cells = 0;
        if (!prepare_map(w, h, cells))
            return false;
        _walls.assign(cells, false);

        for (const auto &pos : wall) {
            if (!outside(pos)) {
                _walls[index(pos)] = true;
            }
        }
        return true;
    }

    // 从降采样障碍网格直接建图（1=墙），免展开墙点 vector。尺寸上限同 set_map。
    bool set_map_grid(int w, int h, const std::vector<uint8_t> &blocked) {
        size_t cells = 0;
        if (!prepare_map(w, h, cells))
            return false;
        if (blocked.size() != cells) {
            // 与原版语义对齐：参数非法同样清墙（全图视为墙），而不是保留旧图。
            _walls.clear();
            return false;
        }
        _walls.resize(cells);
        for (size_t i = 0; i < cells; ++i)
            _walls[i] = blocked[i] != 0;
        return true;
    }

    // 保持现有输出顺序：终点 -> 起点。
    void findpath(int beginX, int beginY, int endX, int endY, std::list<Vec2i> &path) {
        path.clear();

        const Vec2i start{beginX, beginY};
        const Vec2i end{endX, endY};
        if (outside(start) || outside(end) || blocked(start) || blocked(end)) {
            return;
        }

        // 每个格子的状态按 y * width + x 映射到一维数组。
        const int total = cell_count();
        std::vector<int> g_score(total, kUnreached);
        std::vector<int> parents(total, -1);

        // 优先扩展预估总代价最低的节点。
        std::priority_queue<OpenNode, std::vector<OpenNode>, OpenNodeGreater> open;
        const int start_index = index(start);
        const int end_index = index(end);
        g_score[start_index] = 0;
        open.push({start, heuristic(start, end), 0});

        while (!open.empty()) {
            const OpenNode current = open.top();
            open.pop();

            const int current_index = index(current.pos);
            // AL2: 惰性删除取代 closed[] —— 本启发式（八方向等代价 + Chebyshev）
            // 是 consistent 的（每移一步 h 恰降 1 = 代价 1），节点首次弹出即最优，
            // 队列里残留的同一节点旧条目 g 必然更大，按陈旧跳过即可。少一份
            // vector<bool> 辅助数组（64M 格上限时即 8MB 分配+位清扫）。
            if (current.g > g_score[current_index]) {
                continue;
            }

            if (current_index == end_index) {
                build_path(end_index, parents, path);
                return;
            }

            for (const auto &dir : kDirections) {
                const Vec2i next = current.pos + dir;
                if (outside(next) || blocked(next)) {
                    continue;
                }
                // 禁止对角移动穿越墙角：相邻两正交格任一为墙则不允许斜穿。
                if (dir.x != 0 && dir.y != 0) {
                    if (blocked({current.pos.x + dir.x, current.pos.y}) ||
                        blocked({current.pos.x, current.pos.y + dir.y})) {
                        continue;
                    }
                }

                const int next_index = index(next);
                // 已弹出（closed）的节点最优 g 已定，新路径 g 不可能更小
                // （consistency 保证），下面的 >= 判据天然涵盖，无需 closed 检查。
                const int next_g = current.g + 1;
                if (next_g >= g_score[next_index]) {
                    continue;
                }

                g_score[next_index] = next_g;
                parents[next_index] = current_index;
                open.push({next, next_g + heuristic(next, end), next_g});
            }
        }
    }

  private:
    struct OpenNode {
        Vec2i pos;
        int f = 0;
        int g = 0;
    };

    struct OpenNodeGreater {
        bool operator()(const OpenNode &lhs, const OpenNode &rhs) const {
            if (lhs.f != rhs.f) {
                return lhs.f > rhs.f;
            }
            return lhs.g < rhs.g;
        }
    };

    static constexpr int kUnreached = (std::numeric_limits<int>::max)();
    // 八方向移动，每步代价相同。
    static constexpr Vec2i kDirections[8] = {
        {0, 1},
        {0, -1},
        {-1, 0},
        {1, 0},
        {1, 1},
        {-1, 1},
        {-1, -1},
        {1, -1},
    };

    // AL3: set_map/set_map_grid 同构前导——置尺寸、算 cells、判非法。
    // 返回 false 表示尺寸非法或超 kMaxCells（_walls 已清 = 全图视为墙）。
    bool prepare_map(int w, int h, size_t &cells) {
        _mapSize = {w, h};
        cells = static_cast<size_t>(w) * static_cast<size_t>(h);
        if (w <= 0 || h <= 0 || cells == 0 || cells > kMaxCells) {
            _walls.clear();
            return false;
        }
        return true;
    }

    bool outside(Vec2i pos) const {
        return pos.x < 0 || pos.y < 0 || pos.x >= _mapSize.x || pos.y >= _mapSize.y;
    }

    bool blocked(Vec2i pos) const {
        if (outside(pos)) {
            return true;
        }
        return _walls.empty() || _walls[index(pos)];
    }

    int cell_count() const {
        if (_mapSize.x <= 0 || _mapSize.y <= 0) {
            return 0;
        }
        return _mapSize.x * _mapSize.y;
    }

    int index(Vec2i pos) const {
        return pos.y * _mapSize.x + pos.x;
    }

    // 八方向等代价移动适合使用切比雪夫距离。
    int heuristic(Vec2i from, Vec2i to) const {
        const int dx = std::abs(from.x - to.x);
        const int dy = std::abs(from.y - to.y);
        return dx > dy ? dx : dy;
    }

    // 命中终点后，通过父节点索引回溯路径。
    void build_path(int end_index, const std::vector<int> &parents, std::list<Vec2i> &path) const {
        for (int current = end_index; current >= 0; current = parents[current]) {
            path.push_back({current % _mapSize.x, current / _mapSize.x});
        }
    }

    Vec2i _mapSize;
    std::vector<bool> _walls;
};

} // namespace op
