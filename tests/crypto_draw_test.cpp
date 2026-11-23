// crypto + draw overlay 模块自测（P2 第二十一批，2026-10-15）。
// 摘要/编码用 RFC 1321 / FIPS 180-1 公开测试向量钉死行为；
// 绘制走隐藏模式（SetVisible(0)），不上屏、不干扰桌面，只验证调用链与返回值。

#include <gtest/gtest.h>

#include <op_c_api.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

struct CApiHandle {
    op_handle handle = OpCreate();
    ~CApiHandle() {
        OpDestroy(handle);
    }
};

TEST(CryptoApiTest, Md5KnownVectors) {
    CApiHandle api;
    EXPECT_STREQ(OpMd5(api.handle, L"abc"), L"900150983cd24fb0d6963f7d28e17f72");
    // RFC 1321 标准测试组
    EXPECT_STREQ(OpMd5(api.handle, L""), L"");
    EXPECT_STREQ(OpMd5(api.handle, L"The quick brown fox jumps over the lazy dog"),
                 L"9e107d9d372bb6826bd81d3542a419d6");
    // UTF-8 编码后哈希（与 ASCII 同一实现路径，中文只验长度与非空）
    const wchar_t *cn = OpMd5(api.handle, L"中文");
    ASSERT_TRUE(cn != nullptr);
    EXPECT_EQ(std::wstring(cn).size(), 32u);
}

TEST(CryptoApiTest, Sha1KnownVectors) {
    CApiHandle api;
    EXPECT_STREQ(OpSha1(api.handle, L"abc"), L"a9993e364706816aba3e25717850c26c9cd0d89d");
    EXPECT_STREQ(OpSha1(api.handle, L""), L"");
    EXPECT_STREQ(OpSha1(api.handle, L"The quick brown fox jumps over the lazy dog"),
                 L"2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");
}

TEST(CryptoApiTest, Sha256KnownVectors) {
    CApiHandle api;
    EXPECT_STREQ(OpSha256(api.handle, L"abc"), L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_STREQ(OpSha256(api.handle, L""), L"");
    EXPECT_STREQ(OpSha256(api.handle, L"The quick brown fox jumps over the lazy dog"),
                 L"d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
}

TEST(CryptoApiTest, HmacSha256KnownVectors) {
    CApiHandle api;
    // RFC 4231 Test Case 1/2
    EXPECT_STREQ(OpHmacSha256(api.handle, L"key", L"The quick brown fox jumps over the lazy dog"),
                 L"f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8");
    EXPECT_STREQ(OpHmacSha256(api.handle, L"Jefe", L"what do ya want for nothing?"),
                 L"5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    // 空 key 返回空串；空消息合法（HMAC(key, "") 仍应产出摘要）。
    EXPECT_STREQ(OpHmacSha256(api.handle, L"", L"abc"), L"");
    EXPECT_STREQ(OpHmacSha256(api.handle, L"key", L""),
                 L"5d5d139563c95b5967b9bd9a8c9b233a9dedb45072794cd232dc1b74832607d0");
}

TEST(CryptoApiTest, RandomBytesShapeAndVariability) {
    CApiHandle api;
    // 长度翻倍为 hex；非法长度返回空串。
    const std::wstring a = OpGenerateRandomBytes(api.handle, 16);
    const std::wstring b = OpGenerateRandomBytes(api.handle, 16);
    EXPECT_EQ(a.size(), 32u);
    EXPECT_EQ(b.size(), 32u);
    EXPECT_NE(a, b); // 16 字节碰撞概率可忽略
    EXPECT_TRUE(a.find_first_not_of(L"0123456789abcdef") == std::wstring::npos);
    EXPECT_STREQ(OpGenerateRandomBytes(api.handle, 0), L"");
    EXPECT_STREQ(OpGenerateRandomBytes(api.handle, -1), L"");
    EXPECT_STREQ(OpGenerateRandomBytes(api.handle, 4097), L"");
}

TEST(CryptoApiTest, GuidShapeAndUniqueness) {
    CApiHandle api;
    const std::wstring a = OpGenerateGuid(api.handle);
    const std::wstring b = OpGenerateGuid(api.handle);
    ASSERT_EQ(a.size(), 36u);
    EXPECT_EQ(a[8], L'-');
    EXPECT_EQ(a[13], L'-');
    EXPECT_EQ(a[18], L'-');
    EXPECT_EQ(a[23], L'-');
    EXPECT_TRUE(a.find_first_not_of(L"0123456789ABCDEF-") == std::wstring::npos);
    EXPECT_NE(a, b);
}

TEST(CryptoApiTest, Base64RoundTripAndInvalid) {
    CApiHandle api;
    EXPECT_STREQ(OpBase64Encode(api.handle, L"hello"), L"aGVsbG8=");
    EXPECT_STREQ(OpBase64Decode(api.handle, L"aGVsbG8="), L"hello");
    // 空输入往返
    EXPECT_STREQ(OpBase64Encode(api.handle, L""), L"");
    EXPECT_STREQ(OpBase64Decode(api.handle, L""), L"");
    // 非法串返回空串
    EXPECT_STREQ(OpBase64Decode(api.handle, L"!!!not-base64!!!"), L"");
    // 中文 UTF-8 往返保真
    // 注意：c_api 所有字符串返回值共用 string_result 缓冲，有效至下一次字符串调用；
    // 跨调用使用必须先拷贝，否则 clear() 会把输入清空（这正是之前的失败原因）。
    const std::wstring enc = OpBase64Encode(api.handle, L"中文abc123");
    EXPECT_STREQ(OpBase64Decode(api.handle, enc.c_str()), L"中文abc123");
}

TEST(DrawOverlayTest, CommandsReturnOkHidden) {
    CApiHandle api;
    // 先隐藏（测试不打扰桌面）；窗口惰性创建失败（无桌面会话）允许整体跳过。
    if (OpDrawSetVisible(api.handle, 0) != 1)
        GTEST_SKIP() << "overlay 不可用（无桌面会话）";

    EXPECT_EQ(OpDrawRect(api.handle, 10, 10, 200, 100, L"FF0000", 2), 1);
    EXPECT_EQ(OpDrawLine(api.handle, 0, 0, 300, 300, L"00FF00", 1), 1);
    EXPECT_EQ(OpDrawText(api.handle, 50, 50, L"test", L"FFFFFF", 16), 1);
    // 非法颜色返回 0
    EXPECT_EQ(OpDrawRect(api.handle, 0, 0, 10, 10, L"GGGGGG", 1), 0);
    EXPECT_EQ(OpDrawRect(api.handle, 0, 0, 10, 10, L"12345678", 1), 0) << "超过 RRGGBB 应拒绝";
    // 清空与空文本
    EXPECT_EQ(OpDrawClear(api.handle), 1);
    EXPECT_EQ(OpDrawText(api.handle, 0, 0, L"", L"FFFFFF", 14), 0) << "空文本不生成图元";
    // 收尾保持隐藏
    EXPECT_EQ(OpDrawSetVisible(api.handle, 0), 1);
}

// ================= 加密补全 / 注册表 / 全局热键（2026-11-23）=================

std::wstring TempFile(const wchar_t *name) {
    wchar_t dir[MAX_PATH] = {};
    if (::GetTempPathW(MAX_PATH, dir) == 0)
        return name;
    wchar_t buf[MAX_PATH] = {};
    swprintf(buf, MAX_PATH, L"%s%s_%lu", dir, name, static_cast<unsigned long>(::GetCurrentProcessId()));
    return buf;
}

bool WriteBytes(const std::wstring &path, const char *data, size_t size) {
    std::ofstream out(std::filesystem::path(path), std::ios::binary);
    if (!out)
        return false;
    out.write(data, static_cast<std::streamsize>(size));
    return static_cast<bool>(out);
}

std::vector<char> ReadBytes(const std::wstring &path) {
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

TEST(CryptoApiTest, AesRoundTripAndAuthentication) {
    CApiHandle api;
    const std::wstring c1 = OpAesEncrypt(api.handle, L"hello 中文 123", L"p@ssw0rd");
    ASSERT_FALSE(c1.empty());
    const std::wstring c2 = OpAesEncrypt(api.handle, L"hello 中文 123", L"p@ssw0rd");
    // 随机 salt/nonce：同明文两次密文必须不同（钉死"没真加密"的假实现）
    EXPECT_NE(c1, c2);
    EXPECT_EQ(L"hello 中文 123", std::wstring(OpAesDecrypt(api.handle, c1.c_str(), L"p@ssw0rd")));

    // 密码错 -> 认证失败返回空串，不能吐出乱码
    EXPECT_STREQ(OpAesDecrypt(api.handle, c1.c_str(), L"wrong"), L"");
    // 密文改动一位 -> tag 校验失败
    std::wstring tampered = c1;
    tampered[tampered.size() - 5] = (tampered[tampered.size() - 5] == L'A') ? L'B' : L'A';
    EXPECT_STREQ(OpAesDecrypt(api.handle, tampered.c_str(), L"p@ssw0rd"), L"");
    // 截断（丢 tag）-> 失败
    EXPECT_STREQ(OpAesDecrypt(api.handle, c1.substr(0, c1.size() - 8).c_str(), L"p@ssw0rd"), L"");
    // 非法 base64 / 空密码 / 空明文
    EXPECT_STREQ(OpAesDecrypt(api.handle, L"!!!not-base64!!!", L"p@ssw0rd"), L"");
    EXPECT_STREQ(OpAesEncrypt(api.handle, L"abc", L""), L"");
    EXPECT_STREQ(OpAesEncrypt(api.handle, L"", L"pw"), L"");
    EXPECT_STREQ(OpAesDecrypt(api.handle, c1.c_str(), L""), L"");
}

TEST(CryptoApiTest, Pbkdf2Sha256RfcVectors) {
    CApiHandle api;
    // RFC 7914 / 常见 SHA256 测试向量
    EXPECT_STREQ(OpPbkdf2Sha256(api.handle, L"password", L"salt", 1, 32),
                 L"120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    EXPECT_STREQ(OpPbkdf2Sha256(api.handle, L"password", L"salt", 2, 32),
                 L"ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43");
    EXPECT_STREQ(OpPbkdf2Sha256(api.handle, L"password", L"salt", 4096, 32),
                 L"c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");
    // 多块输出（40 字节 = 32 + 8）
    EXPECT_STREQ(OpPbkdf2Sha256(api.handle, L"passwordPASSWORDpassword",
                                L"saltSALTsaltSALTsaltSALTsaltSALTsalt", 4096, 40),
                 L"348c89dbcbd32b2f32d814b8116e84cf2b17347ebc1800181c4e2a1fb8dd53e1c635518c7dac47e9");
    // 非法参数
    EXPECT_STREQ(OpPbkdf2Sha256(api.handle, L"password", L"salt", 0, 32), L"");
    EXPECT_STREQ(OpPbkdf2Sha256(api.handle, L"password", L"salt", 1, 0), L"");
    EXPECT_STREQ(OpPbkdf2Sha256(api.handle, L"password", L"salt", 1, 1025), L"");
    EXPECT_STREQ(OpPbkdf2Sha256(api.handle, L"password", L"salt", 10000001, 32), L"");
}

TEST(CryptoApiTest, FileHashMatchesKnownContent) {
    CApiHandle api;
    const std::wstring path = TempFile(L"__op_hash.txt");
    ASSERT_TRUE(WriteBytes(path, "abc", 3));
    EXPECT_STREQ(OpMd5File(api.handle, path.c_str()), L"900150983cd24fb0d6963f7d28e17f72");
    EXPECT_STREQ(OpSha1File(api.handle, path.c_str()), L"a9993e364706816aba3e25717850c26c9cd0d89d");
    EXPECT_STREQ(OpSha256File(api.handle, path.c_str()),
                 L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    ::DeleteFileW(path.c_str());
    // 文件不存在：空串，不能崩
    EXPECT_STREQ(OpMd5File(api.handle, path.c_str()), L"");
    EXPECT_STREQ(OpSha256File(api.handle, L""), L"");
}

TEST(CryptoApiTest, RsaRoundTripAndKeyRoleChecks) {
    // 一次性测试密钥（2048 位，openssl 生成，仅用于本用例）
    static const wchar_t *kPrivate =
        LR"(-----BEGIN PRIVATE KEY-----
MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQDw1Scq6YVxkpA+
1kW/UwPxDf5JZl9qEtHqPZtcfgseY0Q7fwfAXecmnEh0xei5E5w/ykyrdPvCVcX0
G/wY+/S6+vr8z9wwmZ+DSUc1AsV9Zj7/GTkIOna4quosxTZJkdv9uoubbF/XZRBb
AiXtwG/5/6vIoTAsChNkDS1INAd5T8rB12SrQi0+4sDuqFX6bzsrli3rKjXdb2zp
kPWgyxY0nmMObRNy5YL3JsBMssnpqIlIYQg19OVXUim4afwe0uU7Y5mllOV1oM6j
R9keRmSy0au7ASNDJjfFcQuzb8v8bxVzG3nrsL5K8VpVLY+ebLLomqtAIeYDBG/i
x15uJVdFAgMBAAECggEAOSbUTy4wsUWPPqI51g/8yPlBpaiKLxF9SEHLezUcLV9w
Q1A/hiqinrqKxU9sDXO2ajr3W978md8iBW6YslMkfjTuWA152vyx6oV6xP094abg
f0g8jRPWtbc1sF8ypV3cX4ej9sPOsL52lapaVuQKjsDaQtfhpynS9TTdUJusRGCx
ye8wYwM6LXyWDed9ZYSnUwxD2S/qvhB9jMfK6gv0c3+3H6GVcLpEGqRx9rU8F8fI
74ciG+N57aCdmljk1jd77OOXuK0D9BPBOfNMMHogN+pZhgMJTt2uph7y1BETVy8c
6wOlZhb0TzQVekfDM9yCIWkrnEHEuMDmW6/rAhZ0lwKBgQD6mbnQXMCKgM0Z6i92
jCgYY3InB0FVs1PXjfIjnwTeCVz1/ItNAlZB6GpLMQ2bW57MKDCPsHKqBfrIF+U/
B0dSfG9tEFkU9FIoEI8IVeFpO9S2Q8iQrm2l/BeHQxgQlrqijPpIa0upX0r5t8nE
CEugdF20lB1aoS3FSV8nT389VwKBgQD2BYyTF/A4u/LYpUoQEBUhv+zvjNziqrE/
XUswsI1/OqsAOmdqcAE4ZqI+GErTbhDWfofl175Mc4dgQuzD42bU0ehakZ3gAl6e
13Or4/wvIACMWALZUBK5A8yCpxM1gRqbxJlMRT0Z45HS2h/ptxI3e6YJu/hIJj/+
mFldZfKSwwKBgQDb5TaSakEdh8XxfnglevjX5bNOIeSnsdLZK5oUvb3Eo8ypoPAP
JATSkoFq92EsOOoP1Rg/jaIgJV0YZTdo5hckf0I7HoQ8ieLpexY3RzyjXbfaM4Jz
yks2wg1XSH1+WPTuG+ymzVe4CIE+vJHUbT8HTfTHtsxHMXV7T1WqpexNwwKBgHI1
sUS34DT2S453bSJ31NeOGZg7dG9lflNEJBu+WnGt8qRweIHY7TgZrUfWTyZH+lkY
9E2nEhi7qSTu7KsjbiFuMMeawge0Eej7bB9n/zKBUL/WO7xWh9qMAOzGUwyLmqal
ZeUaX3sdQhz4VchRzIslQ7dRI7mayCqQ3yuUoD+xAoGBAOy7N8JTN0o15jo48FzO
DRkFFymlXM7/ixJ7Rw2L0A5lUTKGDMoZ9VOTjSITeh52HhM2ugctypojsD+4I02c
zhqH9nJTf2pM20ff2K69jcAqd+2g7qzN8qC1LqPdIi8X5/WIlvlF+IfVqD0zYKSx
pNROoA2m7BYpiDDzAEyRs7b7
-----END PRIVATE KEY-----
)";
    static const wchar_t *kPublic =
        LR"(-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA8NUnKumFcZKQPtZFv1MD
8Q3+SWZfahLR6j2bXH4LHmNEO38HwF3nJpxIdMXouROcP8pMq3T7wlXF9Bv8GPv0
uvr6/M/cMJmfg0lHNQLFfWY+/xk5CDp2uKrqLMU2SZHb/bqLm2xf12UQWwIl7cBv
+f+ryKEwLAoTZA0tSDQHeU/Kwddkq0ItPuLA7qhV+m87K5Yt6yo13W9s6ZD1oMsW
NJ5jDm0TcuWC9ybATLLJ6aiJSGEINfTlV1IpuGn8HtLlO2OZpZTldaDOo0fZHkZk
stGruwEjQyY3xXELs2/L/G8Vcxt567C+SvFaVS2Pnmyy6JqrQCHmAwRv4sdebiVX
RQIDAQAB
-----END PUBLIC KEY-----
)";

    CApiHandle api;
    const std::wstring cipher = OpRsaEncrypt(api.handle, L"secret 中文 message", kPublic);
    ASSERT_FALSE(cipher.empty());
    EXPECT_EQ(L"secret 中文 message", std::wstring(OpRsaDecrypt(api.handle, cipher.c_str(), kPrivate)));

    // 角色错用必须失败，而不是用"公钥"解出东西
    EXPECT_STREQ(OpRsaDecrypt(api.handle, cipher.c_str(), kPublic), L"");
    EXPECT_STREQ(OpRsaEncrypt(api.handle, L"x", kPrivate), L"");
    // 篡改密文 -> PKCS#1 填充校验失败。
    // ⚠ 改动必须落在**有效位**上：base64 末组只剩 1 字节时后两个字符的低位会被解码器丢弃，
    // 改结尾字符等于什么都没改（曾因此让断言假红）。改中间字符才是真改密文。
    std::wstring bad = cipher;
    const size_t mid = bad.size() / 2;
    bad[mid] = (bad[mid] == L'A') ? L'B' : L'A';
    EXPECT_STREQ(OpRsaDecrypt(api.handle, bad.c_str(), kPrivate), L"");
    // 长度对不上（截断一半，仍是合法 base64）也必须失败，而不是解出一段垃圾
    const std::wstring half = cipher.substr(0, (cipher.size() / 2 / 4) * 4);
    EXPECT_STREQ(OpRsaDecrypt(api.handle, half.c_str(), kPrivate), L"");
    // 超过 2048 位上限（mod_len-11 = 245 字节）
    EXPECT_STREQ(OpRsaEncrypt(api.handle, std::wstring(300, L'a').c_str(), kPublic), L"");
    EXPECT_STREQ(OpRsaEncrypt(api.handle, L"x", L"not a pem"), L"");
}

TEST(RegistryApiTest, StringAndDwordRoundTrip) {
    CApiHandle api;
    const std::wstring sub = L"Software\\OPTestApi_" + std::to_wstring(::GetCurrentProcessId());
    const std::wstring key = L"HKCU\\" + sub;
    ::RegDeleteTreeW(HKEY_CURRENT_USER, sub.c_str());

    // 环境判别（不是"失败就跳过"）：沙箱 / 受限令牌会把 RegCreateKeyExW 直接拒成
    // ERROR_ACCESS_DENIED(5)，那种环境下本用例判不了实现好坏，只能 SKIP 并写清原因。
    // 只在**原生调用**被拒时跳过；实现坏了（原生能写、API 写不进）依旧是 FAIL。
    {
        HKEY probe = nullptr;
        DWORD disp = 0;
        const LONG rc = ::RegCreateKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                                          &probe, &disp);
        if (probe)
            ::RegCloseKey(probe);
        if (rc == ERROR_ACCESS_DENIED)
            GTEST_SKIP() << "环境禁止写 HKCU（沙箱/受限令牌），注册表用例无法在此环境判定";
    }

    EXPECT_EQ(1, OpRegistryWriteString(api.handle, key.c_str(), L"Str", L"hello 中文"));
    int status = 0;
    {
        const std::wstring value = OpRegistryReadString(api.handle, key.c_str(), L"Str", &status);
        EXPECT_EQ(1, status);
        EXPECT_EQ(L"hello 中文", value);
    }

    EXPECT_EQ(1, OpRegistryWriteDword(api.handle, key.c_str(), L"Num", 0x7fff1234));
    int value = -1;
    EXPECT_EQ(1, OpRegistryReadDword(api.handle, key.c_str(), L"Num", &value));
    EXPECT_EQ(0x7fff1234, static_cast<unsigned>(value));

    // 类型不匹配（DWORD 当字符串读）与缺失值都必须显式失败
    status = 1;
    EXPECT_STREQ(OpRegistryReadString(api.handle, key.c_str(), L"Num", &status), L"");
    EXPECT_EQ(0, status);
    status = 1;
    EXPECT_STREQ(OpRegistryReadString(api.handle, key.c_str(), L"Missing", &status), L"");
    EXPECT_EQ(0, status);
    value = -1;
    EXPECT_EQ(0, OpRegistryReadDword(api.handle, key.c_str(), L"Missing", &value));
    EXPECT_EQ(0, value);

    // 默认值（name 传空）
    EXPECT_EQ(1, OpRegistryWriteString(api.handle, key.c_str(), L"", L"default-value"));
    status = 0;
    EXPECT_EQ(L"default-value", std::wstring(OpRegistryReadString(api.handle, key.c_str(), L"", &status)));
    EXPECT_EQ(1, status);

    // 非法根键
    EXPECT_EQ(0, OpRegistryWriteString(api.handle, L"NOTAHIVE\\Software\\x", L"v", L"1"));
    EXPECT_EQ(0, OpRegistryWriteDword(api.handle, L"NOTAHIVE\\Software\\x", L"v", 1));

    ::RegDeleteTreeW(HKEY_CURRENT_USER, sub.c_str());
    status = 1;
    EXPECT_STREQ(OpRegistryReadString(api.handle, key.c_str(), L"Str", &status), L"") << "清理后必须读不到";
}

TEST(HotkeyApiTest, RegisterPollAndUnregister) {
    CApiHandle api;
    const int id = 4242;
    const int vk_f24 = 0x87;
    if (OpHotkeyRegister(api.handle, id, 0, vk_f24) != 1)
        GTEST_SKIP() << "环境不允许注册热键（无桌面会话或 F24 被占用）";

    EXPECT_EQ(0, OpHotkeyPoll(api.handle, 0)) << "无事件时非阻塞轮询返回 0";
    EXPECT_EQ(0, OpHotkeyRegister(api.handle, id, 0, vk_f24)) << "同一 id 重复注册必须失败";

    // 直接把 WM_HOTKEY 投给热键消息窗口：验证"注册 -> 排队 -> 轮询"整条链路，
    // 不依赖真实按键（CI/隐藏会话里按不出来）。
    wchar_t cls[96] = {};
    swprintf(cls, 96, L"Local\\OpHotkeySink_%lu", static_cast<unsigned long>(::GetCurrentProcessId()));
    HWND sink = ::FindWindowExW(HWND_MESSAGE, nullptr, cls, nullptr);
    if (!sink) {
        OpHotkeyUnregister(api.handle, id);
        GTEST_SKIP() << "找不到 OpHotkeySink 消息窗口";
    }

    ASSERT_TRUE(::PostMessageW(sink, WM_HOTKEY, static_cast<WPARAM>(id), 0));
    EXPECT_EQ(id, OpHotkeyPoll(api.handle, 2000)) << "投递的 WM_HOTKEY 应按 id 取出";
    EXPECT_EQ(0, OpHotkeyPoll(api.handle, 0)) << "事件取走后队列应为空";

    // FIFO：两条按投递顺序取出
    ASSERT_TRUE(::PostMessageW(sink, WM_HOTKEY, static_cast<WPARAM>(id), 0));
    ASSERT_TRUE(::PostMessageW(sink, WM_HOTKEY, static_cast<WPARAM>(id + 1), 0));
    EXPECT_EQ(id, OpHotkeyPoll(api.handle, 1000));
    EXPECT_EQ(id + 1, OpHotkeyPoll(api.handle, 1000));

    EXPECT_EQ(1, OpHotkeyUnregister(api.handle, id));
    EXPECT_EQ(0, OpHotkeyUnregister(api.handle, id)) << "重复注销必须失败";
}

TEST(HotkeyApiTest, InvalidArgumentsRejected) {
    CApiHandle api;
    EXPECT_EQ(0, OpHotkeyRegister(api.handle, 0, 0, 0x41)) << "id 下界";
    EXPECT_EQ(0, OpHotkeyRegister(api.handle, 0xBFFF + 1, 0, 0x41)) << "id 上界";
    EXPECT_EQ(0, OpHotkeyRegister(api.handle, 10, 0x10, 0x41)) << "modifiers 只允许低 4 位";
    EXPECT_EQ(0, OpHotkeyRegister(api.handle, 10, 0, 0)) << "vk 下界";
    EXPECT_EQ(0, OpHotkeyRegister(api.handle, 10, 0, 0x100)) << "vk 上界";
}

TEST(HotkeyApiTest, UnregisterAllReturnsCount) {
    CApiHandle api;
    const int vk_first = 0x86; // F23
    const int vk_second = 0x85; // F22
    if (OpHotkeyRegister(api.handle, 4301, 0, vk_first) != 1 ||
        OpHotkeyRegister(api.handle, 4302, 0, vk_second) != 1) {
        OpHotkeyUnregisterAll(api.handle);
        GTEST_SKIP() << "环境不允许注册测试热键";
    }
    EXPECT_EQ(2, OpHotkeyUnregisterAll(api.handle));
    EXPECT_EQ(0, OpHotkeyUnregisterAll(api.handle)) << "已清空后再清应是 0 个";
}

} // namespace
