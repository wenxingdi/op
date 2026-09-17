#include "ProcessMemory.h"
#include <Windows.h>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <sstream>

namespace op {

namespace {

template <typename T> T pop_back_value(vector<T> &items) {
    T value = items.back();
    items.pop_back();
    return value;
}

wstring bytesToWide(const vector<uchar> &bin, UINT cp) {
    if (bin.empty())
        return L"";
    const int len =
        ::MultiByteToWideChar(cp, 0, reinterpret_cast<LPCCH>(bin.data()), static_cast<int>(bin.size()), nullptr, 0);
    if (len <= 0)
        return L"";
    wstring out(len, L'\0');
    ::MultiByteToWideChar(cp, 0, reinterpret_cast<LPCCH>(bin.data()), static_cast<int>(bin.size()), &out[0], len);
    return out;
}

vector<uchar> wideToBytes(const wstring &text, UINT cp, bool nullTerminated) {
    const int len = ::WideCharToMultiByte(cp, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0)
        return {};
    vector<uchar> bin(static_cast<size_t>(len));
    ::WideCharToMultiByte(cp, 0, text.c_str(), -1, reinterpret_cast<LPSTR>(bin.data()), len, nullptr, nullptr);
    if (!nullTerminated && !bin.empty() && bin.back() == 0)
        bin.pop_back();
    return bin;
}

wstring decodeUnicodeBytes(const vector<uchar> &bin) {
    if (bin.size() < sizeof(wchar_t))
        return L"";
    const size_t chars = bin.size() / sizeof(wchar_t);
    wstring out(chars, L'\0');
    memcpy(&out[0], bin.data(), chars * sizeof(wchar_t));
    const size_t z = out.find(L'\0');
    if (z != wstring::npos)
        out.resize(z);
    return out;
}

vector<uchar> encodeUnicodeBytes(const wstring &text) {
    vector<uchar> bin((text.size() + 1) * sizeof(wchar_t));
    if (!text.empty())
        memcpy(bin.data(), text.c_str(), text.size() * sizeof(wchar_t));
    return bin;
}

size_t trimNullByteLength(const vector<uchar> &bin, size_t charWidth) {
    if (charWidth == 0 || bin.empty())
        return 0;
    for (size_t i = 0; i + charWidth <= bin.size(); i += charWidth) {
        bool zero = true;
        for (size_t j = 0; j < charWidth; ++j) {
            if (bin[i + j] != 0) {
                zero = false;
                break;
            }
        }
        if (zero)
            return i;
    }
    return bin.size();
}

bool address_has_operator(const wstring &address) {
    for (wchar_t c : address) {
        if (c == L'+' || c == L'-' || c == L'*' || c == L'/')
            return true;
    }
    return false;
}

size_t parse_hex_word(const wstring &num) {
    if (num.empty())
        return 0;
    wchar_t *end = nullptr;
    const unsigned long long v = wcstoull(num.c_str(), &end, 16);
    if (end == num.c_str())
        return 0;
#if defined(_WIN64)
    return static_cast<size_t>(v);
#else
    return static_cast<size_t>(static_cast<unsigned long>(v));
#endif
}

size_t do_op_ptr(size_t a, size_t b, wchar_t op) {
    switch (op) {
    case L'+':
        return a + b;
    case L'-':
        return a - b;
    case L'*':
        return a * b;
    case L'/':
        return b ? a / b : 0;
    default:
        return 0;
    }
}

wstring normalize_hex(const wstring &hex) {
    wstring out;
    out.reserve(hex.size());
    for (wchar_t c : hex) {
        if ((c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'F') || (c >= L'a' && c <= L'f'))
            out.push_back(static_cast<wchar_t>(towupper(c)));
    }
    return out;
}

wstring toUpperHex(uintptr_t value) {
    std::wstringstream out;
    out << std::uppercase << std::hex << value;
    return out.str();
}

} // namespace

namespace {

int get_op_prior(wchar_t op) {
    if (op == L'+' || op == L'-')
        return 0;
    if (op == L'*' || op == L'/')
        return 1;
    return 2;
}

bool is_op(wchar_t op) {
    return op == L'+' || op == L'-' || op == L'*' || op == L'/';
}

// like AA+BB+DD-CC*cc. Use size_t so x64 absolute addresses parse correctly.
size_t stringcompute(const wchar_t *s) {
    if (!s || !*s)
        return 0;
    wstring num;
    vector<size_t> ns;
    vector<wchar_t> os;
    while (true) {
        if (*s && !is_op(*s)) {
            num.push_back(*s++);
            continue;
        }

        if (!num.empty()) {
            ns.push_back(parse_hex_word(num));
            num.clear();
        }

        if (*s == 0)
            break;

        if (ns.empty()) {
            os.push_back(*s++);
            continue;
        }

        if (!os.empty()) {
            const wchar_t pending = os.back();
            if (get_op_prior(pending) >= get_op_prior(*s)) {
                const size_t num2 = pop_back_value(ns);
                const size_t num1 = pop_back_value(ns);
                const wchar_t popped = pop_back_value(os);
                ns.push_back(do_op_ptr(num1, num2, popped));
                continue;
            }
        }

        os.push_back(*s++);
    }

    while (!os.empty()) {
        if (ns.size() < 2)
            break;
        const size_t num2 = pop_back_value(ns);
        const size_t num1 = pop_back_value(ns);
        const wchar_t popped = pop_back_value(os);
        ns.push_back(do_op_ptr(num1, num2, popped));
    }

    if (ns.empty())
        return 0;
    return ns.back();
}

} // namespace

ProcessMemory::ProcessMemory() {
}

ProcessMemory::~ProcessMemory() {
}

long ProcessMemory::WriteData(HWND hwnd, const wstring &address, const wstring &data, LONG size) {
    if (size <= 0 || !checkaddress(address))
        return 0;
    // size 超出 data 实际字节数时按大漠兼容行为补零写入，但静默补零容易掩盖
    // 用户 size 手滑（多写一位就往目标进程写一串 0），这里必须留痕。
    const size_t available = normalize_hex(data).size() / 2;
    if (static_cast<size_t>(size) > available)
        setlog(L"WriteData: size=%ld 超出数据实际字节 %zu，不足部分补零写入", size, available);
    vector<uchar> bin;
    hex2bins(bin, data, size);
    return WriteRaw(hwnd, address, bin.data(), bin.size()) ? 1 : 0;
}

wstring ProcessMemory::ReadData(HWND hwnd, const wstring &address, LONG size) {
    if (size <= 0 || !checkaddress(address))
        return L"";
    vector<uchar> bin(static_cast<size_t>(size));
    wstring hex;
    if (!ReadRaw(hwnd, address, bin.data(), bin.size()))
        return L"";
    bin2hexs(bin, hex);
    return hex;
}

bool ProcessMemory::ReadRaw(HWND hwnd, const wstring &address, void *buf, size_t size) {
    if (!buf || size == 0 || !checkaddress(address))
        return false;
    if (!prepare_process(hwnd))
        return false;
    const size_t addr = str2address(address);
    if (addr == 0)
        return false;
    return mem_read(buf, addr, size);
}

bool ProcessMemory::WriteRaw(HWND hwnd, const wstring &address, const void *buf, size_t size) {
    if (!buf || size == 0 || !checkaddress(address))
        return false;
    if (!prepare_process(hwnd))
        return false;
    const size_t addr = str2address(address);
    if (addr == 0)
        return false;
    return mem_write(addr, const_cast<void *>(buf), size);
}

size_t ProcessMemory::IntTypeSize(long type) {
    switch (type) {
    case 1:
    case 5:
        return 2;
    case 2:
    case 6:
        return 1;
    case 3:
        return 8;
    case 0:
    case 4:
    default:
        return 4;
    }
}

bool ProcessMemory::ReadInt(HWND hwnd, const wstring &address, long type, int64_t *value) {
    if (value)
        *value = 0;
    if (!value)
        return false;

    const size_t sz = IntTypeSize(type);
    vector<uchar> bin(sz);
    if (!ReadRaw(hwnd, address, bin.data(), sz))
        return false;

    switch (type) {
    case 2:
        *value = static_cast<int64_t>(static_cast<int8_t>(bin[0]));
        break;
    case 6:
        *value = static_cast<int64_t>(bin[0]);
        break;
    case 1: {
        int16_t v = 0;
        memcpy(&v, bin.data(), sizeof(v));
        *value = v;
        break;
    }
    case 5: {
        uint16_t v = 0;
        memcpy(&v, bin.data(), sizeof(v));
        *value = static_cast<int64_t>(v);
        break;
    }
    case 3: {
        int64_t v = 0;
        memcpy(&v, bin.data(), sizeof(v));
        *value = v;
        break;
    }
    case 4: {
        uint32_t v = 0;
        memcpy(&v, bin.data(), sizeof(v));
        *value = static_cast<int64_t>(v);
        break;
    }
    case 0:
    default: {
        int32_t v = 0;
        memcpy(&v, bin.data(), sizeof(v));
        *value = static_cast<int64_t>(v);
        break;
    }
    }
    return true;
}

int64_t ProcessMemory::ReadInt(HWND hwnd, const wstring &address, long type) {
    int64_t value = 0;
    ReadInt(hwnd, address, type, &value);
    return value;
}

long ProcessMemory::WriteInt(HWND hwnd, const wstring &address, long type, int64_t value) {
    const size_t sz = IntTypeSize(type);
    vector<uchar> bin(sz);
    switch (type) {
    case 2:
        bin[0] = static_cast<uchar>(static_cast<int8_t>(value));
        break;
    case 6:
        bin[0] = static_cast<uchar>(value);
        break;
    case 1: {
        const int16_t v = static_cast<int16_t>(value);
        memcpy(bin.data(), &v, sizeof(v));
        break;
    }
    case 5: {
        const uint16_t v = static_cast<uint16_t>(value);
        memcpy(bin.data(), &v, sizeof(v));
        break;
    }
    case 3:
        memcpy(bin.data(), &value, sizeof(value));
        break;
    case 4: {
        const uint32_t v = static_cast<uint32_t>(value);
        memcpy(bin.data(), &v, sizeof(v));
        break;
    }
    case 0:
    default: {
        const int32_t v = static_cast<int32_t>(value);
        memcpy(bin.data(), &v, sizeof(v));
        break;
    }
    }
    return WriteRaw(hwnd, address, bin.data(), sz) ? 1 : 0;
}

bool ProcessMemory::ReadFloat(HWND hwnd, const wstring &address, float *value) {
    if (value)
        *value = 0.0f;
    if (!value)
        return false;
    return ReadRaw(hwnd, address, value, sizeof(*value));
}

float ProcessMemory::ReadFloat(HWND hwnd, const wstring &address) {
    float value = 0.0f;
    ReadFloat(hwnd, address, &value);
    return value;
}

long ProcessMemory::WriteFloat(HWND hwnd, const wstring &address, float value) {
    return WriteRaw(hwnd, address, &value, sizeof(value)) ? 1 : 0;
}

bool ProcessMemory::ReadDouble(HWND hwnd, const wstring &address, double *value) {
    if (value)
        *value = 0.0;
    if (!value)
        return false;
    return ReadRaw(hwnd, address, value, sizeof(*value));
}

double ProcessMemory::ReadDouble(HWND hwnd, const wstring &address) {
    double value = 0.0;
    ReadDouble(hwnd, address, &value);
    return value;
}

long ProcessMemory::WriteDouble(HWND hwnd, const wstring &address, double value) {
    return WriteRaw(hwnd, address, &value, sizeof(value)) ? 1 : 0;
}

wstring ProcessMemory::ReadString(HWND hwnd, const wstring &address, long type, long len) {
    const long maxAuto = 4096;
    // 显式 len 原本无上限（len=10 亿会一次分配 ~1GB），截断到 16MB 并留痕。
    const long kMaxExplicit = 16 * 1024 * 1024;
    long readLen = len > 0 ? len : maxAuto;
    if (len > kMaxExplicit) {
        setlog(L"ReadString: len=%ld 超上限，截断为 %ld", len, kMaxExplicit);
        readLen = kMaxExplicit;
    }
    if (readLen <= 0)
        return L"";
    vector<uchar> bin(static_cast<size_t>(readLen));
    if (!ReadRaw(hwnd, address, bin.data(), bin.size()))
        return L"";

    if (len <= 0) {
        // Auto-length reads stop at the first null character in the selected encoding.
        const size_t byteWidth = (type == 1) ? sizeof(wchar_t) : 1;
        const size_t used = trimNullByteLength(bin, byteWidth);
        bin.resize(used);
    }

    switch (type) {
    case 1:
        return decodeUnicodeBytes(bin);
    case 2:
        return bytesToWide(bin, CP_UTF8);
    case 0:
    default:
        return bytesToWide(bin, CP_ACP);
    }
}

long ProcessMemory::WriteString(HWND hwnd, const wstring &address, long type, const wstring &value) {
    vector<uchar> bin;
    // type follows the historical DM convention: 0=ACP/GBK, 1=UTF-16, 2=UTF-8.
    switch (type) {
    case 1:
        bin = encodeUnicodeBytes(value);
        break;
    case 2:
        bin = wideToBytes(value, CP_UTF8, true);
        break;
    case 0:
    default:
        bin = wideToBytes(value, CP_ACP, true);
        break;
    }
    if (bin.empty())
        return 0;
    return WriteRaw(hwnd, address, bin.data(), bin.size()) ? 1 : 0;
}

bool ProcessMemory::prepare_process(HWND hwnd) {
    _hwnd = hwnd;
    if (!_hwnd) {
        // 切回"读写当前进程"模式：立即释放之前附加的远程进程句柄
        // （否则旧 BlackBone 句柄会保持打开直到下次 Attach 或析构）
        _proc.Detach();
        return true;
    }
    if (!::IsWindow(hwnd))
        return false;

    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    // BlackBone Process::Attach 内部先 Detach 旧进程再 Open 新进程，
    // 切换窗口句柄不会泄漏旧进程对象。
    if (_proc.Attach(pid) < 0) {
        setlog(L"prepare_process: BlackBone Attach pid=%lu 失败", pid);
        return false;
    }
    return true;
}

bool ProcessMemory::mem_read(void *dst, size_t src, size_t size) {
    if (!dst || size == 0)
        return false;
    if (_hwnd) {
        return _proc.memory().Read(src, size, dst) >= 0;
    }
    SIZE_T read = 0;
    return ::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<LPCVOID>(src), dst, size, &read) && read == size;
}

bool ProcessMemory::mem_write(size_t dst, void *src, size_t size) {
    if (!src || size == 0)
        return false;
    if (_hwnd) {
        return _proc.memory().Write(dst, size, src) >= 0;
    }
    SIZE_T written = 0;
    return ::WriteProcessMemory(::GetCurrentProcess(), reinterpret_cast<LPVOID>(dst), src, size, &written) &&
           written == size;
}

bool ProcessMemory::checkaddress(const wstring &address) {
    vector<wchar_t> sk;
    auto p = address.data();
    while (*p) {
        if (*p == L'[') {
            sk.push_back(*p);
        } else if (*p == L']') {
            if (sk.empty())
                return false;
            sk.pop_back();
        }
        p++;
    }
    return sk.empty();
}

size_t ProcessMemory::str2address(const wstring &caddress) {
    wstring address = caddress;
    if (!checkaddress(address))
        return 0;

    vector<size_t> sk;
    size_t idx1 = address.find(L'<');
    size_t idx2 = address.find(L'>');
    if (idx1 != wstring::npos && idx2 != wstring::npos && idx1 < idx2) {
        auto mod_name = address.substr(idx1 + 1, idx2 - idx1 - 1);
        HMODULE hmod = NULL;
        if (_hwnd == 0)
            hmod = ::GetModuleHandleW(mod_name.data());
        else {
            auto mptr = _proc.modules().GetModule(mod_name);
            if (mptr)
                hmod = (HMODULE)mptr->baseAddress;
        }
        if (hmod == NULL) {
            setlog(L"str2address: 模块 <%s> 未找到", mod_name.c_str());
            return 0;
        }
        address.replace(idx1, idx2 - idx1 + 1, toUpperHex(reinterpret_cast<uintptr_t>(hmod)));
    }
    for (size_t i = 0; i < address.size();) {
        if (address[i] == L'[')
            sk.push_back(i);
        if (address[i] == L']') {
            idx1 = pop_back_value(sk);
            idx2 = i;
            auto sad = address.substr(idx1 + 1, idx2 - idx1 - 1);
            size_t src = stringcompute(sad.data());
            size_t next;
            if (!mem_read(&next, src, sizeof(size_t)) || next == 0)
                return 0;
            address.replace(idx1, idx2 - idx1 + 1, toUpperHex(next));
            i = idx1;
        }
        ++i;
    }
    if (address.find(L'[') == wstring::npos && !address_has_operator(address))
        return parse_hex_word(address);
    return stringcompute(address.data());
}

void ProcessMemory::hex2bins(vector<uchar> &bin, const wstring &hex, size_t size) {
    const wstring clean = normalize_hex(hex);
    const size_t available = clean.size() / 2;
    const size_t write_bytes = size > 0 ? size : available;
    bin.resize(write_bytes);
    ZeroMemory(bin.data(), bin.size());
    const size_t copy_bytes = available < write_bytes ? available : write_bytes;
    for (size_t i = 0; i < copy_bytes; ++i) {
        const int hi = hex2bin(clean[i * 2]);
        const int lo = hex2bin(clean[i * 2 + 1]);
        bin[i] = static_cast<uchar>((hi << 4) | lo);
    }
}

void ProcessMemory::bin2hexs(const vector<uchar> &bin, wstring &hex) {
    hex.reserve(bin.size() * 2);
    hex.clear();
    for (size_t i = 0; i < bin.size(); ++i) {
        int ans = bin2hex(bin[i]);
        hex.push_back(ans >> 8);
        hex.push_back(ans & 0xff);
    }
}

namespace {

// 解析特征码：去空白后按字节对解析，"??"（或 "? " 组合的两个问号）为通配，其余须为十六进制对。
// 返回 false 表示格式非法（空串/奇数长度/含非十六进制且非问号字符）。
bool is_hex_char(wchar_t c) {
    return (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'F') || (c >= L'a' && c <= L'f');
}

bool parse_pattern(const wstring &pattern, vector<uchar> &bytes, vector<uchar> &mask) {
    wstring clean;
    clean.reserve(pattern.size());
    for (wchar_t c : pattern) {
        if (c == L' ' || c == L'\t' || c == L',')
            continue;
        clean.push_back(c);
    }
    if (clean.empty() || clean.size() % 2 != 0)
        return false;
    const size_t n = clean.size() / 2;
    bytes.assign(n, 0);
    mask.assign(n, 0);
    for (size_t i = 0; i < n; ++i) {
        const wchar_t c0 = clean[i * 2];
        const wchar_t c1 = clean[i * 2 + 1];
        if (c0 == L'?' && c1 == L'?')
            continue; // 通配字节：mask 保持 0
        if (!is_hex_char(c0) || !is_hex_char(c1))
            return false;
        const int hi = hex2bin(towupper(c0));
        const int lo = hex2bin(towupper(c1));
        bytes[i] = static_cast<uchar>((hi << 4) | lo);
        mask[i] = 1;
    }
    return true;
}

// 解析 "start-end" 十六进制范围；空串 = [0, UINTPTR_MAX]。
bool parse_range(const wstring &range, uintptr_t &begin, uintptr_t &end) {
    begin = 0;
    end = UINTPTR_MAX;
    if (range.empty())
        return true;
    const size_t dash = range.find(L'-');
    if (dash == wstring::npos)
        return false;
    const wstring lo = range.substr(0, dash);
    const wstring hi = range.substr(dash + 1);
    if (!lo.empty())
        begin = static_cast<uintptr_t>(parse_hex_word(lo));
    if (!hi.empty())
        end = static_cast<uintptr_t>(parse_hex_word(hi));
    return end >= begin;
}

bool region_is_readable(const MEMORY_BASIC_INFORMATION &mbi) {
    if (mbi.State != MEM_COMMIT)
        return false;
    const DWORD p = mbi.Protect & 0xFF;
    switch (p) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false; // PAGE_NOACCESS / PAGE_GUARD / PAGE_EXECUTE（不可读）
    }
}

// 在 buf 内按 step 扫描特征码；anchor 为首个非通配字节下标（调用前已保证存在）。
// 候选起点 i 的范围 [0, buf_size-m]；anchor 字节位于 i+anchor，memchr 只在该窗口内找锚。
void scan_chunk(const uchar *buf, size_t buf_size, const vector<uchar> &bytes, const vector<uchar> &mask,
                size_t anchor, size_t step, vector<uintptr_t> &hits, uintptr_t chunk_base, size_t max_results) {
    const size_t m = bytes.size();
    if (buf_size < m)
        return;
    const uchar anchor_byte = bytes[anchor];
    const size_t start_hi = buf_size - m; // 特征码起点的最后一个合法下标
    for (size_t i = 0; i <= start_hi && hits.size() < max_results; i += step) {
        const size_t from = i + anchor;
        const uchar *p =
            static_cast<const uchar *>(memchr(buf + from, anchor_byte, start_hi - i + 1));
        if (!p)
            break;
        i = static_cast<size_t>(p - buf) - anchor; // i 回到特征码起点，末尾 i+=step 推进
        bool ok = true;
        for (size_t j = 0; j < m; ++j) {
            if (mask[j] && buf[i + j] != bytes[j]) {
                ok = false;
                break;
            }
        }
        if (ok)
            hits.push_back(chunk_base + i);
    }
}

} // namespace

wstring ProcessMemory::FindData(HWND hwnd, const wstring &range, const wstring &pattern, long step, long max_results) {
    const size_t kDefaultMax = 1024;
    const size_t kMaxPattern = 256;
    const size_t kChunk = 16 * 1024 * 1024;

    vector<uchar> bytes, mask;
    if (!parse_pattern(pattern, bytes, mask) || bytes.empty() || bytes.size() > kMaxPattern) {
        setlog(L"FindData: 非法特征码（空/奇数长度/超 %zu 字节/含非十六进制字符）", kMaxPattern);
        return L"";
    }
    size_t anchor = 0;
    while (anchor < mask.size() && !mask[anchor])
        ++anchor;
    if (anchor == mask.size())
        return L""; // 全通配无意义
    if (step <= 0)
        step = 1;
    const size_t max_hits = max_results > 0 ? static_cast<size_t>(max_results) : kDefaultMax;

    uintptr_t range_begin = 0, range_end = UINTPTR_MAX;
    if (!parse_range(range, range_begin, range_end)) {
        setlog(L"FindData: 非法范围 \"%s\"（应为 \"start-end\" 十六进制，且 end>=begin）", range.c_str());
        return L"";
    }

    // 搜索独立走 OpenProcess/VirtualQueryEx，不经 BlackBone（枚举与分块读只需 VM_READ）。
    HANDLE proc = ::GetCurrentProcess();
    HANDLE opened = NULL;
    DWORD pid = 0;
    if (hwnd) {
        if (!::IsWindow(hwnd)) {
            setlog(L"FindData: 无效窗口句柄 %p", hwnd);
            return L"";
        }
        ::GetWindowThreadProcessId(hwnd, &pid);
        opened = ::OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
        if (!opened) {
            setlog(L"FindData: OpenProcess pid=%lu 失败，错误 %lu", pid, ::GetLastError());
            return L"";
        }
        proc = opened;
    }

    vector<uintptr_t> hits;
    // 缓冲区按范围实际大小 lazy 分配（原实现无条件预分配 16MB，搜小范围也是 16MB）。
    size_t chunk_cap = kChunk;
    if (range_end - range_begin < kChunk)
        chunk_cap = static_cast<size_t>(range_end - range_begin) + 1;
    vector<uchar> chunk(chunk_cap);
    uintptr_t addr = range_begin;
    while (addr <= range_end && hits.size() < max_hits) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (::VirtualQueryEx(proc, reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) != sizeof(mbi))
            break;
        if (mbi.State == MEM_FREE && mbi.RegionSize == 0)
            break;
        const uintptr_t region_base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t region_end = region_base + static_cast<uintptr_t>(mbi.RegionSize) - 1;
        if (region_end < range_begin) { // 整个区域在范围之前
            addr = region_end + 1;
            continue;
        }
        if (region_base > range_end)
            break;
        const uintptr_t scan_lo_save = region_base > range_begin ? region_base : range_begin;
        if (scan_lo_save > range_end)
            break;
        if (region_is_readable(mbi)) {
            const uintptr_t scan_lo = scan_lo_save;
            const uintptr_t scan_hi = region_end < range_end ? region_end : range_end;
            uintptr_t cur = scan_lo;
            while (cur <= scan_hi && hits.size() < max_hits) {
                const uintptr_t remain = scan_hi - cur + 1;
                const size_t want = remain > chunk_cap ? chunk_cap : static_cast<size_t>(remain);
                SIZE_T got = 0;
                // 末尾多读 m-1 字节，避免跨块特征码漏检（读失败则按精确长度重试一次）
                size_t want_ext = want + (bytes.size() - 1);
                if (want_ext > remain)
                    want_ext = static_cast<size_t>(remain);
                if (!::ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(cur), chunk.data(), want_ext, &got) ||
                    got < want)
                    break;
                scan_chunk(chunk.data(), got, bytes, mask, anchor, static_cast<size_t>(step), hits, cur, max_hits);
                if (got < want_ext)
                    break; // 区域尾部，读不满说明到头
                cur += want;
            }
        }
        addr = region_end + 1;
        if (addr <= region_base)
            break; // region_end 已触顶（UINTPTR_MAX），防回绕死循环
    }
    if (opened)
        ::CloseHandle(opened);

    wstring out;
    for (size_t i = 0; i < hits.size(); ++i) {
        if (i)
            out.push_back(L'|');
        out += toUpperHex(hits[i]);
    }
    return out;
}

wstring ProcessMemory::GetModuleBaseAddr(HWND hwnd, const wstring &module) {
    if (module.empty())
        return L"";
    if (!prepare_process(hwnd))
        return L"";
    const size_t addr = str2address(L"<" + module + L">");
    if (addr == 0)
        return L"";
    return toUpperHex(addr);
}

} // namespace op
