// OpFile.cpp: 文件模块实现（P1 新增 2026-10-15，参考 OLA 文件模块常用子集）。
// 全部为无状态纯 Win32/STD 操作，不依赖 OpContext。
#include "OpResult.h"

#include "../base/WindowsHandle.h"

#include <libop.h>

#include <Windows.h>

// Windows.h 宏会把 Op::CopyFile/MoveFile/DeleteFile 的定义名改写为 *W 后缀，
// 导致与 libop.h 中的声明不匹配（C2888），此处局部取消映射。
#undef CopyFile
#undef MoveFile
#undef DeleteFile

#include <algorithm>
#include <string>
#include <vector>

namespace {

// CP_UTF8 双向转换（Utils 只有 utf8_to_ansi（经 CP_ACP），无 wstring 版；
// MiniImage.h 的 utf8_to_wstring 属图像模块命名空间，文件模块自含一份）。
std::wstring utf8_to_wstring(const std::string &s) {
    if (s.empty())
        return L"";
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0)
        return L"";
    std::wstring out(n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string wstring_to_utf8(const std::wstring &s) {
    if (s.empty())
        return "";
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return "";
    std::string out(n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

// 规范化路径中的正斜杠并剔除首尾空白；空输入返回 false。
bool normalize_path(const wchar_t *path, std::wstring &out) {
    if (!path)
        return false;
    out = path;
    const auto first = out.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        out.clear();
        return false;
    }
    const auto last = out.find_last_not_of(L" \t\r\n");
    out = out.substr(first, last - first + 1);
    std::replace(out.begin(), out.end(), L'/', L'\\');
    return !out.empty();
}

// 逐级创建目录（已存在视为成功）。
bool create_folder_recursive(const std::wstring &folder) {
    if (folder.empty())
        return false;
    if (::CreateDirectoryW(folder.c_str(), nullptr))
        return true;
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        // 已存在但须确认它是目录（同名文件应判失败）
        const DWORD attr = ::GetFileAttributesW(folder.c_str());
        return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
    }
    if (::GetLastError() != ERROR_PATH_NOT_FOUND)
        return false;
    // 父目录不存在：递归创建父级后重试
    const auto pos = folder.find_last_of(L'\\');
    if (pos == std::wstring::npos || pos == 0)
        return false;
    if (!create_folder_recursive(folder.substr(0, pos)))
        return false;
    return ::CreateDirectoryW(folder.c_str(), nullptr) != FALSE || ::GetLastError() == ERROR_ALREADY_EXISTS;
}

// 深度优先递归删除目录树（文件 + 子目录）。
bool delete_folder_recursive(const std::wstring &folder) {
    const std::wstring search = folder + L"\\*";
    WIN32_FIND_DATAW fd = {};
    HANDLE find = ::FindFirstFileW(search.c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE)
        return ::RemoveDirectoryW(folder.c_str()) != FALSE;
    bool ok = true;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..")
            continue;
        const std::wstring full = folder + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!delete_folder_recursive(full))
                ok = false;
        } else if (::DeleteFileW(full.c_str()) == FALSE) {
            ok = false;
        }
    } while (::FindNextFileW(find, &fd));
    ::FindClose(find);
    if (::RemoveDirectoryW(folder.c_str()) == FALSE)
        ok = false;
    return ok;
}

// UTF-8 文本读取：带 BOM 则剔除，按 CP_UTF8 解码。
bool read_utf8_file(const std::wstring &path, std::wstring &out) {
    const op::win32::unique_handle file(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file)
        return false;
    const DWORD file_size = ::GetFileSize(file.get(), nullptr);
    if (file_size == INVALID_FILE_SIZE)
        return false;
    if (file_size == 0) {
        out.clear();
        return true;
    }
    std::string raw(file_size, '\0');
    DWORD read = 0;
    if (::ReadFile(file.get(), raw.data(), file_size, &read, nullptr) == FALSE)
        return false;
    raw.resize(read);
    // 剔除 UTF-8 BOM
    if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF && static_cast<unsigned char>(raw[1]) == 0xBB &&
        static_cast<unsigned char>(raw[2]) == 0xBF)
        raw.erase(0, 3);
    out = utf8_to_wstring(raw);
    return true;
}

// UTF-8 文本写入：带 BOM 覆盖写（与 ReadFileString 配对，脚本编辑器友好）。
bool write_utf8_file(const std::wstring &path, const std::wstring &content) {
    const op::win32::unique_handle file(::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                                      FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file)
        return false;
    const std::string utf8 = wstring_to_utf8(content);
    const char bom[] = {'\xEF', '\xBB', '\xBF'};
    DWORD written = 0;
    if (::WriteFile(file.get(), bom, 3, &written, nullptr) == FALSE)
        return false;
    if (!utf8.empty() &&
        ::WriteFile(file.get(), utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr) == FALSE)
        return false;
    return true;
}

} // namespace

void op::Op::FileExists(const wchar_t *file_name, long *ret) {
    internal::set_result(ret, 0L);
    std::wstring path;
    if (!normalize_path(file_name, path))
        return;
    const DWORD attr = ::GetFileAttributesW(path.c_str());
    internal::set_result(ret, attr != INVALID_FILE_ATTRIBUTES ? 1L : 0L);
}

void op::Op::CreateFolder(const wchar_t *folder_name, long *ret) {
    internal::set_result(ret, 0L);
    std::wstring path;
    if (!normalize_path(folder_name, path))
        return;
    internal::set_result(ret, create_folder_recursive(path) ? 1L : 0L);
}

void op::Op::DeleteFolder(const wchar_t *folder_name, long *ret) {
    internal::set_result(ret, 0L);
    std::wstring path;
    if (!normalize_path(folder_name, path))
        return;
    const DWORD attr = ::GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
        return;
    internal::set_result(ret, delete_folder_recursive(path) ? 1L : 0L);
}

void op::Op::GetFileList(const wchar_t *folder, const wchar_t *wildcard, std::wstring &retstr) {
    retstr.clear();
    std::wstring dir;
    if (!normalize_path(folder, dir))
        return;
    const std::wstring pattern = dir + L"\\" + (wildcard && wildcard[0] ? wildcard : L"*");
    WIN32_FIND_DATAW fd = {};
    HANDLE find = ::FindFirstFileW(pattern.c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE)
        return;
    std::wstring result;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..")
            continue;
        if (!result.empty())
            result.push_back(L'|');
        result += dir + L"\\" + name;
    } while (::FindNextFileW(find, &fd));
    ::FindClose(find);
    retstr = result;
}

void op::Op::CopyFile(const wchar_t *src_file, const wchar_t *dst_file, long *ret) {
    internal::set_result(ret, 0L);
    std::wstring src, dst;
    if (!normalize_path(src_file, src) || !normalize_path(dst_file, dst))
        return;
    internal::set_result(ret, ::CopyFileW(src.c_str(), dst.c_str(), FALSE) != FALSE ? 1L : 0L);
}

void op::Op::MoveFile(const wchar_t *src_file, const wchar_t *dst_file, long *ret) {
    internal::set_result(ret, 0L);
    std::wstring src, dst;
    if (!normalize_path(src_file, src) || !normalize_path(dst_file, dst))
        return;
    internal::set_result(ret, ::MoveFileW(src.c_str(), dst.c_str()) != FALSE ? 1L : 0L);
}

void op::Op::DeleteFile(const wchar_t *file_name, long *ret) {
    internal::set_result(ret, 0L);
    std::wstring path;
    if (!normalize_path(file_name, path))
        return;
    internal::set_result(ret, ::DeleteFileW(path.c_str()) != FALSE ? 1L : 0L);
}

void op::Op::GetFileSize(const wchar_t *file_name, int64_t *size, long *ret) {
    internal::set_result(ret, 0L);
    if (size)
        *size = 0;
    std::wstring path;
    if (!normalize_path(file_name, path) || !size)
        return;
    const op::win32::unique_handle file(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file)
        return;
    LARGE_INTEGER file_size = {};
    if (::GetFileSizeEx(file.get(), &file_size) == FALSE)
        return;
    *size = static_cast<int64_t>(file_size.QuadPart);
    internal::set_result(ret, 1L);
}

void op::Op::ReadFileString(const wchar_t *file_name, std::wstring &retstr) {
    retstr.clear();
    std::wstring path;
    if (!normalize_path(file_name, path))
        return;
    read_utf8_file(path, retstr);
}

void op::Op::WriteStringToFile(const wchar_t *file_name, const wchar_t *content, long *ret) {
    internal::set_result(ret, 0L);
    std::wstring path;
    if (!normalize_path(file_name, path))
        return;
    internal::set_result(ret, write_utf8_file(path, content ? content : L"") ? 1L : 0L);
}

void op::Op::GetFileName(const wchar_t *file_path, long flag, std::wstring &retstr) {
    retstr.clear();
    std::wstring path;
    if (!normalize_path(file_path, path))
        return;
    // 去掉结尾斜杠后再拆目录与文件名
    while (path.size() > 1 && path.back() == L'\\')
        path.pop_back();
    const auto slash = path.find_last_of(L'\\');
    const std::wstring dir = slash == std::wstring::npos ? L"" : path.substr(0, slash);
    const std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const auto dot = name.find_last_of(L'.');
    const bool has_ext = dot != std::wstring::npos && dot != 0; // ".gitignore" 视为无扩展名
    switch (flag) {
    case 0:
        retstr = name;
        break;
    case 1:
        retstr = has_ext ? name.substr(0, dot) : name;
        break;
    case 2:
        retstr = has_ext ? name.substr(dot) : L"";
        break;
    case 3:
        retstr = dir;
        break;
    default:
        break;
    }
}

void op::Op::ToAbsolutePath(const wchar_t *path, std::wstring &retstr) {
    retstr.clear();
    std::wstring input;
    if (!normalize_path(path, input))
        return;
    const DWORD needed = ::GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
    if (needed == 0)
        return;
    std::wstring buffer(needed - 1, L'\0');
    if (::GetFullPathNameW(input.c_str(), needed, buffer.data(), nullptr) == 0)
        return;
    retstr = buffer;
}
