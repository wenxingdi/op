// algorithm 模块自测：AStarFindPath / FindNearestPos 均为纯计算，免窗口/免注入，本进程内直接验证。
// 路径用例的期望值均按 AStar.h 的确定性行为推演（切比雪夫启发式 + f 相同按大 g 优先 + 方向序），
// 不依赖优先队列对等价节点的弹出顺序。

#include <gtest/gtest.h>

#include <op_c_api.h>

#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace {

struct CApiHandle {
    op_handle handle = OpCreate();
    ~CApiHandle() {
        OpDestroy(handle);
    }
};

std::vector<std::wstring> SplitPipe(const std::wstring &s) {
    std::vector<std::wstring> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        const size_t cut = s.find(L'|', pos);
        if (cut == std::wstring::npos) {
            out.push_back(s.substr(pos));
            break;
        }
        out.push_back(s.substr(pos, cut - pos));
        pos = cut + 1;
    }
    return out;
}

// ---- AStarFindPath ----

TEST(AlgorithmTest, OneCellMapStartEqualsEnd) {
    CApiHandle api;
    EXPECT_STREQ(OpAStarFindPath(api.handle, 1, 1, L"", 0, 0, 0, 0), L"0,0");
}

TEST(AlgorithmTest, OpenFieldDiagonalTakesDirectStep) {
    CApiHandle api;
    // 对角空位时 f=1 的斜步优先弹出，直接命中终点。
    EXPECT_STREQ(OpAStarFindPath(api.handle, 2, 2, L"", 0, 0, 1, 1), L"0,0|1,1");
}

TEST(AlgorithmTest, CorridorForcesThroughCenter) {
    CApiHandle api;
    // 3x3，(1,0)/(1,2) 为墙，唯一通道经过中心。
    EXPECT_STREQ(OpAStarFindPath(api.handle, 3, 3, L"1,0|1,2", 0, 1, 2, 1), L"0,1|1,1|2,1");
}

TEST(AlgorithmTest, CornerCutPreventionBlocksDiagonal) {
    CApiHandle api;
    // 两个正交邻格均为墙时禁止斜穿 → 无路可达。
    EXPECT_STREQ(OpAStarFindPath(api.handle, 2, 2, L"1,0|0,1", 0, 0, 1, 1), L"");
}

TEST(AlgorithmTest, PartialWallDivertToFreeSide) {
    CApiHandle api;
    // 斜穿被 (1,0) 挡住，改走 (0,1) 一侧。
    EXPECT_STREQ(OpAStarFindPath(api.handle, 2, 2, L"1,0", 0, 0, 1, 1), L"0,0|0,1|1,1");
}

TEST(AlgorithmTest, MalformedFirstEntryParsesNoWalls) {
    CApiHandle api;
    // 首段即非法 → 整个列表被忽略（break 语义），等价于无障碍地图。
    EXPECT_STREQ(OpAStarFindPath(api.handle, 2, 2, L"abc|1,0|0,1", 0, 0, 1, 1), L"0,0|1,1");
}

TEST(AlgorithmTest, MalformedMiddleEntryTruncatesTail) {
    CApiHandle api;
    // "1,0" 已入墙，"xx" 处 break → 其后的 "0,1" 不再解析（仍留有空侧可走）。
    EXPECT_STREQ(OpAStarFindPath(api.handle, 2, 2, L"1,0|xx|0,1", 0, 0, 1, 1), L"0,0|0,1|1,1");
}

TEST(AlgorithmTest, OutOfRangeWallsIgnored) {
    CApiHandle api;
    EXPECT_STREQ(OpAStarFindPath(api.handle, 2, 2, L"5,5|-1,0", 0, 0, 1, 1), L"0,0|1,1");
}

TEST(AlgorithmTest, InvalidMapSizeReturnsEmpty) {
    CApiHandle api;
    EXPECT_STREQ(OpAStarFindPath(api.handle, 0, 10, L"", 0, 0, 1, 1), L"");
    EXPECT_STREQ(OpAStarFindPath(api.handle, -3, 5, L"", 0, 0, 1, 1), L"");
}

TEST(AlgorithmTest, OversizedMapRejectedWithoutCrash) {
    CApiHandle api;
    // 1e10 格远超 64M 上限：拒绝寻路返回空，且不分配内存（毫秒级返回）。
    EXPECT_STREQ(OpAStarFindPath(api.handle, 100000, 100000, L"", 0, 0, 1, 1), L"");
}

TEST(AlgorithmTest, FullyWalledStartReturnsEmpty) {
    CApiHandle api;
    // 起点被三面包围 → 不可达返回空串（合法结果，不视为错误）。
    EXPECT_STREQ(OpAStarFindPath(api.handle, 3, 3, L"1,0|0,1|1,1", 0, 0, 2, 2), L"");
}

TEST(AlgorithmTest, OpenFieldLongPathEndpointsAndLength) {
    CApiHandle api;
    const std::wstring path = OpAStarFindPath(api.handle, 20, 20, L"", 0, 0, 9, 0);
    const std::vector<std::wstring> pts = SplitPipe(path);
    // 最短 9 步 = 10 个点；开放场tie序不保证形状，只钉端点与点数。
    ASSERT_EQ(pts.size(), 10u);
    EXPECT_EQ(pts.front(), L"0,0");
    EXPECT_EQ(pts.back(), L"9,0");
}

// ---- FindNearestPos ----

TEST(AlgorithmTest, NearestType1ReturnsBarePair) {
    CApiHandle api;
    EXPECT_STREQ(OpFindNearestPos(api.handle, L"10,10|20,20|30,30", 1, 12, 9), L"10,10");
}

TEST(AlgorithmTest, NearestTieKeepsFirstOccurrence) {
    CApiHandle api;
    // (0,0) 到两点等距 → 严格小于比较保留先出现者。
    EXPECT_STREQ(OpFindNearestPos(api.handle, L"10,0|0,10", 1, 0, 0), L"10,0");
}

TEST(AlgorithmTest, NearestType2ReturnsNameAndPos) {
    CApiHandle api;
    EXPECT_STREQ(OpFindNearestPos(api.handle, L"药店,10,10|铁匠,20,20", 2, 18, 19), L"铁匠,20,20");
}

TEST(AlgorithmTest, NearestType2SkipsBarePair) {
    CApiHandle api;
    // type=2 时裸 "x,y" 只有两词元，第三个解析失败 → 跳过该项。
    EXPECT_STREQ(OpFindNearestPos(api.handle, L"10,10|name,20,20", 2, 0, 0), L"name,20,20");
}

TEST(AlgorithmTest, NearestEmptyOrAllInvalidReturnsEmpty) {
    CApiHandle api;
    EXPECT_STREQ(OpFindNearestPos(api.handle, L"", 1, 0, 0), L"");
    EXPECT_STREQ(OpFindNearestPos(api.handle, L"abc|def", 1, 0, 0), L"");
}

// ---- 寻路工具组：障碍位图 A* / LOS / RDP / 途经 / 多边形 ----
// 16x16 白底地图，一堵竖墙 x=8, y∈[0,10]：(2,8)->(14,8) 必须从墙底 y>=11 绕行。

namespace {

std::vector<unsigned char> MakeMapBgra(int w, int h, const std::vector<std::pair<int, int>> &wall_cells) {
    std::vector<unsigned char> buf(static_cast<size_t>(w) * h * 4, 0xff);
    for (const auto &cell : wall_cells) {
        if (cell.first < 0 || cell.first >= w || cell.second < 0 || cell.second >= h)
            continue;
        const auto idx = (static_cast<size_t>(cell.second) * w + cell.first) * 4;
        buf[idx + 0] = buf[idx + 1] = buf[idx + 2] = 0x00;
        buf[idx + 3] = 0xff;
    }
    return buf;
}

std::vector<std::pair<int, int>> VerticalWall(int x, int y_from, int y_to) {
    std::vector<std::pair<int, int>> cells;
    for (int y = y_from; y <= y_to; ++y)
        cells.emplace_back(x, y);
    return cells;
}

// 反向验证判据：路径任一点落在墙格（x==8 && y<=10）即说明绕障失败，必挂。
bool PathTouchesWall(const wchar_t *path, int scale, int ox, int oy) {
    long px = 0, py = 0;
    for (const auto &seg : SplitPipe(path)) {
        if (swscanf(seg.c_str(), L"%ld,%ld", &px, &py) != 2)
            return true;
        const int gx = static_cast<int>(std::floor((px - ox) / static_cast<double>(scale)));
        const int gy = static_cast<int>(std::floor((py - oy) / static_cast<double>(scale)));
        if (gx == 8 && gy <= 10)
            return true;
    }
    return false;
}

} // namespace

TEST(AlgorithmTest, AStarBMPathAvoidsWall) {
    CApiHandle api;
    auto map = MakeMapBgra(16, 16, VerticalWall(8, 0, 10));
    ASSERT_EQ(OpSetAStarMapData(api.handle, 16, 16, map.data(), static_cast<int>(map.size()), 1, 0, 0), 1);

    const wchar_t *path = OpAStarFindPathBM(api.handle, 2, 8, 14, 8);
    EXPECT_TRUE(path && *path != L'\0');
    ASSERT_FALSE(PathTouchesWall(path, 1, 0, 0));

    // 起点终点保真（scale=1 格中心即原坐标）。
    const auto segs = SplitPipe(path);
    EXPECT_EQ(segs.front(), L"2,8");
    EXPECT_EQ(segs.back(), L"14,8");
    // 同图直穿判定配套：墙上有遮挡，墙下方空行畅通。
    EXPECT_EQ(OpIsLineBlocked(api.handle, 2, 5, 14, 5), 1);
    EXPECT_EQ(OpIsLineBlocked(api.handle, 2, 13, 14, 13), 0);
}

TEST(AlgorithmTest, AStarBMOffsetNegativeCoordinates) {
    CApiHandle api;
    auto map = MakeMapBgra(16, 16, VerticalWall(8, 0, 10));
    ASSERT_EQ(OpSetAStarMapData(api.handle, 16, 16, map.data(), static_cast<int>(map.size()), 1, -100, -100), 1);

    const wchar_t *path = OpAStarFindPathBM(api.handle, -98, -92, -86, -92);
    EXPECT_TRUE(path && *path != L'\0');
    ASSERT_FALSE(PathTouchesWall(path, 1, -100, -100));
    const auto segs = SplitPipe(path);
    EXPECT_EQ(segs.front(), L"-98,-92");
    EXPECT_EQ(segs.back(), L"-86,-92");
}

TEST(AlgorithmTest, AStarBMScaleDownsampleKeepsObstacle) {
    CApiHandle api;
    // 32x32 世界图 scale=2 -> 16x16 网格；世界墙 x=16..17（网格 gx=8），y∈[0,21]（gy<=10）。
    std::vector<std::pair<int, int>> wall;
    for (int y = 0; y <= 21; ++y) {
        wall.emplace_back(16, y);
        wall.emplace_back(17, y);
    }
    auto map = MakeMapBgra(32, 32, wall);
    ASSERT_EQ(OpSetAStarMapData(api.handle, 32, 32, map.data(), static_cast<int>(map.size()), 2, 0, 0), 1);

    const wchar_t *path = OpAStarFindPathBM(api.handle, 4, 16, 28, 16);
    EXPECT_TRUE(path && *path != L'\0');
    // 世界坐标 -> 网格（floor 除）后不得命中墙格；若降采样障碍判定失效（漏墙）必挂。
    ASSERT_FALSE(PathTouchesWall(path, 2, 0, 0));
    // 格中心换算：起点格 (2,8) -> (5,17)，终点格 (14,8) -> (29,17)。
    const auto segs = SplitPipe(path);
    EXPECT_EQ(segs.front(), L"5,17");
    EXPECT_EQ(segs.back(), L"29,17");
}

TEST(AlgorithmTest, AStarBMUnreachableOrNoMapReturnsEmpty) {
    CApiHandle api;
    // 未设图：返回空路径，IsLineBlocked 返回 -1（判别：漏检查会返回 0/1 或崩溃）。
    EXPECT_STREQ(OpAStarFindPathBM(api.handle, 2, 8, 14, 8), L"");
    EXPECT_EQ(OpIsLineBlocked(api.handle, 2, 8, 14, 8), -1);

    auto map = MakeMapBgra(16, 16, VerticalWall(8, 0, 10));
    ASSERT_EQ(OpSetAStarMapData(api.handle, 16, 16, map.data(), static_cast<int>(map.size()), 1, 0, 0), 1);
    // 起点落在墙格上：返回空。
    EXPECT_STREQ(OpAStarFindPathBM(api.handle, 8, 5, 14, 8), L"");

    // 空文件名清空地图后回到"未设图"语义。
    EXPECT_EQ(OpSetAStarMap(api.handle, L"", 1, 0, 0), 1);
    EXPECT_EQ(OpIsLineBlocked(api.handle, 2, 8, 14, 8), -1);
}

TEST(AlgorithmTest, AStarFindPathWayChainsAndDedupsJoints) {
    CApiHandle api;
    auto map = MakeMapBgra(16, 16, VerticalWall(8, 0, 10));
    ASSERT_EQ(OpSetAStarMapData(api.handle, 16, 16, map.data(), static_cast<int>(map.size()), 1, 0, 0), 1);

    const wchar_t *path = OpAStarFindPathWay(api.handle, L"2,8|14,8|14,14");
    EXPECT_TRUE(path && *path != L'\0');
    ASSERT_FALSE(PathTouchesWall(path, 1, 0, 0));
    const auto segs = SplitPipe(path);
    EXPECT_EQ(segs.front(), L"2,8");
    EXPECT_EQ(segs.back(), L"14,14");
    // 衔接点去重：路径中 "14,8" 恰好出现一次。
    int joint_count = 0;
    for (const auto &seg : segs) {
        if (seg == L"14,8")
            ++joint_count;
    }
    EXPECT_EQ(joint_count, 1);
}

TEST(AlgorithmTest, SmoothPathByLOSShortensAndKeepsPassable) {
    CApiHandle api;
    auto map = MakeMapBgra(16, 16, VerticalWall(8, 0, 10));
    ASSERT_EQ(OpSetAStarMapData(api.handle, 16, 16, map.data(), static_cast<int>(map.size()), 1, 0, 0), 1);

    // 注意：C API 的字符串返回值指向共享缓冲，跨调用传递必须先拷贝到本地。
    std::wstring raw = OpAStarFindPathBM(api.handle, 2, 8, 14, 8);
    ASSERT_FALSE(raw.empty());
    const auto raw_count = SplitPipe(raw.c_str()).size();

    const wchar_t *smoothed = OpSmoothPathByLOS(api.handle, raw.c_str());
    ASSERT_TRUE(smoothed && *smoothed != L'\0');
    const auto smooth_segs = SplitPipe(smoothed);
    // 反向验证判据：A* 网格绕行必有可合并的锯齿段——若 LOS 实现坏掉（全不可视）则点数不减，必挂。
    EXPECT_LT(smooth_segs.size(), raw_count);
    // 拉直后每相邻段必须畅通（与 IsLineBlocked 自洽）。
    for (size_t i = 0; i + 1 < smooth_segs.size(); ++i) {
        long ax = 0, ay = 0, bx = 0, by = 0;
        ASSERT_EQ(swscanf(smooth_segs[i].c_str(), L"%ld,%ld", &ax, &ay), 2);
        ASSERT_EQ(swscanf(smooth_segs[i + 1].c_str(), L"%ld,%ld", &bx, &by), 2);
        EXPECT_EQ(OpIsLineBlocked(api.handle, ax, ay, bx, by), 0) << "seg " << i << " blocked";
    }
    // 首尾保真。
    EXPECT_EQ(smooth_segs.front(), L"2,8");
    EXPECT_EQ(smooth_segs.back(), L"14,8");
}

TEST(AlgorithmTest, SimplifyPathCollinearAndZeroEpsilon) {
    CApiHandle api;
    // 纯共线：RDP 只留首尾（判别：若抽稀实现坏掉保留中间点，必挂）。
    EXPECT_STREQ(OpSimplifyPath(api.handle, L"0,0|1,0|2,0|3,0", 0.5), L"0,0|3,0");
    // epsilon<=0 原样返回。
    EXPECT_STREQ(OpSimplifyPath(api.handle, L"0,0|1,0|2,0|3,0", 0.0), L"0,0|1,0|2,0|3,0");
    // 非法点串返回空。
    EXPECT_STREQ(OpSimplifyPath(api.handle, L"abc|1,0", 0.5), L"");
}

TEST(AlgorithmTest, FindNearestPathPointReturnsIndexAndCoords) {
    CApiHandle api;
    int index = -1, nx = -1, ny = -1;
    EXPECT_EQ(OpFindNearestPathPoint(api.handle, L"0,0|10,0|20,0", 11, 1, &index, &nx, &ny), 1);
    EXPECT_EQ(index, 1);
    EXPECT_EQ(nx, 10);
    EXPECT_EQ(ny, 0);
    // 非法路径返回 0 且输出清零。
    index = 9;
    nx = 9;
    ny = 9;
    EXPECT_EQ(OpFindNearestPathPoint(api.handle, L"bad", 0, 0, &index, &nx, &ny), 0);
    EXPECT_EQ(index, 0);
    EXPECT_EQ(nx, 0);
    EXPECT_EQ(ny, 0);
}

TEST(AlgorithmTest, PointInPolygonRayRule) {
    CApiHandle api;
    EXPECT_EQ(OpPointInPolygon(api.handle, L"5,5", L"0,0|10,0|10,10|0,10"), 1);
    EXPECT_EQ(OpPointInPolygon(api.handle, L"15,5", L"0,0|10,0|10,10|0,10"), 0);
    // 凹多边形（L 形：底部横条 [0,10]x[0,6] + 右上竖条 [6,10]x[6,10]，凹口 x<6 且 y>6）。
    EXPECT_EQ(OpPointInPolygon(api.handle, L"1,1", L"0,0|10,0|10,10|6,10|6,6|0,6"), 1);
    EXPECT_EQ(OpPointInPolygon(api.handle, L"8,8", L"0,0|10,0|10,10|6,10|6,6|0,6"), 1);
    EXPECT_EQ(OpPointInPolygon(api.handle, L"1,8", L"0,0|10,0|10,10|6,10|6,6|0,6"), 0);
    EXPECT_EQ(OpPointInPolygon(api.handle, L"3,3", L"0,0|10,0|10,10|6,10|6,6|0,6"), 1);
    // 顶点不足 3 返回 0。
    EXPECT_EQ(OpPointInPolygon(api.handle, L"5,5", L"0,0|10,10"), 0);
}

} // namespace
