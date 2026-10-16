// 鼠标光标图像自测（P2 第二十五批，2026-10-16）：OpGetCursorImage。
// 返回 32bpp BMP 的 Base64：解出字节流后校验 BM 魔数、头尺寸与像素区长度，
// 宽高与 OpGetCursorShape 的元数据交叉验证。无桌面会话（光标不可见）允许跳过。

#include <gtest/gtest.h>

#include <op_c_api.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

struct CApiHandle {
    op_handle handle = OpCreate();
    ~CApiHandle() {
        OpDestroy(handle);
    }
};

// 按固定偏移解析 BMP 头字段（避免 #pragma pack 在测试编译单元里不生效的坑）。
unsigned short readU16(const std::vector<unsigned char> &b, size_t off) {
    return static_cast<unsigned short>(b[off] | (b[off + 1] << 8));
}
unsigned int readU32(const std::vector<unsigned char> &b, size_t off) {
    return static_cast<unsigned int>(b[off]) | (static_cast<unsigned int>(b[off + 1]) << 8) |
           (static_cast<unsigned int>(b[off + 2]) << 16) | (static_cast<unsigned int>(b[off + 3]) << 24);
}
int readI32(const std::vector<unsigned char> &b, size_t off) {
    return static_cast<int>(readU32(b, off));
}

// 测试侧 Base64 解码（仅含标准字符集，不处理空白）。
std::vector<unsigned char> base64Decode(const std::wstring &s) {
    int table[256] = {0};
    for (int i = 0; i < 26; ++i) {
        table[static_cast<unsigned char>('A' + i)] = i;
        table[static_cast<unsigned char>('a' + i)] = 26 + i;
    }
    for (int i = 0; i < 10; ++i)
        table[static_cast<unsigned char>('0' + i)] = 52 + i;
    table[static_cast<unsigned char>('+')] = 62;
    table[static_cast<unsigned char>('/')] = 63;

    std::vector<unsigned char> out;
    int val = 0;
    int bits = -8;
    for (wchar_t wc : s) {
        if (wc == L'=')
            break;
        if (wc > 255 || table[static_cast<unsigned char>(wc)] == 0 && wc != L'A')
            continue;
        val = (val << 6) + table[static_cast<unsigned char>(wc)];
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<unsigned char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

TEST(CursorImageTest, ReturnsBmpWithConsistentHeaders) {
    CApiHandle api;
    const std::wstring shape = OpGetCursorShape(api.handle);
    const std::wstring b64 = OpGetCursorImage(api.handle);
    if (b64.empty())
        GTEST_SKIP() << "光标不可见或无桌面会话";

    ASSERT_FALSE(shape.empty());
    const std::vector<unsigned char> bmp = base64Decode(b64);
    ASSERT_GE(bmp.size(), 54u);

    // BITMAPFILEHEADER: type@0, size@2, offset@10；BITMAPINFOHEADER: size@14, width@18,
    // height@22, planes@26, bit_count@28, compression@30, size_image@34
    const unsigned short type = readU16(bmp, 0);
    const unsigned int file_size = readU32(bmp, 2);
    const unsigned int pixel_offset = readU32(bmp, 10);
    const unsigned int header_size = readU32(bmp, 14);
    const int width = readI32(bmp, 18);
    const int height = readI32(bmp, 22);
    const unsigned short bit_count = readU16(bmp, 28);
    const unsigned int compression = readU32(bmp, 30);

    EXPECT_EQ(type, 0x4D42) << "BMP 魔数应为 'BM'";
    EXPECT_EQ(pixel_offset, 54u);
    EXPECT_EQ(file_size, bmp.size());
    EXPECT_EQ(header_size, 40u) << "BITMAPINFOHEADER";
    EXPECT_EQ(bit_count, 32);
    EXPECT_EQ(compression, 0u) << "BI_RGB";
    EXPECT_LT(height, 0) << "top-down 负高度";
    EXPECT_EQ(bmp.size(), 54u + static_cast<size_t>(width) * static_cast<size_t>(-height) * 4);

    // 与 GetCursorShape 元数据交叉验证宽高（shape: visible,hash,width,height,hotX,hotY）
    int visible = 0;
    unsigned long long hash = 0;
    long meta_w = 0, meta_h = 0, hot_x = 0, hot_y = 0;
    ASSERT_EQ(swscanf(shape.c_str(), L"%d,%llx,%ld,%ld,%ld,%ld", &visible, &hash, &meta_w, &meta_h, &hot_x, &hot_y),
              6);
    EXPECT_EQ(width, meta_w);
    EXPECT_EQ(-height, meta_h);
}

TEST(CursorImageTest, NullHandleReturnsEmpty) {
    EXPECT_EQ(std::wstring(OpGetCursorImage(nullptr)), std::wstring());
}

} // namespace
