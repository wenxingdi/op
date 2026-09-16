
// #include "stdafx.h"

#include "HookCapture.h"
#include "../../base/AutomationModes.h"
#include "../../base/Environment.h"
#include "../../base/Utils.h"
#include "../../hook/HookModule.h"
#include "../../hook/SharedFrame.h"
#include "BlackBone/Process/Process.h"
#include "BlackBone/Process/RPC/RemoteFunction.hpp"
#include <cstddef>
#include <exception>
#include <memory>
#include <span>

#include "../../image/Image.h"

#include <sstream>

using op::capture::FrameInfo;

namespace {

constexpr DWORD kHookFrameReadyTimeoutMs = 200;
constexpr DWORD kHookFramePollIntervalMs = 10;

void release_remote_display_hook(blackbone::Process &proc, const std::wstring &dllname) {
    using release_display_hook_t = long(__stdcall *)(void);
    auto release = blackbone::MakeRemoteFunction<release_display_hook_t>(proc, dllname, "ReleaseDisplayHook");
    if (release) {
        release();
    }
}

// 在目标进程远端设置/清除 DLL 搜索目录。blackbone Inject 走 LoadLibraryW(绝对路径)，
// 被注入 DLL 的依赖(如 onnxruntime.dll 位于 op 库目录)按目标进程标准搜索序
// (exe 目录->cwd->系统->PATH)解析、不含 m_opPath，缺失即 0xC0000135 注入失败。
// 用法：注入前 SetDllDirectoryW(m_opPath)，LoadLibrary 完成依赖解析后传 nullptr 恢复。
// 注意：AsmVariant 对 const wchar_t* 深拷贝到远端(dataPtr)，RPC 安全。
bool set_remote_dll_search_dir(blackbone::Process &proc, const wchar_t *dir) {
    using set_dll_dir_t = BOOL(__stdcall *)(const wchar_t *);
    try {
        auto pSetDllDir = blackbone::MakeRemoteFunction<set_dll_dir_t>(proc, L"kernel32.dll", "SetDllDirectoryW");
        if (!pSetDllDir) {
            if (dir) {
                setlog(L"remote SetDllDirectoryW not resolvable.");
            }
            return false;
        }
        return pSetDllDir(dir).result() != FALSE;
    } catch (const std::exception &e) {
        if (dir) {
            setlog(L"remote SetDllDirectoryW exception: %S (dir may still have taken effect)", e.what());
        }
        return false;
    } catch (...) {
        if (dir) {
            setlog(L"remote SetDllDirectoryW unknown exception (dir may still have taken effect).");
        }
        return false;
    }
}

struct HookFrameView {
    FrameInfo *info = nullptr;
    std::span<std::byte> pixels;
};

bool isHookFrameReady(const FrameInfo &info, HWND hwnd) {
    FrameInfo expected = info;
    const auto chk = expected.chk;
    expected.fmtChk();
    return chk == expected.chk && info.hwnd == reinterpret_cast<unsigned __int64>(hwnd) && info.width > 0 &&
           info.height > 0;
}

HookFrameView makeHookFrameView(op::SharedMemory &sharedMemory, int width, int height) {
    auto *base = sharedMemory.data<std::byte>();
    auto *info = reinterpret_cast<FrameInfo *>(base);
    const auto pixelBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;

    // 使用 span 表达像素区，后续行拷贝不再直接散落裸指针偏移。
    return {info, {base + sizeof(FrameInfo), pixelBytes}};
}

std::span<const std::byte> hookFrameRow(std::span<const std::byte> pixels, int srcWidth, int row, int x, int width) {
    const auto offset = (static_cast<size_t>(row) * static_cast<size_t>(srcWidth) + static_cast<size_t>(x)) * 4;
    const auto bytes = static_cast<size_t>(width) * 4;
    return pixels.subspan(offset, bytes);
}

} // namespace

namespace op::capture {

HookCapture::HookCapture() : ICaptureBackend(), m_opPath(RuntimeEnvironment::getBasePath()) {
}

HookCapture::~HookCapture() {
    // do clear
    UnBindEx();
}

long HookCapture::BindEx(HWND hwnd, long render_type) {
    // setlog("BindEx");
    _hwnd = hwnd;
    long bind_ret = 0;
    if (render_type == RDT_GL_NOX) {
        bind_ret = BindNox(hwnd, render_type);
    } else {
        _render_type = render_type;
        RECT rc;
        // 获取客户区大小
        ::GetClientRect(hwnd, &rc);
        _width = rc.right - rc.left;
        _height = rc.bottom - rc.top;
        // bind_init();
        if (render_type == RDT_GL_NOX) {
        }
        DWORD id;
        ::GetWindowThreadProcessId(_hwnd, &id);

        // attach 进程
        blackbone::Process proc;
        NTSTATUS hr;

        hr = proc.Attach(id);

        if (NT_SUCCESS(hr)) {
            BOOL is64 = proc.modules().GetMainModule()->type == blackbone::eModType::mt_mod64;
            wstring dllname = op::hook::ResolveHookModuleName(is64 != FALSE);

            bool injected = false;
            // 判断是否已经注入
            auto _dllptr = proc.modules().GetModule(dllname);
            if (_dllptr) {
                injected = true;
            } else {
                wstring opFile = m_opPath + L"\\" + dllname;
                if (::PathFileExistsW(opFile.data())) {
                    // 注入前补依赖搜索目录（见 set_remote_dll_search_dir 注释）。dir_ok=false 不代表未生效：
                    // blackbone RPC 执行成功后取结果可能抛异常被 catch（实际目录已设置，Inject 随之成功），
                    // 故恢复必须无条件执行，避免目标进程搜索路径残留 op 目录。
                    const bool dir_ok = set_remote_dll_search_dir(proc, m_opPath.c_str());
                    if (!dir_ok) {
                        setlog(L"SetDllDirectoryW(remote) reported failure, proceed anyway. pid=%d", id);
                    }
                    auto iret = proc.modules().Inject(opFile);
                    set_remote_dll_search_dir(proc, nullptr);  // 恢复搜索路径，忽略结果
                    injected = (iret ? true : false);
                    if (!injected) {
                        setlog(L"Inject failed. pid=%d hwnd=%d status=0x%X dll=%s", id, _hwnd, iret.status,
                               opFile.c_str());
                    }
                } else {
                    setlog(L"file:<%s> not exists!", opFile.data());
                }
            }
            if (injected) {
                // setlog("before MakeRemoteFunction");
                using my_func_t = long(__stdcall *)(HWND, int);
                auto pSetXHook = blackbone::MakeRemoteFunction<my_func_t>(proc, dllname, "SetDisplayHook");
                if (pSetXHook) {
                    // setlog("after MakeRemoteFunction");
                    auto cret = pSetXHook(hwnd, render_type);
                    // setlog("after pSetXHook");
                    bind_ret = cret.result();
                    // setlog("after result");
                    if (bind_ret != 1) {
                        release_remote_display_hook(proc, dllname);
                    }
                } else {
                    setlog(L"remote function 'SetDisplayHook' not found in %s.", dllname.c_str());
                }
            } else {
                setlog(L"Inject false.");
            }
        } else {
            setlog(L"attach false. pid=%d hwnd=%d hr=0x%X", id, _hwnd, hr);
        }

        proc.Detach();
        // setlog("after Detach");
    }

    if (bind_ret == -1) {
        setlog("UnknownError");
    } else if (bind_ret == -2) {
        setlog("NotSupportedError");
    } else if (bind_ret == -3) {
        setlog("ModuleNotFoundError");
    }
    return bind_ret;
}

long HookCapture::UnBindEx() {
    // setlog("bkdo::UnBindEx()");
    if (_render_type == RDT_GL_NOX)
        return UnBindNox();
    DWORD id;
    ::GetWindowThreadProcessId(_hwnd, &id);

    // attach 进程s
    blackbone::Process proc;
    NTSTATUS hr;
    // setlog("bkdo::Attach");
    hr = proc.Attach(id);

    if (NT_SUCCESS(hr)) {
        BOOL is64 = proc.modules().GetMainModule()->type == blackbone::eModType::mt_mod64;
        wstring dllname = op::hook::ResolveHookModuleName(is64 != FALSE);
        // setlog(L"bkdo::dllname=%s",dllname);
        using my_func_t = long(__stdcall *)(void);
        auto pUnXHook = blackbone::MakeRemoteFunction<my_func_t>(proc, dllname, "ReleaseDisplayHook");
        if (pUnXHook) {
            // setlog(L"bkdo::pUnXHook");
            pUnXHook();
            // BOOL fret = ::FreeLibrary((HMODULE)proc.modules().GetModule(dllname)->baseAddress);
            // if (!fret)setlog("fret=%d", fret);
            /*proc.modules().RemoveManualModule(dllname,
                is64 ? blackbone::eModType::mt_mod64 : blackbone::eModType::mt_mod32);*/
        } else {
            setlog(L"get unhook ptr false.");
        }
    } else {
        setlog("blackbone::MakeRemoteFunction false,errcode:%X,pid=%d,hwnd=%d", hr, id, _hwnd);
    }
    // setlog(L"bkdo::Detach");
    proc.Detach();
    // bind_release();
    return 1;
}

void HookCapture::waitForBindReady() {
    if (!_pmutex || !_shmem) {
        return;
    }

    // 远端 hook 成功后，要等目标进程下一帧写入共享内存。
    const auto start = ::GetTickCount64();
    do {
        FrameInfo info = {};
        getFrameInfo(info);
        if (isHookFrameReady(info, _hwnd)) {
            return;
        }
        ::Sleep(kHookFramePollIntervalMs);
    } while (::GetTickCount64() - start < kHookFrameReadyTimeoutMs);
}

long HookCapture::BindNox(HWND hwnd, long render_type) {
    _render_type = render_type;
    _hwnd = hwnd;
    RECT rc;
    // 获取客户区大小
    ::GetClientRect(hwnd, &rc);
    _width = rc.right - rc.left;
    _height = rc.bottom - rc.top;
    // bind_init();

    // attach 进程
    blackbone::Process proc;
    NTSTATUS hr = -1;

    wstring dllname = L"op_x64.dll";

    hr = proc.Attach(L"NoxVMHandle.exe");

    long bind_ret = 0;

    if (NT_SUCCESS(hr)) {
        /*_process.Resume();*/
        bool injected = false;
        // 判断是否已经注入
        auto _dllptr = proc.modules().GetModule(dllname);
        if (_dllptr) {
            injected = true;
        } else {
            wstring opFile = m_opPath + L"\\" + dllname;
            if (::PathFileExistsW(opFile.data())) {
                const bool dir_ok = set_remote_dll_search_dir(proc, m_opPath.c_str());
                if (!dir_ok) {
                    setlog(L"SetDllDirectoryW(remote) reported failure, proceed anyway. pid=%d", proc.pid());
                }
                auto iret = proc.modules().Inject(opFile);
                set_remote_dll_search_dir(proc, nullptr);  // 无条件恢复搜索路径
                injected = (iret ? true : false);
                if (!injected) {
                    setlog(L"Inject failed. pid=%d hwnd=%d status=0x%X dll=%s", proc.pid(), _hwnd, iret.status,
                           opFile.c_str());
                }
            } else {
                setlog(L"file:<%s> not exists!", opFile.data());
            }
        }
        if (injected) {
            using my_func_t = long(__stdcall *)(HWND, int);
            auto pSetXHook = blackbone::MakeRemoteFunction<my_func_t>(proc, dllname, "SetDisplayHook");
            if (pSetXHook) {
                auto cret = pSetXHook(hwnd, render_type);
                bind_ret = cret.result();
                if (bind_ret != 1) {
                    release_remote_display_hook(proc, dllname);
                }
            } else {
                setlog(L"remote function not found.");
            }
        } else {
            setlog(L"Inject false.");
        }

    } else {
        setlog(L"attach false.");
    }
    proc.Detach();

    return bind_ret;
}

long HookCapture::UnBindNox() {

    // attach 进程
    blackbone::Process proc;
    NTSTATUS hr;

    hr = proc.Attach(L"NoxVMHandle.exe");
    wstring dllname = L"op_x64.dll";

    if (NT_SUCCESS(hr)) {

        using my_func_t = long(__stdcall *)(void);
        auto pUnXHook = blackbone::MakeRemoteFunction<my_func_t>(proc, dllname, "ReleaseDisplayHook");
        if (pUnXHook) {
            pUnXHook();

            /*BOOL fret = ::FreeLibrary((HMODULE)proc.modules().GetModule(dllname)->baseAddress);
            if (!fret)setlog("fret=%d", fret);*/
        } else {
            setlog(L"get unhook ptr false.");
        }
    } else {
        setlog("blackbone::MakeRemoteFunction false,errcode:%Xhwnd=%d", hr, _hwnd);
    }

    proc.Detach();

    return 1;
}

bool HookCapture::requestCapture(int x1, int y1, int w, int h, Image &img) {
    if (!_pmutex || !_shmem) {
        return false;
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        _pmutex->lock();
        FrameInfo *pInfo = reinterpret_cast<FrameInfo *>(_shmem->data<std::byte>());
        const FrameInfo info = pInfo ? *pInfo : FrameInfo{};
        if (!isHookFrameReady(info, _hwnd)) {
            _pmutex->unlock();
            // 目标进程没有向共享帧写入可用画面。hook 型显示模式(dx/dx2/opengl
            // 系列)依赖目标自身的 Present/SwapBuffers 调用：纯 GDI/Qt 软件绘制
            // 的窗口不会产生这类帧，表现就是 bind 成功但 capture/找色全空。
            setlog(L"hook frame not ready: hwnd=%p, target produced no present frame "
                   L"(use display 'normal'/'gdi'/'dx2' for GDI/Qt-rendered windows)",
                   static_cast<void *>(_hwnd));
            return false;
        }

        const int src_width = info.width > 0 ? (int)info.width : (int)_width;
        const int src_height = info.height > 0 ? (int)info.height : (int)_height;

        if (info.width > 0 && info.height > 0 &&
            ((long)info.width != _width || (long)info.height != _height)) {
            _width = (long)info.width;
            _height = (long)info.height;
        }

        if (!op::hook::SharedFrameHasCapacity(*_shmem, src_width, src_height)) {
            _pmutex->unlock();
            if (!ensureSharedFrameCapacity(src_width, src_height)) {
                return false;
            }
            waitForBindReady();
            continue;
        }

        if (src_width <= 0 || src_height <= 0 || x1 < 0 || y1 < 0 || w <= 0 || h <= 0 || x1 + w > src_width ||
            y1 + h > src_height) {
            _pmutex->unlock();
            return false;
        }

        img.create(w, h);
        auto frame = makeHookFrameView(*_shmem, src_width, src_height);

        // 远端写入共享帧的行顺序不完全等同于渲染大类。
        // DX 和 EGL/GLES 已经是窗口坐标方向；传统 OpenGL 仍按左下角原点翻转读取。
        const bool top_down_frame = GET_RENDER_TYPE(_render_type) == RENDER_TYPE::DX || _render_type == RDT_GL_ES;
        if (top_down_frame) {
            for (int i = 0; i < h; i++) {
                const auto row = hookFrameRow(frame.pixels, src_width, i + y1, x1, w);
                memcpy(img.ptr<uchar>(i), row.data(), row.size());
            }
        } else {
            for (int i = 0; i < h; i++) {
                const auto row = hookFrameRow(frame.pixels, src_width, src_height - 1 - i - y1, x1, w);
                memcpy(img.ptr<uchar>(i), row.data(), row.size());
            }
        }

        _pmutex->unlock();
        return true;
    }

    return false;
}

bool HookCapture::ensureSharedFrameCapacity(int width, int height) {
    const size_t required = op::hook::RequiredSharedFrameBytes(width, height);
    if (required == 0) {
        return false;
    }
    if (_shmem && _shmem->size() >= required) {
        return true;
    }

    if (!_pmutex) {
        auto mutex = std::make_unique<ProcessMutex>();
        if (!mutex->open_create(_mutex_name)) {
            return false;
        }
        _pmutex = mutex.release();
    }

    for (int attempt = 0; attempt < 20; ++attempt) {
        _pmutex->lock();
        SAFE_DELETE(_shmem);

        auto shmem = std::make_unique<SharedMemory>();
        const bool opened = shmem->open_create(_shared_res_name, required);
        if (opened && shmem->size() >= required) {
            _shmem = shmem.release();
            _pmutex->unlock();
            return true;
        }

        _pmutex->unlock();
        ::Sleep(10);
    }

    return false;
}

} // namespace op::capture
