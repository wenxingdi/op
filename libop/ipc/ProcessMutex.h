#pragma once
#include "../base/WindowsHandle.h"
#include <assert.h>
#include <string>
#include <utility>
#include <windows.h>
namespace op {

class ProcessMutex {
  public:
    explicit ProcessMutex() = default;
    ~ProcessMutex() {
        close();
    }

    ProcessMutex(const ProcessMutex &) = delete;
    ProcessMutex &operator=(const ProcessMutex &) = delete;

    bool open_create(const std::wstring &name_) {
        close();

        op::win32::unique_handle temp(OpenMutexW(MUTEX_ALL_ACCESS, FALSE, name_.c_str()));
        if (!temp) {
            temp.reset(CreateMutexW(NULL, FALSE, name_.c_str()));
        }
        if (temp) {
            _hmutex = std::move(temp);
            return true;
        } else {
            return false;
        }
    }
    bool open(const std::wstring &name_) {
        close();

        op::win32::unique_handle temp(OpenMutexW(MUTEX_ALL_ACCESS, FALSE, name_.c_str()));
        if (temp) {
            _hmutex = std::move(temp);
            return true;
        }
        return false;
    }
    void lock() {
        _abandoned = false;
        const DWORD result = ::WaitForSingleObject(_hmutex.get(), INFINITE);
        if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) {
            // WAIT_ABANDONED: a previous (cross-process) owner crashed without
            // releasing. Ownership is transferred to us, but the guarded shared
            // state may be inconsistent — query was_abandoned() before trusting it.
            if (result == WAIT_ABANDONED)
                _abandoned = true;
            ++_lock_count;
        }
    }

    DWORD try_lock(size_t time_) {
        _abandoned = false;
        const DWORD result = ::WaitForSingleObject(_hmutex.get(), static_cast<DWORD>(time_));
        if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) {
            if (result == WAIT_ABANDONED)
                _abandoned = true;
            ++_lock_count;
            // Return WAIT_OBJECT_0 so callers treat the lock as acquired and always
            // release it via unlock(); use was_abandoned() to decide whether the
            // protected shared state is still trustworthy.
            return WAIT_OBJECT_0;
        }
        return result;
    }

    // True if the most recent successful lock()/try_lock() was granted on an
    // abandoned mutex (previous owner terminated mid-section). The shared state
    // behind the mutex may be half-written / corrupt.
    // 读取侧不逐个查询本标志：DX 帧读取靠 FrameInfo 校验和 + hwnd/宽高合法性
    // 三重校验兜底，撕裂/半写帧过不了校验和即判"无帧"，故 abandoned 的共享
    // 内存内容不会向上传递；此处保留查询能力供调试与新链路评估。
    bool was_abandoned() const {
        return _abandoned;
    }
    void unlock() {
        // 守卫须先于 assert：op_test 构建（无 NDEBUG，assert 生效）下
        // "unlock 未 open 的锁"应安全返回，而不是中止测试进程。
        if (!_hmutex || _lock_count == 0) {
            assert(!_hmutex || _lock_count == 0);
            return;
        }
        ::ReleaseMutex(_hmutex.get());
        --_lock_count;
    }

    void close() {
        while (_lock_count > 0)
            unlock();
        _hmutex.reset();
    }

  protected:
  private:
    op::win32::unique_handle _hmutex;
    size_t _lock_count{0};
    bool _abandoned{false};
};

} // namespace op
