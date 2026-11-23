// OpCrypto.cpp: 摘要/编码三件套（P2 新增 2026-10-15，参考 OLA 加密模块常用子集）。
// MD5/SHA1 走 WinCrypt 哈希链（advapi32），Base64 走 CryptBinaryToStringW（crypt32）。
// 输入宽字符串一律按 UTF-8 编码后参与运算，输出小写十六进制（摘要）或 ASCII（Base64）。
#include "OpCaptureHelpers.h"
#include "OpResult.h"
#include "../base/CryptoBox.h"
#include "../json/JsonValue.h"

#include <libop.h>

#include "OpContext.h"

#include <Windows.h>
#include <objbase.h>
#include <wincrypt.h>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "advapi32.lib")

#include <algorithm>
#include <cstdio>
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

namespace {

// 原始摘要（HMAC 需要二进制结果再拼一次哈希，不能走 hex 中转）。
bool sha256_raw(const std::string &data, std::vector<BYTE> &digest) {
    digest.assign(32, 0);
    HashHandle h;
    if (!::CryptAcquireContextW(&h.provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        return false;
    if (!::CryptCreateHash(h.provider, CALG_SHA_256, 0, 0, &h.hash))
        return false;
    if (!data.empty() && !::CryptHashData(h.hash, reinterpret_cast<const BYTE *>(data.data()),
                                          static_cast<DWORD>(data.size()), 0))
        return false;
    DWORD len = static_cast<DWORD>(digest.size());
    return ::CryptGetHashParam(h.hash, HP_HASHVAL, digest.data(), &len, 0) && len == 32;
}

std::wstring bytes_to_hex(const std::vector<BYTE> &bytes) {
    static const wchar_t kHex[] = L"0123456789abcdef";
    std::wstring out(bytes.size() * 2, L'\0');
    for (size_t i = 0; i < bytes.size(); ++i) {
        out[i * 2] = kHex[bytes[i] >> 4];
        out[i * 2 + 1] = kHex[bytes[i] & 0x0F];
    }
    return out;
}

// RFC 2104 HMAC-SHA256：K' = H(K)（超块长时）补零到 64 字节，H((K'^opad)||H((K'^ipad)||msg))。
// 不用 CryptoAPI 的 CALG_HMAC：其密钥传入路径依赖 CSP 实现，手搓只需 sha256_raw 两次。
bool hmac_sha256_hex(const std::string &key, const std::string &message, std::wstring &out) {
    out.clear();
    constexpr size_t kBlockSize = 64;
    std::vector<BYTE> block(kBlockSize, 0);
    if (key.size() > kBlockSize) {
        std::vector<BYTE> hashed;
        if (!sha256_raw(key, hashed))
            return false;
        std::copy(hashed.begin(), hashed.end(), block.begin());
    } else {
        std::copy(key.begin(), key.end(), block.begin());
    }

    std::string inner;
    inner.reserve(kBlockSize + message.size());
    std::string outer;
    outer.reserve(kBlockSize + 32);
    for (size_t i = 0; i < kBlockSize; ++i) {
        inner.push_back(static_cast<char>(block[i] ^ 0x36));
        outer.push_back(static_cast<char>(block[i] ^ 0x5C));
    }
    inner += message;

    std::vector<BYTE> inner_digest;
    if (!sha256_raw(inner, inner_digest))
        return false;
    outer.append(reinterpret_cast<const char *>(inner_digest.data()), inner_digest.size());

    std::vector<BYTE> digest;
    if (!sha256_raw(outer, digest))
        return false;
    out = bytes_to_hex(digest);
    return true;
}

} // namespace

void op::Op::Sha256(const wchar_t *text, std::wstring &ret) {
    ret.clear();
    if (!text || !text[0])
        return;
    std::vector<BYTE> digest;
    if (sha256_raw(wstring_to_utf8(text), digest))
        ret = bytes_to_hex(digest);
}

void op::Op::HmacSha256(const wchar_t *key, const wchar_t *text, std::wstring &ret) {
    ret.clear();
    if (!key || !key[0])
        return;
    hmac_sha256_hex(wstring_to_utf8(key), text ? wstring_to_utf8(text) : std::string(), ret);
}

void op::Op::GenerateRandomBytes(long length, std::wstring &ret) {
    ret.clear();
    if (length <= 0 || length > 4096)
        return;
    HCRYPTPROV provider = 0;
    if (!::CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT | CRYPT_SILENT))
        return;
    std::vector<BYTE> bytes(static_cast<size_t>(length), 0);
    const BOOL ok = ::CryptGenRandom(provider, static_cast<DWORD>(bytes.size()), bytes.data());
    ::CryptReleaseContext(provider, 0);
    if (ok)
        ret = bytes_to_hex(bytes);
}

void op::Op::GenerateGuid(std::wstring &ret) {
    ret.clear();
    GUID guid{};
    if (FAILED(::CoCreateGuid(&guid)))
        return;
    wchar_t buf[40] = {};
    if (::swprintf_s(buf, L"%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", guid.Data1, guid.Data2,
                     guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4],
                     guid.Data4[5], guid.Data4[6], guid.Data4[7]) > 0)
        ret = buf;
}

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

// ---- 并发诊断（批 A 2026-11-22）----
// 放在本文件而不是新开 .cpp：内容与"运行时自省"同族（GetLastErrorString / Md5 也在这层），
// 且只需要 OpContext.h 一个额外依赖，不值得为 20 行新增编译单元。
void op::Op::ConcurrencyInfo(std::wstring &retjson) {
    retjson.clear();
    internal::guard_exceptions("ConcurrencyInfo", [&]() {
        using namespace op::json;
        auto num = [](double d) {
            Value n;
            n.type = Type::Number;
            n.number = d;
            return n;
        };
        auto key = [](const wchar_t *k, Value v) { return std::make_pair(std::wstring(k), std::move(v)); };

        Value root;
        root.type = Type::Object;
        root.obj.push_back(key(L"id", num(m_context->id)));
        root.obj.push_back(key(L"in_flight", num(m_context->in_flight.load(std::memory_order_relaxed))));
        root.obj.push_back(key(L"peak", num(m_context->peak_in_flight.load(std::memory_order_relaxed))));
        root.obj.push_back(key(L"reentrant", num(m_context->reentrant_hits.load(std::memory_order_relaxed))));
        root.obj.push_back(key(L"calls_total", num(m_context->calls_total.load(std::memory_order_relaxed))));
        root.obj.push_back(key(L"image_count", num(m_context->image_proc.image_count())));
        root.obj.push_back(key(L"bound", num(m_context->bkproc.IsBind() ? 1 : 0)));
        // 恒定 false：这是**约定**不是**能力探测**。保留字段让脚本能显式判断而不是靠文档记忆。
        Value ts;
        ts.type = Type::Bool;
        ts.boolean = false;
        root.obj.push_back(key(L"thread_safe", std::move(ts)));
        retjson = op::json::Stringify(root);
    });
}

// ---- #4 加密补全（2026-11-23）：AES-GCM / PBKDF2 / 文件摘要 / RSA ----
// AES 与 PBKDF2 走 base/CryptoBox.h（BCrypt），文件摘要继续用本 TU 的 WinCrypt 哈希链。
// RSA 走 CryptoAPI：公钥 SPKI、私钥 PKCS#8/PKCS#1，PKCS#1 v1.5 填充。
namespace {

bool file_hash_hex(ALG_ID alg, size_t digest_len, const std::wstring &path, std::wstring &out) {
    out.clear();
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    HashHandle h;
    bool ok = ::CryptAcquireContextW(&h.provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) != FALSE &&
              ::CryptCreateHash(h.provider, alg, 0, 0, &h.hash) != FALSE;
    std::vector<BYTE> buf(1 << 20);
    while (ok) {
        DWORD got = 0;
        if (!::ReadFile(file, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) {
            ok = false;
            break;
        }
        if (got == 0)
            break;
        if (!::CryptHashData(h.hash, buf.data(), got, 0)) {
            ok = false;
            break;
        }
    }
    ::CloseHandle(file);
    if (!ok)
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

struct RsaContext {
    HCRYPTPROV provider = 0;
    HCRYPTKEY key = 0;
    ~RsaContext() {
        if (key)
            ::CryptDestroyKey(key);
        if (provider)
            ::CryptReleaseContext(provider, 0);
    }
};

// PEM -> DER。CRYPT_STRING_BASE64HEADER 同时接受 PUBLIC KEY / PRIVATE KEY / RSA PRIVATE KEY 头。
bool pem_to_der(const std::wstring &pem, std::vector<BYTE> &der) {
    der.clear();
    const std::string text = wstring_to_utf8(pem);
    if (text.find("-----BEGIN") == std::string::npos)
        return false;
    DWORD len = 0;
    if (!::CryptStringToBinaryA(text.c_str(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64HEADER, nullptr,
                                &len, nullptr, nullptr) ||
        len == 0)
        return false;
    der.resize(len);
    return ::CryptStringToBinaryA(text.c_str(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64HEADER, der.data(),
                                  &len, nullptr, nullptr) != FALSE;
}

// 最小 DER TLV 读取（只支持长度 < 2^24，RSA 密钥足够）。
bool der_read_tlv(const BYTE *&p, const BYTE *end, BYTE &tag, const BYTE *&val, size_t &len) {
    if (p >= end)
        return false;
    tag = *p++;
    if (p >= end)
        return false;
    size_t n = *p++;
    if (n & 0x80) {
        const int bytes = static_cast<int>(n & 0x7F);
        if (bytes == 0 || bytes > 3 || p + bytes > end)
            return false;
        n = 0;
        for (int i = 0; i < bytes; ++i)
            n = (n << 8) | *p++;
    }
    if (p + n > end)
        return false;
    val = p;
    len = n;
    p += n;
    return true;
}

// PKCS#8 PrivateKeyInfo -> 内层 PKCS#1 RSAPrivateKey。
bool unwrap_pkcs8(const std::vector<BYTE> &der, std::vector<BYTE> &inner) {
    inner.clear();
    const BYTE *p = der.data();
    const BYTE *end = der.data() + der.size();
    BYTE tag = 0;
    const BYTE *val = nullptr;
    size_t len = 0;
    if (!der_read_tlv(p, end, tag, val, len) || tag != 0x30)
        return false;
    const BYTE *q = val;
    const BYTE *qend = val + len;
    if (!der_read_tlv(q, qend, tag, val, len) || tag != 0x02) // version
        return false;
    if (!der_read_tlv(q, qend, tag, val, len) || tag != 0x30) // AlgorithmIdentifier
        return false;
    if (!der_read_tlv(q, qend, tag, val, len) || tag != 0x04) // privateKey
        return false;
    inner.assign(val, val + len);
    return true;
}

bool import_rsa_private(const std::vector<BYTE> &der, RsaContext &ctx) {
    DWORD blob_len = 0;
    bool decoded = ::CryptDecodeObjectEx(X509_ASN_ENCODING, PKCS_RSA_PRIVATE_KEY, der.data(),
                                         static_cast<DWORD>(der.size()), 0, nullptr, nullptr, &blob_len) != FALSE;
    std::vector<BYTE> blob;
    if (!decoded) {
        std::vector<BYTE> inner;
        if (!unwrap_pkcs8(der, inner))
            return false;
        if (!::CryptDecodeObjectEx(X509_ASN_ENCODING, PKCS_RSA_PRIVATE_KEY, inner.data(),
                                   static_cast<DWORD>(inner.size()), 0, nullptr, nullptr, &blob_len))
            return false;
        blob.resize(blob_len);
        if (!::CryptDecodeObjectEx(X509_ASN_ENCODING, PKCS_RSA_PRIVATE_KEY, inner.data(),
                                   static_cast<DWORD>(inner.size()), 0, nullptr, blob.data(), &blob_len))
            return false;
    } else {
        blob.resize(blob_len);
        if (!::CryptDecodeObjectEx(X509_ASN_ENCODING, PKCS_RSA_PRIVATE_KEY, der.data(),
                                   static_cast<DWORD>(der.size()), 0, nullptr, blob.data(), &blob_len))
            return false;
    }
    return ::CryptImportKey(ctx.provider, blob.data(), blob_len, 0, 0, &ctx.key) != FALSE;
}

bool import_rsa_public(const std::vector<BYTE> &der, RsaContext &ctx) {
    DWORD info_len = 0;
    if (!::CryptDecodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO, der.data(),
                               static_cast<DWORD>(der.size()), 0, nullptr, nullptr, &info_len) ||
        info_len == 0)
        return false;
    std::vector<BYTE> info(info_len);
    if (!::CryptDecodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO, der.data(),
                               static_cast<DWORD>(der.size()), 0, nullptr, info.data(), &info_len))
        return false;
    auto *pub = reinterpret_cast<CERT_PUBLIC_KEY_INFO *>(info.data());
    return ::CryptImportPublicKeyInfo(ctx.provider, X509_ASN_ENCODING, pub, &ctx.key) != FALSE;
}

bool rsa_load(const std::wstring &pem, bool private_key, RsaContext &ctx) {
    if (!::CryptAcquireContextW(&ctx.provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        return false;
    std::vector<BYTE> der;
    if (!pem_to_der(pem, der))
        return false;
    return private_key ? import_rsa_private(der, ctx) : import_rsa_public(der, ctx);
}

} // namespace

void op::Op::AesEncrypt(const wchar_t *text, const wchar_t *password, std::wstring &ret) {
    ret.clear();
    if (!text || !text[0] || !password || !password[0])
        return;
    const std::string plain = wstring_to_utf8(text);
    const auto blob = op::cryptobox::seal("OPAES01", wstring_to_utf8(password),
                                          reinterpret_cast<const BYTE *>(plain.data()), plain.size());
    if (blob.empty())
        return;
    ret = utf8_to_wstring(op::cryptobox::base64_encode(blob.data(), blob.size()));
}

void op::Op::AesDecrypt(const wchar_t *base64, const wchar_t *password, std::wstring &ret) {
    ret.clear();
    if (!base64 || !base64[0] || !password || !password[0])
        return;
    std::vector<BYTE> blob;
    if (!op::cryptobox::base64_decode(wstring_to_utf8(base64), blob))
        return;
    std::vector<BYTE> plain;
    if (!op::cryptobox::unseal(blob.data(), blob.size(), "OPAES01", wstring_to_utf8(password), plain))
        return;
    ret = utf8_to_wstring(std::string(reinterpret_cast<const char *>(plain.data()), plain.size()));
}

void op::Op::Pbkdf2Sha256(const wchar_t *password, const wchar_t *salt, long iterations, long length,
                          std::wstring &ret) {
    ret.clear();
    if (!password || !salt || iterations <= 0 || iterations > 10000000 || length <= 0 || length > 1024)
        return;
    const std::string pw = wstring_to_utf8(password);
    const std::string salt_text = wstring_to_utf8(salt);
    std::vector<BYTE> out(static_cast<size_t>(length));
    if (!op::cryptobox::pbkdf2_sha256(pw, reinterpret_cast<const BYTE *>(salt_text.data()), salt_text.size(),
                                      static_cast<uint64_t>(iterations), out.data(), out.size()))
        return;
    ret = bytes_to_hex(out);
}

void op::Op::Md5File(const wchar_t *file_name, std::wstring &ret) {
    ret.clear();
    if (!file_name || !file_name[0])
        return;
    file_hash_hex(CALG_MD5, 16, file_name, ret);
}

void op::Op::Sha1File(const wchar_t *file_name, std::wstring &ret) {
    ret.clear();
    if (!file_name || !file_name[0])
        return;
    file_hash_hex(CALG_SHA1, 20, file_name, ret);
}

void op::Op::Sha256File(const wchar_t *file_name, std::wstring &ret) {
    ret.clear();
    if (!file_name || !file_name[0])
        return;
    file_hash_hex(CALG_SHA_256, 32, file_name, ret);
}

void op::Op::RsaEncrypt(const wchar_t *text, const wchar_t *public_key_pem, std::wstring &ret) {
    ret.clear();
    if (!text || !text[0] || !public_key_pem || !public_key_pem[0])
        return;
    RsaContext ctx;
    if (!rsa_load(public_key_pem, /*private_key=*/false, ctx))
        return;
    DWORD bits = 0;
    DWORD bits_len = sizeof(bits);
    if (!::CryptGetKeyParam(ctx.key, KP_KEYLEN, reinterpret_cast<BYTE *>(&bits), &bits_len, 0) || bits == 0)
        return;
    const size_t mod_len = (bits + 7) / 8;
    const std::string plain = wstring_to_utf8(text);
    if (plain.size() + 11 > mod_len)
        return;
    std::vector<BYTE> buf(mod_len, 0);
    std::reverse_copy(plain.begin(), plain.end(), buf.begin()); // CryptoAPI 要求小端整数
    DWORD len = static_cast<DWORD>(plain.size());
    if (!::CryptEncrypt(ctx.key, 0, TRUE, 0, buf.data(), &len, static_cast<DWORD>(mod_len)))
        return;
    std::vector<BYTE> cipher(buf.begin(), buf.begin() + len);
    std::reverse(cipher.begin(), cipher.end());
    ret = utf8_to_wstring(op::cryptobox::base64_encode(cipher.data(), cipher.size()));
}

void op::Op::RsaDecrypt(const wchar_t *base64, const wchar_t *private_key_pem, std::wstring &ret) {
    ret.clear();
    if (!base64 || !base64[0] || !private_key_pem || !private_key_pem[0])
        return;
    RsaContext ctx;
    if (!rsa_load(private_key_pem, /*private_key=*/true, ctx))
        return;
    std::vector<BYTE> cipher;
    if (!op::cryptobox::base64_decode(wstring_to_utf8(base64), cipher) || cipher.empty())
        return;
    std::reverse(cipher.begin(), cipher.end()); // 大端 -> CryptoAPI 小端
    DWORD len = static_cast<DWORD>(cipher.size());
    if (!::CryptDecrypt(ctx.key, 0, TRUE, 0, cipher.data(), &len))
        return;
    std::vector<BYTE> plain(cipher.begin(), cipher.begin() + len);
    std::reverse(plain.begin(), plain.end());
    ret = utf8_to_wstring(std::string(reinterpret_cast<const char *>(plain.data()), plain.size()));
}
