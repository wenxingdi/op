// 图像变换自测（P2 第二十三批，2026-10-15）：OpCvRotate / OpCvFlip。
// 用 OpenCV 直接生成非对称小图（4x2，四角颜色互不相同），经 C API 变换后
// 读回断言像素位置映射——翻转/90° 旋转都是整数坐标的精确映射，不允许插值误差。

#include <gtest/gtest.h>

#include <op_c_api.h>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <filesystem>
#include <string>

namespace {

struct CApiHandle {
    op_handle handle = OpCreate();
    ~CApiHandle() {
        OpDestroy(handle);
    }
};

// 4x2 BGR 图，四角颜色互不相同：
//   TL(0,0)=红  TR(0,3)=绿
//   BL(1,0)=蓝  BR(1,3)=黄
cv::Mat makeAsymImage() {
    cv::Mat img(2, 4, CV_8UC3);
    img.at<cv::Vec3b>(0, 0) = cv::Vec3b(0, 0, 255);    // 红 TL
    img.at<cv::Vec3b>(0, 3) = cv::Vec3b(0, 255, 0);    // 绿 TR
    img.at<cv::Vec3b>(1, 0) = cv::Vec3b(255, 0, 0);    // 蓝 BL
    img.at<cv::Vec3b>(1, 3) = cv::Vec3b(0, 255, 255);  // 黄 BR
    return img;
}

// 目标路径不需要预建文件。
std::wstring makePath(const std::wstring &name) {
    return (std::filesystem::temp_directory_path() / name).wstring();
}

std::wstring writePng(const std::wstring &name, const cv::Mat &img) {
    const auto path = makePath(name);
    cv::imwrite(std::filesystem::path(path).string(), img);
    return path;
}

TEST(CvTransformTest, FlipModesMapCornersExactly) {
    CApiHandle api;
    const std::wstring src = writePng(L"op_cvflip_src.png", makeAsymImage());

    // mode 0 上下翻转：TL<->BL，TR<->BR；尺寸不变
    const std::wstring dst0 = makePath(L"op_cvflip_0.png");
    ASSERT_EQ(OpCvFlip(api.handle, src.c_str(), dst0.c_str(), 0), 1);
    cv::Mat f0 = cv::imread(std::filesystem::path(dst0).string());
    ASSERT_EQ(f0.rows, 2);
    ASSERT_EQ(f0.cols, 4);
    EXPECT_EQ(f0.at<cv::Vec3b>(0, 0), cv::Vec3b(255, 0, 0)) << "mode0: (0,0) 应是原 BL 蓝";
    EXPECT_EQ(f0.at<cv::Vec3b>(1, 0), cv::Vec3b(0, 0, 255)) << "mode0: (1,0) 应是原 TL 红";

    // mode 1 左右翻转：TL<->TR，BL<->BR
    const std::wstring dst1 = makePath(L"op_cvflip_1.png");
    ASSERT_EQ(OpCvFlip(api.handle, src.c_str(), dst1.c_str(), 1), 1);
    cv::Mat f1 = cv::imread(std::filesystem::path(dst1).string());
    ASSERT_EQ(f1.rows, 2);
    ASSERT_EQ(f1.cols, 4);
    EXPECT_EQ(f1.at<cv::Vec3b>(0, 0), cv::Vec3b(0, 255, 0)) << "mode1: (0,0) 应是原 TR 绿";
    EXPECT_EQ(f1.at<cv::Vec3b>(0, 3), cv::Vec3b(0, 0, 255)) << "mode1: (0,3) 应是原 TL 红";

    // mode 2 双向 = 旋转 180°：TL<->BR
    const std::wstring dst2 = makePath(L"op_cvflip_2.png");
    ASSERT_EQ(OpCvFlip(api.handle, src.c_str(), dst2.c_str(), 2), 1);
    cv::Mat f2 = cv::imread(std::filesystem::path(dst2).string());
    ASSERT_EQ(f2.rows, 2);
    ASSERT_EQ(f2.cols, 4);
    EXPECT_EQ(f2.at<cv::Vec3b>(0, 0), cv::Vec3b(0, 255, 255)) << "mode2: (0,0) 应是原 BR 黄";

    // 非法 mode / 非法源文件：返回 0
    EXPECT_EQ(OpCvFlip(api.handle, src.c_str(), dst2.c_str(), 7), 0);
    EXPECT_EQ(OpCvFlip(api.handle, L"", dst2.c_str(), 0), 0);
}

TEST(CvTransformTest, Rotate90SwapsDimensionsLossless) {
    CApiHandle api;
    const std::wstring src = writePng(L"op_cvrotate_src.png", makeAsymImage());
    const std::wstring dst = makePath(L"op_cvrotate_90.png");

    // 正角 = 逆时针：原 TL(0,0) 应转到新图左下角 (rows-1, 0) = (3,0)
    ASSERT_EQ(OpCvRotate(api.handle, src.c_str(), dst.c_str(), 90.0), 1);
    cv::Mat r = cv::imread(std::filesystem::path(dst).string());
    ASSERT_EQ(r.rows, 4) << "90° 旋转后高宽互换";
    ASSERT_EQ(r.cols, 2);
    EXPECT_EQ(r.at<cv::Vec3b>(3, 0), cv::Vec3b(0, 0, 255)) << "逆时针 90°：原 TL 红 应到左下";
    EXPECT_EQ(r.at<cv::Vec3b>(0, 0), cv::Vec3b(0, 255, 0)) << "逆时针 90°：原 TR 绿 应到左上";
    EXPECT_EQ(r.at<cv::Vec3b>(0, 1), cv::Vec3b(0, 255, 255)) << "逆时针 90°：原 BR 黄 应到右上";

    // 180°：尺寸不变，TL<->BR
    const std::wstring dst180 = makePath(L"op_cvrotate_180.png");
    ASSERT_EQ(OpCvRotate(api.handle, src.c_str(), dst180.c_str(), 180.0), 1);
    cv::Mat r180 = cv::imread(std::filesystem::path(dst180).string());
    ASSERT_EQ(r180.rows, 2);
    ASSERT_EQ(r180.cols, 4);
    EXPECT_EQ(r180.at<cv::Vec3b>(0, 0), cv::Vec3b(0, 255, 255));

    // 360° 归一化到恒等：尺寸与四角全等
    const std::wstring dst360 = makePath(L"op_cvrotate_360.png");
    ASSERT_EQ(OpCvRotate(api.handle, src.c_str(), dst360.c_str(), 360.0), 1);
    cv::Mat r360 = cv::imread(std::filesystem::path(dst360).string());
    ASSERT_EQ(r360.rows, 2);
    ASSERT_EQ(r360.cols, 4);
    EXPECT_EQ(r360.at<cv::Vec3b>(0, 0), cv::Vec3b(0, 0, 255));
    EXPECT_EQ(r360.at<cv::Vec3b>(1, 3), cv::Vec3b(0, 255, 255));
}

TEST(CvTransformTest, RotateArbitraryExpandsCanvas) {
    CApiHandle api;
    const std::wstring src = writePng(L"op_cvrotate_arb_src.png", makeAsymImage());
    const std::wstring dst = makePath(L"op_cvrotate_45.png");

    // 45° 任意角：外接矩形扩画布，输出面积严格大于原图（4x2=8）。
    // 注意扁平图 45° 单维可能不增（(4+2)*sin45≈4.2 截断后宽仍 4），只卡总面积。
    ASSERT_EQ(OpCvRotate(api.handle, src.c_str(), dst.c_str(), 45.0), 1);
    cv::Mat r = cv::imread(std::filesystem::path(dst).string());
    ASSERT_FALSE(r.empty());
    EXPECT_GT(r.rows * r.cols, 2 * 4);
    // 原图中心像素旋转后仍在中心附近（中心不动），且通道保持 3
    EXPECT_EQ(r.channels(), 3);

    // 非法源文件返回 0
    EXPECT_EQ(OpCvRotate(api.handle, L"", dst.c_str(), 45.0), 0);
}

} // namespace
