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
    bool was_abandoned() const {
        return _abandoned;
    }
    void unlock() {
        assert(_hmutex);
        if (!_hmutex || _lock_count == 0)
            return;
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
