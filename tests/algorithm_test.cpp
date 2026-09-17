// algorithm 模块自测：AStarFindPath / FindNearestPos 均为纯计算，免窗口/免注入，本进程内直接验证。
// 路径用例的期望值均按 AStar.h 的确定性行为推演（切比雪夫启发式 + f 相同按大 g 优先 + 方向序），
// 不依赖优先队列对等价节点的弹出顺序。

#include <gtest/gtest.h>

#include <op_c_api.h>

#include <string>
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

} // namespace
