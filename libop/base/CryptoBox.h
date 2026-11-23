#pragma once

// 加密工具箱（header-only）：AES-256-GCM / PBKDF2-SHA256 / 密封容器 / Base64 / 文件 IO。
// 为什么单独抽头：OpCrypto（字符串与文件 API）和 OpYolo（加密模型包）都要用同一套
// 密钥派生与容器格式；复制两份必然出现"同一密码两边解不开"的漂移。
//
// 容器格式（全部小端）：
//   magic[8] | version:u32 | plain_size:u64 | salt[16] | nonce[12] | tag[16] | cipher[...]
// AAD = magic+version+plain_size（前 20 字节），防止改头后仍能解密。
// 密钥 = PBKDF2-HMAC-SHA256(password, salt, 100000, 32)。

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace op::cryptobox {

constexpr size_t kMagicLen = 8;
constexpr size_t kSaltLen = 16;
constexpr size_t kNonceLen = 12;
constexpr size_t kTagLen = 16;
constexpr uint32_t kSealVersion = 1;
constexpr uint64_t kPbkdf2Iterations = 100000;
constexpr size_t kHeaderLen = kMagicLen + 4 + 8 + kSaltLen + kNonceLen + kTagLen; // 64

struct NtStatus {
    NTSTATUS code = 0;
    explicit operator bool() const { return code >= 0; }
};

inline void secure_zero(void *p, size_t n) {
    if (!p || !n)
        return;
    ::SecureZeroMemory(p, n);
}

inline std::vector<uint8_t> random_bytes(size_t n) {
    std::vector<uint8_t> out(n, 0);
    if (n == 0)
        return out;
    const NTSTATUS st = ::BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (st < 0)
        out.clear();
    return out;
}

// PBKDF2-HMAC-SHA256（BCryptDeriveKeyPBKDF2，Win8+）。out 为空/密码过长时返回 false。
inline bool pbkdf2_sha256(const std::string &password, const uint8_t *salt, size_t salt_len, uint64_t iterations,
                          uint8_t *out, size_t out_len) {
    if (!out || out_len == 0 || salt_len > 1024 || iterations == 0)
        return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    NTSTATUS st = ::BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (st < 0)
        return false;
    st = ::BCryptDeriveKeyPBKDF2(alg, reinterpret_cast<PUCHAR>(const_cast<char *>(password.data())),
                                 static_cast<ULONG>(password.size()), const_cast<PUCHAR>(salt),
                                 static_cast<ULONG>(salt_len), iterations, out, static_cast<ULONG>(out_len), 0);
    ::BCryptCloseAlgorithmProvider(alg, 0);
    return st >= 0;
}

class AesGcmKey {
  public:
    AesGcmKey() = default;
    AesGcmKey(const AesGcmKey &) = delete;
    AesGcmKey &operator=(const AesGcmKey &) = delete;
    ~AesGcmKey() { reset(); }

    void reset() {
        if (key_) {
            ::BCryptDestroyKey(key_);
            key_ = nullptr;
        }
        if (alg_) {
            ::BCryptCloseAlgorithmProvider(alg_, 0);
            alg_ = nullptr;
        }
    }

    bool init(const uint8_t *key32, size_t key_len = 32) {
        reset();
        if (!key32 || key_len != 32)
            return false;
        NTSTATUS st = ::BCryptOpenAlgorithmProvider(&alg_, BCRYPT_AES_ALGORITHM, nullptr, 0);
        if (st < 0) {
            reset();
            return false;
        }
        st = ::BCryptSetProperty(alg_, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(BCRYPT_CHAIN_MODE_GCM)),
                                 sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
        if (st < 0) {
            reset();
            return false;
        }
        st = ::BCryptGenerateSymmetricKey(alg_, &key_, nullptr, 0, const_cast<PUCHAR>(key32),
                                          static_cast<ULONG>(key_len), 0);
        if (st < 0) {
            reset();
            return false;
        }
        return true;
    }

    BCRYPT_KEY_HANDLE get() const { return key_; }

  private:
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_KEY_HANDLE key_ = nullptr;
};

inline bool aes_gcm_encrypt(BCRYPT_KEY_HANDLE key, const uint8_t *nonce, const uint8_t *aad, size_t aad_len,
                            const uint8_t *plain, size_t plain_len, uint8_t *cipher, uint8_t *tag) {
    if (!key || !nonce || !tag || (!plain && plain_len) || (!cipher && plain_len))
        return false;
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = const_cast<PUCHAR>(nonce);
    info.cbNonce = static_cast<ULONG>(kNonceLen);
    info.pbAuthData = const_cast<PUCHAR>(aad);
    info.cbAuthData = static_cast<ULONG>(aad_len);
    info.pbTag = tag;
    info.cbTag = static_cast<ULONG>(kTagLen);

    ULONG written = 0;
    // GCM 是流模式：密文长度 == 明文长度，缓冲可同址。
    const NTSTATUS st = ::BCryptEncrypt(key, const_cast<PUCHAR>(plain), static_cast<ULONG>(plain_len), &info, nullptr, 0,
                                        cipher, static_cast<ULONG>(plain_len), &written, 0);
    return st >= 0 && written == plain_len;
}

inline bool aes_gcm_decrypt(BCRYPT_KEY_HANDLE key, const uint8_t *nonce, const uint8_t *aad, size_t aad_len,
                            const uint8_t *cipher, size_t cipher_len, const uint8_t *tag, uint8_t *plain) {
    if (!key || !nonce || !tag || (!cipher && cipher_len) || (!plain && cipher_len))
        return false;
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = const_cast<PUCHAR>(nonce);
    info.cbNonce = static_cast<ULONG>(kNonceLen);
    info.pbAuthData = const_cast<PUCHAR>(aad);
    info.cbAuthData = static_cast<ULONG>(aad_len);
    info.pbTag = const_cast<PUCHAR>(tag);
    info.cbTag = static_cast<ULONG>(kTagLen);

    ULONG written = 0;
    const NTSTATUS st = ::BCryptDecrypt(key, const_cast<PUCHAR>(cipher), static_cast<ULONG>(cipher_len), &info, nullptr, 0,
                                        plain, static_cast<ULONG>(cipher_len), &written, 0);
    return st >= 0 && written == cipher_len;
}

inline void put_u32(uint8_t *p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

inline void put_u64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        p[i] = static_cast<uint8_t>(v >> (8 * i));
}

inline uint32_t get_u32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t get_u64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i)
        v = (v << 8) | p[i];
    return v;
}

// 生成密封容器。password 为空返回空。
inline std::vector<uint8_t> seal(const char magic[kMagicLen], const std::string &password,
                                 const uint8_t *plain, size_t plain_len) {
    std::vector<uint8_t> out;
    if (!magic || password.empty() || (!plain && plain_len) || plain_len > (1ull << 32))
        return out;

    const auto salt = random_bytes(kSaltLen);
    const auto nonce = random_bytes(kNonceLen);
    if (salt.size() != kSaltLen || nonce.size() != kNonceLen)
        return out;

    uint8_t key[32] = {};
    if (!pbkdf2_sha256(password, salt.data(), salt.size(), kPbkdf2Iterations, key, sizeof(key)))
        return out;

    out.resize(kHeaderLen + plain_len);
    std::memcpy(out.data(), magic, kMagicLen);
    put_u32(out.data() + kMagicLen, kSealVersion);
    put_u64(out.data() + kMagicLen + 4, static_cast<uint64_t>(plain_len));
    std::memcpy(out.data() + kMagicLen + 12, salt.data(), kSaltLen);
    std::memcpy(out.data() + kMagicLen + 12 + kSaltLen, nonce.data(), kNonceLen);
    uint8_t *tag = out.data() + kMagicLen + 12 + kSaltLen + kNonceLen;
    uint8_t *cipher = out.data() + kHeaderLen;

    AesGcmKey aes;
    bool ok = aes.init(key);
    if (ok)
        ok = aes_gcm_encrypt(aes.get(), nonce.data(), out.data(), kMagicLen + 12, plain, plain_len, cipher, tag);
    secure_zero(key, sizeof(key));
    if (!ok) {
        out.clear();
        return out;
    }
    return out;
}

// 打开密封容器。magic 不符/版本不符/密码错/被篡改 -> false（plain 清空）。
inline bool unseal(const uint8_t *blob, size_t blob_len, const char magic[kMagicLen], const std::string &password,
                   std::vector<uint8_t> &plain) {
    plain.clear();
    if (!blob || !magic || password.empty() || blob_len < kHeaderLen)
        return false;
    if (std::memcmp(blob, magic, kMagicLen) != 0)
        return false;
    if (get_u32(blob + kMagicLen) != kSealVersion)
        return false;
    const uint64_t plain_size = get_u64(blob + kMagicLen + 4);
    if (plain_size != blob_len - kHeaderLen)
        return false;

    const uint8_t *salt = blob + kMagicLen + 12;
    const uint8_t *nonce = salt + kSaltLen;
    const uint8_t *tag = nonce + kNonceLen;
    const uint8_t *cipher = blob + kHeaderLen;

    uint8_t key[32] = {};
    if (!pbkdf2_sha256(password, salt, kSaltLen, kPbkdf2Iterations, key, sizeof(key)))
        return false;

    plain.resize(static_cast<size_t>(plain_size));
    AesGcmKey aes;
    bool ok = aes.init(key);
    if (ok)
        ok = aes_gcm_decrypt(aes.get(), nonce, blob, kMagicLen + 12, cipher, static_cast<size_t>(plain_size), tag,
                             plain.data());
    secure_zero(key, sizeof(key));
    if (!ok) {
        plain.clear();
        return false;
    }
    return true;
}

inline std::string base64_encode(const uint8_t *data, size_t len) {
    if (!data && len)
        return {};
    DWORD chars = 0;
    if (!::CryptBinaryToStringA(data, static_cast<DWORD>(len), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr,
                                &chars))
        return {};
    std::string out(chars, '\0');
    if (!::CryptBinaryToStringA(data, static_cast<DWORD>(len), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(),
                                &chars))
        return {};
    out.resize(chars);
    while (!out.empty() && out.back() == '\0')
        out.pop_back();
    return out;
}

inline bool base64_decode(const std::string &text, std::vector<uint8_t> &out) {
    out.clear();
    if (text.empty())
        return false;
    DWORD bytes = 0;
    if (!::CryptStringToBinaryA(text.c_str(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64, nullptr, &bytes,
                                nullptr, nullptr) ||
        bytes == 0)
        return false;
    out.resize(bytes);
    return ::CryptStringToBinaryA(text.c_str(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64, out.data(), &bytes,
                                  nullptr, nullptr) != FALSE;
}

inline bool read_file(const std::wstring &path, std::vector<uint8_t> &out) {
    out.clear();
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(h, &size) || size.QuadPart < 0 || size.QuadPart > (1ll << 32)) {
        ::CloseHandle(h);
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    size_t done = 0;
    while (done < out.size()) {
        DWORD got = 0;
        const DWORD want = static_cast<DWORD>(std::min<size_t>(out.size() - done, 1u << 20));
        if (!::ReadFile(h, out.data() + done, want, &got, nullptr) || got == 0) {
            out.clear();
            ::CloseHandle(h);
            return false;
        }
        done += got;
    }
    ::CloseHandle(h);
    return true;
}

inline bool write_file(const std::wstring &path, const uint8_t *data, size_t size) {
    if (!data && size)
        return false;
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    size_t done = 0;
    while (done < size) {
        DWORD wrote = 0;
        const DWORD want = static_cast<DWORD>(std::min<size_t>(size - done, 1u << 20));
        if (!::WriteFile(h, data + done, want, &wrote, nullptr) || wrote == 0) {
            ::CloseHandle(h);
            return false;
        }
        done += wrote;
    }
    ::CloseHandle(h);
    return true;
}

} // namespace op::cryptobox