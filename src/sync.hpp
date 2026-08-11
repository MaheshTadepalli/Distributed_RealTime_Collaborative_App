#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <winsock2.h>
#include <windows.h>
#else
#include <mutex>
#include <thread>
#include <chrono>
#endif

namespace collab {

#ifdef _WIN32

class Mutex {
public:
    Mutex() { InitializeCriticalSection(&cs_); }
    ~Mutex() { DeleteCriticalSection(&cs_); }
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    void lock() { EnterCriticalSection(&cs_); }
    void unlock() { LeaveCriticalSection(&cs_); }

private:
    CRITICAL_SECTION cs_;
};

class LockGuard {
public:
    explicit LockGuard(Mutex& mutex) : mutex_(mutex) { mutex_.lock(); }
    ~LockGuard() { mutex_.unlock(); }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    Mutex& mutex_;
};

inline void sleep_ms(unsigned ms) { Sleep(ms); }

#else

using Mutex = std::mutex;
using LockGuard = std::lock_guard<std::mutex>;

inline void sleep_ms(unsigned ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

#endif

} // namespace collab
