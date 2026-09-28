// 共享帧读/写链路的纯函数判据。
//
// 为什么需要本文件：
//   6 条 hook 写入路径（D3D9/10/11/12/GL/EGL）把帧写进同一份共享内存，宿主侧用同一个
//   校验函数决定"这一帧能不能读"。这条链路是「撕裂帧 / 半写帧」的唯一防线，却长期零单测 ——
//   因为它的实现原先都住在 HookCapture.cpp（依赖 blackbone，编不进测试进程）的匿名命名空间里。
//   本轮把 isHookFrameReady / makeHookFrameView / hookFrameRow 与写入侧的
//   make_shared_frame_span / write_shared_frame 搬进 hook/SharedFrame.h 后，
//   它们成了 header-only 的纯函数，可以直接被本文件覆盖。
//
// 覆盖重点不是"跑通"，而是几个**一旦写错就必定静默**的点：
//   - 校验和失配必须被拒（否则撕裂帧被当成正常帧送进 OCR/找色）
//   - 溢出保护必须返回 0（否则拿一个"看起来正常"的大数当容量去写 -> 越界）
//   - 容量边界必须精确（否则截到一半的帧被当成完整帧）

#include "test_support.h"

#include "../libop/base/AutomationModes.h" // IBF_B8G8R8A8
#include "../libop/hook/DxCaptureCommon.h" // NormalizeDxgiFormat
#include "../libop/hook/SharedFrame.h"

#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace {

const HWND kHwndA = reinterpret_cast<HWND>(0x1234ABCDull);
const HWND kHwndB = reinterpret_cast<HWND>(0xDEADBEEFull);

// 造一个"正常写入侧会产出"的帧头：format() 会自增 frameId、打时间戳并算出自洽的 chk。
op::capture::FrameInfo MakeReadyFrame(HWND hwnd, int width, int height) {
    op::capture::FrameInfo info = {};
    info.format(hwnd, width, height);
    return info;
}

} // namespace

// ---------------------------------------------------------------- 尺寸与容量

TEST(SharedFrameSizeTest, ComputesHeaderPlusPixels) {
    const std::uint64_t expected = sizeof(op::capture::FrameInfo) + 16ull * 8ull * 4ull;
    EXPECT_EQ(op::hook::RequiredSharedFrameBytes(16, 8), expected);
}

// 零尺寸必须返回 0：调用方拿 0 会走"无容量"分支去重建映射，而不是按 28 字节算出一个
// 看起来合法的需求值。
TEST(SharedFrameSizeTest, ZeroDimensionYieldsZero) {
    EXPECT_EQ(op::hook::RequiredSharedFrameBytes(0, 100), 0u);
    EXPECT_EQ(op::hook::RequiredSharedFrameBytes(100, 0), 0u);
    EXPECT_EQ(op::hook::RequiredSharedFrameBytes(0, 0), 0u);
}

// 两条溢出保护各一测。输入要选得能**分别命中**，否则是假通过：
// 用 UINT64_MAX 当宽时，width*height 会回绕成一个仍然很大的值，于是第二条保护
// （像素字节数 vs size_t）也拦得住 —— "第一条被删掉"这件事根本测不出来（反向验证实测如此）。
// 所以这里用 1<<63：它乘 2 正好回绕成 0，第二条拦不住（0 当然不超界），只有第一条能拦。
TEST(SharedFrameSizeTest, RejectsWidthTimesHeightOverflow) {
    const std::uint64_t wrap = 1ull << 63;
    EXPECT_EQ(op::hook::RequiredSharedFrameBytes(wrap, 2), 0u)
        << "width*height 回绕成 0，只有第一条保护能拦（反向验证点）";
    EXPECT_EQ(op::hook::RequiredSharedFrameBytes(2, wrap), 0u) << "同上（宽高互换）";

    // 大值方向一并覆盖：此时两条保护都成立。
    const std::uint64_t huge = (std::numeric_limits<std::uint64_t>::max)();
    EXPECT_EQ(op::hook::RequiredSharedFrameBytes(huge, 2), 0u) << "width*height 溢出未拦住（大值方向）";
    EXPECT_EQ(op::hook::RequiredSharedFrameBytes(2, huge), 0u) << "width*height 溢出未拦住（大值方向）";
}

TEST(SharedFrameSizeTest, RejectsPixelBytesOverflowAgainstSizeT) {
    // width*height 本身不溢出（正好是 max/2*2 = max-1），但再乘 4 字节就超了 size_t。
    const std::uint64_t half = (std::numeric_limits<size_t>::max)() / 2;
    EXPECT_EQ(op::hook::RequiredSharedFrameBytes(half, 2), 0u) << "像素字节数溢出未拦住";
}

TEST(SharedFrameCapacityTest, RejectsZeroSizedFrameRegardlessOfMapping) {
    op::SharedMemory mem;
    ASSERT_TRUE(mem.open_create(L"op_test_shared_frame_cap_zero", 1 << 16)) << "无法创建测试用映射";
    EXPECT_FALSE(op::hook::SharedFrameHasCapacity(mem, 0, 10));
    EXPECT_FALSE(op::hook::SharedFrameHasCapacity(mem, 10, 0));
}

// 边界必须精确：刚好放得下要通过，多一行就必须拒。
// 注意 mem.size() 报的是映射视图的真实大小（页对齐），所以这里先读出它再反推尺寸，
// 而不是假设申请值就是实际值。
TEST(SharedFrameCapacityTest, BoundaryIsExactFit) {
    op::SharedMemory mem;
    ASSERT_TRUE(mem.open_create(L"op_test_shared_frame_cap_bound", 1 << 20)) << "无法创建测试用映射";
    const size_t cap = mem.size();
    ASSERT_GT(cap, sizeof(op::capture::FrameInfo) + 4u) << "映射太小，无法做边界断言";

    const std::uint64_t maxPixels = (cap - sizeof(op::capture::FrameInfo)) / 4u;
    EXPECT_TRUE(op::hook::SharedFrameHasCapacity(mem, maxPixels, 1)) << "恰好放得下却被拒（会白白重建映射）";
    EXPECT_FALSE(op::hook::SharedFrameHasCapacity(mem, maxPixels, 2)) << "放不下却接受了（会越界写）";
}

// ---------------------------------------------------------------- 帧可读性校验

TEST(HookFrameReadyTest, AcceptsSelfConsistentFrameForMatchingWindow) {
    const auto info = MakeReadyFrame(kHwndA, 640, 480);
    EXPECT_TRUE(op::hook::isHookFrameReady(info, kHwndA));
}

// 校验过程会重算校验和，但只能改本地副本：若实现改成直接改入参，第二次调用看到的
// 就是"被自己写过的 chk"，判据会自我污染。
TEST(HookFrameReadyTest, IsIdempotentAndDoesNotMutateInput) {
    const auto info = MakeReadyFrame(kHwndA, 640, 480);
    const auto chkBefore = info.chk;

    EXPECT_TRUE(op::hook::isHookFrameReady(info, kHwndA));
    EXPECT_TRUE(op::hook::isHookFrameReady(info, kHwndA)) << "第二次调用结果变了 -> 入参被改写过";
    EXPECT_EQ(info.chk, chkBefore) << "入参被 mutate 了（应只改本地副本）";
}

// 撕裂帧：写入侧先写 header、后写像素，读侧可能读到 frameId/时间戳与 chk 对不上的中间态。
// 这一条是整条链路的唯一防线 —— 放过去，撕裂帧会被当成正常画面送进 OCR/找色。
TEST(HookFrameReadyTest, RejectsTornFrameWithMismatchedChecksum) {
    auto info = MakeReadyFrame(kHwndA, 640, 480);
    info.width = 641; // 改了字段但不重算 chk -> 校验和失配
    EXPECT_FALSE(op::hook::isHookFrameReady(info, kHwndA)) << "校验和失配的撕裂帧被接受了";
}

// 上一个绑定残留的旧帧：校验和自洽、尺寸正常，只有 hwnd 对不上。
TEST(HookFrameReadyTest, RejectsFrameForAnotherWindow) {
    const auto info = MakeReadyFrame(kHwndB, 640, 480);
    EXPECT_FALSE(op::hook::isHookFrameReady(info, kHwndA)) << "别的窗口的帧被当成当前窗口的帧";
}

// 初始化态：header 在场、校验和自洽，但宽高还是 0。
TEST(HookFrameReadyTest, RejectsZeroSizedHeader) {
    EXPECT_FALSE(op::hook::isHookFrameReady(MakeReadyFrame(kHwndA, 0, 480), kHwndA));
    EXPECT_FALSE(op::hook::isHookFrameReady(MakeReadyFrame(kHwndA, 640, 0), kHwndA));
}

// ---------------------------------------------------------------- 校验和本身

TEST(FrameChecksumTest, IsStableAcrossRecomputation) {
    auto info = MakeReadyFrame(kHwndA, 320, 240);
    const auto first = info.chk;
    info.fmtChk();
    EXPECT_EQ(info.chk, first) << "重算校验和结果不稳定 -> 正常帧会被误判为撕裂帧";
}

// 被校验和覆盖的每个字段都必须真的改变结果，否则该字段上的撕裂检测形同虚设。
TEST(FrameChecksumTest, DetectsEveryCoveredField) {
    const auto base = MakeReadyFrame(kHwndA, 320, 240);

    auto mutated = base;
    mutated.frameId += 1;
    mutated.fmtChk();
    EXPECT_NE(mutated.chk, base.chk) << "frameId 变化没有被校验和覆盖";

    mutated = base;
    mutated.time += 1;
    mutated.fmtChk();
    EXPECT_NE(mutated.chk, base.chk) << "time 变化没有被校验和覆盖";

    mutated = base;
    mutated.width += 1;
    mutated.fmtChk();
    EXPECT_NE(mutated.chk, base.chk) << "width 变化没有被校验和覆盖";

    mutated = base;
    mutated.height += 1;
    mutated.fmtChk();
    EXPECT_NE(mutated.chk, base.chk) << "height 变化没有被校验和覆盖";

    mutated = base;
    mutated.hwnd ^= 1;
    mutated.fmtChk();
    EXPECT_NE(mutated.chk, base.chk) << "hwnd 变化没有被校验和覆盖";
}

// ---------------------------------------------------------------- 行寻址

// 行/列偏移算错会取到相邻行的像素：截图看起来"错位一列/一行"，但尺寸与字节数全部正常。
TEST(HookFrameRowTest, ReturnsRequestedWindow) {
    constexpr int kWidth = 3;
    constexpr int kHeight = 2;
    std::vector<std::byte> pixels(static_cast<size_t>(kWidth) * kHeight * 4);
    for (int row = 0; row < kHeight; ++row) {
        for (int col = 0; col < kWidth; ++col) {
            for (int b = 0; b < 4; ++b) {
                pixels[(static_cast<size_t>(row) * kWidth + col) * 4 + b] =
                    static_cast<std::byte>(row * 10 + col);
            }
        }
    }

    const std::span<const std::byte> view(pixels);
    const auto window = op::hook::hookFrameRow(view, kWidth, /*row=*/1, /*x=*/1, /*width=*/2);

    ASSERT_EQ(window.size(), 8u) << "取出的字节数不等于 width*4";
    EXPECT_EQ(static_cast<unsigned char>(window[0]), 11u) << "第 1 行第 1 列取错（偏移算错）";
    EXPECT_EQ(static_cast<unsigned char>(window[4]), 12u) << "第 1 行第 2 列取错（步长算错）";
}

// ---------------------------------------------------------------- 写入侧

TEST(SharedFrameWriteTest, SpanCoversHeaderPlusPixels) {
    op::SharedMemory mem;
    ASSERT_TRUE(mem.open_create(L"op_test_shared_frame_write", 1 << 20)) << "无法创建测试用映射";
    const auto span = op::hook::make_shared_frame_span(mem, 4, 3);
    EXPECT_EQ(span.size(), sizeof(op::capture::FrameInfo) + 4u * 3u * 4u);
}

// 写出的帧头必须自洽 —— 否则读取侧 isHookFrameReady 永远判"无帧"，而捕获返回值一切正常。
TEST(SharedFrameWriteTest, WritesConsistentHeaderAndCopiesPixels) {
    op::SharedMemory mem;
    ASSERT_TRUE(mem.open_create(L"op_test_shared_frame_write2", 1 << 20)) << "无法创建测试用映射";
    const HWND hwnd = reinterpret_cast<HWND>(0x2222ull);

    auto span = op::hook::make_shared_frame_span(mem, 2, 2);
    const unsigned char src[2 * 2 * 4] = {
        1,  2,  3,  4,  5,  6,  7,  8,  // 第 0 行
        9,  10, 11, 12, 13, 14, 15, 16, // 第 1 行
    };
    op::hook::write_shared_frame(span, hwnd, 2, 2, src, /*sourceRows=*/2, /*sourceCols=*/2, /*rowPitch=*/2 * 4,
                                IBF_B8G8R8A8);

    const auto *info = reinterpret_cast<const op::capture::FrameInfo *>(span.data());
    EXPECT_EQ(info->hwnd, reinterpret_cast<unsigned __int64>(hwnd));
    EXPECT_EQ(info->width, 2u);
    EXPECT_EQ(info->height, 2u);
    EXPECT_TRUE(op::hook::isHookFrameReady(*info, hwnd)) << "写出的帧头不自洽 -> 读取侧会永远判无帧";

    // BGRA 且 rowPitch == cols*4，走的是整块 memcpy 分支，字节应逐一相同。
    const std::byte *px = span.data() + sizeof(op::capture::FrameInfo);
    for (size_t i = 0; i < sizeof(src); ++i)
        EXPECT_EQ(static_cast<unsigned char>(px[i]), src[i]) << "像素第 " << i << " 字节不一致";
}

// ---------------------------------------------------------------- 交换链格式归一化

TEST(DxgiFormatNormalizeTest, StripsSrgbSuffixOnly) {
    using op::hook::NormalizeDxgiFormat;
    EXPECT_EQ(NormalizeDxgiFormat(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB), DXGI_FORMAT_B8G8R8A8_UNORM);
    EXPECT_EQ(NormalizeDxgiFormat(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB), DXGI_FORMAT_R8G8B8A8_UNORM);
}

// 非 sRGB 必须原样透传：归一化顺手改成别的格式会让 staging 与后备缓冲格式不符，
// 结果是 CopyResource 失败或产出错色帧（全程静默）。
TEST(DxgiFormatNormalizeTest, LeavesNonSrgbUntouched) {
    using op::hook::NormalizeDxgiFormat;
    EXPECT_EQ(NormalizeDxgiFormat(DXGI_FORMAT_B8G8R8A8_UNORM), DXGI_FORMAT_B8G8R8A8_UNORM);
    EXPECT_EQ(NormalizeDxgiFormat(DXGI_FORMAT_R8G8B8A8_UNORM), DXGI_FORMAT_R8G8B8A8_UNORM);
    EXPECT_EQ(NormalizeDxgiFormat(DXGI_FORMAT_R10G10B10A2_UNORM), DXGI_FORMAT_R10G10B10A2_UNORM);
    EXPECT_EQ(NormalizeDxgiFormat(DXGI_FORMAT_UNKNOWN), DXGI_FORMAT_UNKNOWN);
}
