#include "test_support.h"

#include "op_c_api.h"

using namespace test_support;

TEST(YoloTest, SetYoloEngineAcceptsBaseUrlAndAliasArgs) {
    op::Op op;
    EXPECT_EQ(1, op.SetYoloEngine(L"http://127.0.0.1:8090", L"", L"--timeout=5000"));
    EXPECT_EQ(1, op.SetYoloEngine(L"yolo", L"", L"--timeout=2000"));
}

TEST(YoloTest, SetYoloEngineRejectsInvalidUrl) {
    op::Op op;
    EXPECT_EQ(0, op.SetYoloEngine(L"http://", L"", L"--timeout=5000"));
}

TEST(YoloTest, YoloDetectFromFileReturnsFailureJsonForMissingFile) {
    op::Op op;
    std::wstring json = L"not-empty";
    long ret = 123;
    op.YoloDetectFromFile(L"__missing_yolo_input__.bmp", 0.25, 0.45, json, &ret);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(L"{\"ok\":0,\"code\":-1,\"results\":[]}", json);
}

TEST(YoloTest, CApiYoloDetectReturnsFailureJsonForInvalidHandle) {
    EXPECT_STREQ(L"{\"ok\":0,\"code\":-1,\"results\":[]}",
                 OpYoloDetectFromFile(nullptr, L"__missing_yolo_input__.bmp", 0.25, 0.45));
}

// ---- 内置 ONNX 引擎（OnnxYoloEngine，方案B：默认不内置模型）----

// 缺省构建无内置模型：onnx + 空路径应 init 失败但不崩溃
TEST(YoloOnnxTest, InitEmbeddedModelMissingFailsGracefully) {
    op::Op op;
    const int ret = op.SetYoloEngine(L"onnx", L"", L"--conf=0.3");
    if (ret == 1) {
        // 构建期编入了自训模型（build/_deps/yolo_models/yolo.onnx 存在）：合法
        SUCCEED();
    } else {
        EXPECT_EQ(0, ret);
    }
}

// 外挂路径不存在：init 失败
TEST(YoloOnnxTest, InitExternalModelMissingFails) {
    op::Op op;
    EXPECT_EQ(0, op.SetYoloEngine(L"onnx", L"__missing_yolo_model__.onnx", L""));
}

// 非法模型文件（乱码）：session 创建抛异常被捕获，init 失败不崩溃
TEST(YoloOnnxTest, InitCorruptModelFailsGracefully) {
    const std::wstring path = GetTempBmpPath(L"__corrupt_yolo_model__.bin");
    {
        FILE *f = nullptr;
        _wfopen_s(&f, path.c_str(), L"wb");
        ASSERT_NE(f, nullptr);
        fputs("this is not an onnx model", f);
        fclose(f);
    }
    op::Op op;
    EXPECT_EQ(0, op.SetYoloEngine(L"onnx", path.c_str(), L""));
    DeleteFileW(path.c_str());
}

// onnx 切换后再切回 http：引擎选择器正常工作，detect 走 HTTP（无服务端 -> 失败 JSON 但不崩溃）
TEST(YoloOnnxTest, SwitchBackToHttpEngineNoCrash) {
    op::Op op;
    op.SetYoloEngine(L"onnx", L"__missing_yolo_model__.onnx", L"");
    EXPECT_EQ(1, op.SetYoloEngine(L"yolo", L"", L"--timeout=1000"));
    std::wstring json;
    long ret = 123;
    op.YoloDetectFromFile(L"__missing_yolo_input__.bmp", 0.25, 0.45, json, &ret);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(L"{\"ok\":0,\"code\":-1,\"results\":[]}", json);
}
