// OpCrypto.cpp: 摘要/编码三件套（P2 新增 2026-10-15，参考 OLA 加密模块常用子集）。
// MD5/SHA1 走 WinCrypt 哈希链（advapi32），Base64 走 CryptBinaryToStringW（crypt32）。
// 输入宽字符串一律按 UTF-8 编码后参与运算，输出小写十六进制（摘要）或 ASCII（Base64）。
#include "OpResult.h"

#include <libop.h>

#include <Windows.h>
#include <wincrypt.h>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "advapi32.lib")

#include <string>
#include <vector>

namespace {

// CP_UTF8 双向转换（Utils 只有 utf8_to_ansi，文件/加密模块各自自含一份）。
std::string wstring_to_utf8(const std::wstring &s) {
    if (s.empty())
        return "";
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return "";
    std::string out(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring utf8_to_wstring(const std::string &s) {
    if (s.empty())
        return L"";
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0)
        return L"";
    std::wstring out(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

// WinCrypt 哈希句柄 RAII
struct HashHandle {
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    ~HashHandle() {
        if (hash)
            ::CryptDestroyHash(hash);
        if (provider)
            ::CryptReleaseContext(provider, 0);
    }
};

// 计算 data 的 MD5/SHA1（ALG_ID 传入 CALG_MD5/CALG_SHA1），输出小写十六进制。
// digest_len 用算法常量（MD5=16/SHA1=20），不查 HP_HASHSIZE——部分 CSP（本机
// Win11 实证）对 HP_HASHSIZE 按 16 字节写盘，缓冲区给小了直接 ERROR_MORE_DATA。
bool hash_hex(ALG_ID alg, size_t digest_len, const std::string &data, std::wstring &out) {
    out.clear();
    HashHandle h;
    if (!::CryptAcquireContextW(&h.provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        return false;
    if (!::CryptCreateHash(h.provider, alg, 0, 0, &h.hash))
        return false;
    if (!data.empty() && !::CryptHashData(h.hash, reinterpret_cast<const BYTE *>(data.data()),
                                          static_cast<DWORD>(data.size()), 0))
        return false;
    std::vector<BYTE> digest(digest_len);
    DWORD len = static_cast<DWORD>(digest.size());
    if (!::CryptGetHashParam(h.hash, HP_HASHVAL, digest.data(), &len, 0))
        return false;
    static const wchar_t kHex[] = L"0123456789abcdef";
    out.resize(static_cast<size_t>(len) * 2);
    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = kHex[digest[i] >> 4];
        out[i * 2 + 1] = kHex[digest[i] & 0x0F];
    }
    return true;
}

} // namespace

void op::Op::Md5(const wchar_t *text, std::wstring &ret) {
    ret.clear();
    if (!text || !text[0])
        return;
    hash_hex(CALG_MD5, 16, wstring_to_utf8(text), ret);
}

void op::Op::Sha1(const wchar_t *text, std::wstring &ret) {
    ret.clear();
    if (!text || !text[0])
        return;
    hash_hex(CALG_SHA1, 20, wstring_to_utf8(text), ret);
}

void op::Op::Base64Encode(const wchar_t *text, std::wstring &ret) {
    ret.clear();
    if (!text || !text[0])
        return;
    const std::string utf8 = wstring_to_utf8(text);
    DWORD len = 0;
    if (!::CryptBinaryToStringW(reinterpret_cast<const BYTE *>(utf8.data()), static_cast<DWORD>(utf8.size()),
                                CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &len))
        return;
    std::wstring out(len, L'\0');
    if (!::CryptBinaryToStringW(reinterpret_cast<const BYTE *>(utf8.data()), static_cast<DWORD>(utf8.size()),
                                CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &len))
        return;
    out.resize(len);
    // 各版本返回的 len 是否含 NUL 终止符不一致：统一从尾部剔除 '\0'（内容中的 '=' 是有效字符，不能误删）
    while (!out.empty() && out.back() == L'\0')
        out.pop_back();
    ret = std::move(out);
}

void op::Op::Base64Decode(const wchar_t *base64, std::wstring &ret) {
    ret.clear();
    if (!base64 || !base64[0])
        return;
    // 先量长度（CRYPT_STRING_BASE64_ANY 容忍空白，但仍拒绝非法字符）
    DWORD byte_len = 0;
    DWORD skip = 0, flags = 0;
    if (!::CryptStringToBinaryW(base64, 0, CRYPT_STRING_BASE64_ANY, nullptr, &byte_len, &skip, &flags) || byte_len == 0)
        return;
    std::string utf8(byte_len, '\0');
    if (!::CryptStringToBinaryW(base64, 0, CRYPT_STRING_BASE64_ANY, reinterpret_cast<BYTE *>(utf8.data()), &byte_len,
                                &skip, &flags))
        return;
    utf8.resize(byte_len);
    ret = utf8_to_wstring(utf8);
}
