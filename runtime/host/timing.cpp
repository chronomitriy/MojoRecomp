#include "timing.h"

#include <cstdint>
#include <thread>

#ifndef _WIN32
#include <condition_variable>
#include <mutex>
#endif

#include <algorithm>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace mojorecomp::host {
namespace {

#ifdef _WIN32
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

class ThreadWaitableTimer
{
public:
    ThreadWaitableTimer() noexcept
    {
        handle_ = CreateWaitableTimerExW(nullptr, nullptr,
                                         CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (!handle_)
            handle_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }

    ~ThreadWaitableTimer()
    {
        if (handle_)
            CloseHandle(handle_);
    }

    ThreadWaitableTimer(const ThreadWaitableTimer&) = delete;
    ThreadWaitableTimer& operator=(const ThreadWaitableTimer&) = delete;

    bool WaitFor(std::chrono::nanoseconds duration) noexcept
    {
        if (!handle_ || duration <= std::chrono::nanoseconds::zero())
            return false;

        const int64_t nanoseconds = duration.count();
        const int64_t hundredNanoseconds = (nanoseconds + 99) / 100;
        if (hundredNanoseconds <= 0)
            return false;

        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(hundredNanoseconds);
        if (!SetWaitableTimer(handle_, &due, 0, nullptr, nullptr, FALSE))
            return false;

        return WaitForSingleObject(handle_, INFINITE) == WAIT_OBJECT_0;
    }

private:
    HANDLE handle_ = nullptr;
};

thread_local ThreadWaitableTimer g_waitableTimer;

// Relative due time in 100 ns units, at least one unit.
LARGE_INTEGER RelativeDueTime(std::chrono::nanoseconds duration) noexcept
{
    const int64_t hundredNanoseconds = std::max<int64_t>(1, (duration.count() + 99) / 100);
    LARGE_INTEGER due{};
    due.QuadPart = -static_cast<LONGLONG>(hundredNanoseconds);
    return due;
}

// The waiting thread's timer for WakeableTimedWait. A separate handle from
// g_waitableTimer, so a thread may use both.
class WakeableWaitTimer
{
public:
    WakeableWaitTimer() noexcept
    {
        handle_ = CreateWaitableTimerExW(nullptr, nullptr,
                                         CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (!handle_)
            handle_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }
    ~WakeableWaitTimer()
    {
        if (handle_)
            CloseHandle(handle_);
    }
    WakeableWaitTimer(const WakeableWaitTimer&) = delete;
    WakeableWaitTimer& operator=(const WakeableWaitTimer&) = delete;

    HANDLE Handle() const noexcept { return handle_; }

private:
    HANDLE handle_ = nullptr;
};

thread_local WakeableWaitTimer g_wakeableWaitTimer;
#endif

} // namespace

#ifdef _WIN32
WakeableTimedWait::WakeableTimedWait() noexcept
    : event_(CreateEventW(nullptr, FALSE, FALSE, nullptr))
{
}

WakeableTimedWait::~WakeableTimedWait()
{
    if (event_)
        CloseHandle(static_cast<HANDLE>(event_));
}

void WakeableTimedWait::Wake() noexcept
{
    if (event_)
        SetEvent(static_cast<HANDLE>(event_));
}

bool WakeableTimedWait::Wait(std::chrono::nanoseconds timeout) noexcept
{
    HANDLE event = static_cast<HANDLE>(event_);
    if (!event)
    {
        WaitFor(timeout);
        return false;
    }
    HANDLE timer = g_wakeableWaitTimer.Handle();
    LARGE_INTEGER due = RelativeDueTime(timeout);
    if (!timer || !SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
    {
        const auto ms = std::chrono::ceil<std::chrono::milliseconds>(timeout).count();
        return WaitForSingleObject(event, static_cast<DWORD>(std::max<int64_t>(1, ms))) ==
               WAIT_OBJECT_0;
    }
    const HANDLE handles[2] = {event, timer};
    const DWORD result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    if (result == WAIT_OBJECT_0)
    {
        CancelWaitableTimer(timer);
        return true;
    }
    return false;
}
#else
struct WakeableTimedWait::State
{
    std::mutex mutex;
    std::condition_variable cv;
    bool signalled = false;
};

WakeableTimedWait::WakeableTimedWait() noexcept
    : state_(new State)
{
}

WakeableTimedWait::~WakeableTimedWait()
{
    delete state_;
}

void WakeableTimedWait::Wake() noexcept
{
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->signalled = true;
    }
    state_->cv.notify_one();
}

bool WakeableTimedWait::Wait(std::chrono::nanoseconds timeout) noexcept
{
    std::unique_lock<std::mutex> lock(state_->mutex);
    const bool woken = state_->cv.wait_for(lock, timeout, [&] { return state_->signalled; });
    state_->signalled = false;
    return woken;
}
#endif

void WaitUntil(SteadyClock::time_point deadline) noexcept
{
    const auto now = SteadyClock::now();
    if (deadline <= now)
        return;

    const auto remaining =
        std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now);

#ifdef _WIN32
    if (g_waitableTimer.WaitFor(remaining))
        return;
#endif

    // Keep a small tail for scheduler jitter on platforms where the high
    // resolution timer is unavailable. This is a fallback path, not a spin loop
    // used during normal Windows gameplay.
    constexpr auto kYieldTail = std::chrono::milliseconds(1);
    if (remaining > kYieldTail)
        std::this_thread::sleep_for(remaining - kYieldTail);
    while (SteadyClock::now() < deadline)
        std::this_thread::yield();
}

void WaitFor(std::chrono::nanoseconds duration) noexcept
{
    if (duration <= std::chrono::nanoseconds::zero())
        return;
    WaitUntil(SteadyClock::now() + duration);
}

} // namespace mojorecomp::host
