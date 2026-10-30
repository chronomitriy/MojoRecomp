#pragma once

#include <chrono>
#include <cstdint>

namespace mojorecomp::host {

using SteadyClock = std::chrono::steady_clock;

class PeriodicDeadline
{
public:
    using Duration = std::chrono::nanoseconds;
    using TimePoint = SteadyClock::time_point;

    void Reset(TimePoint start, Duration period) noexcept
    {
        if (period <= Duration::zero())
        {
            Clear();
            return;
        }
        period_ = period;
        deadline_ = start + period;
        active_ = true;
    }

    void Clear() noexcept
    {
        period_ = Duration::zero();
        deadline_ = TimePoint{};
        active_ = false;
    }

    bool Active() const noexcept { return active_; }
    Duration Period() const noexcept { return period_; }
    TimePoint Deadline() const noexcept { return deadline_; }

    void Advance(uint64_t periods = 1) noexcept
    {
        if (!active_ || periods == 0)
            return;
        deadline_ += period_ * static_cast<Duration::rep>(periods);
    }

    // Rebase an active schedule without changing its period. The next Advance()
    // resumes from this point instead of trying to repay a long host stall.
    void RebaseDeadline(TimePoint deadline) noexcept
    {
        if (active_)
            deadline_ = deadline;
    }

    // Move the deadline to the first period strictly after now and report how
    // many deadlines were skipped. This is arithmetic only; callers decide how
    // much catch-up work, if any, is semantically safe for their subsystem.
    uint64_t SkipPast(TimePoint now) noexcept
    {
        if (!active_ || now < deadline_)
            return 0;

        const auto behind = now - deadline_;
        const uint64_t skipped =
            static_cast<uint64_t>(behind / period_) + 1;
        Advance(skipped);
        return skipped;
    }

private:
    Duration period_ = Duration::zero();
    TimePoint deadline_{};
    bool active_ = false;
};

// Host-side timing primitive shared by runtime subsystems that need precise
// pacing. Guest simulation, display/vblank and audio keep separate clocks; this
// module only centralizes how host threads wait for their own deadlines.
void WaitFor(std::chrono::nanoseconds duration) noexcept;
void WaitUntil(SteadyClock::time_point deadline) noexcept;

// A short wait that ends at its timeout or when another thread calls Wake,
// whichever comes first. std::condition_variable::wait_for rounds a timeout up
// to whole milliseconds on Windows, and the wait then ends on the next process
// timer tick (15.6 ms unless the process raised the timer resolution); here
// the timeout is a high-resolution waitable timer instead.
class WakeableTimedWait
{
public:
    WakeableTimedWait() noexcept;
    ~WakeableTimedWait();

    WakeableTimedWait(const WakeableTimedWait&) = delete;
    WakeableTimedWait& operator=(const WakeableTimedWait&) = delete;

    // Any thread. A Wake with nobody waiting makes the next Wait return at once.
    void Wake() noexcept;
    // One waiting thread at a time. True when woken, false on timeout.
    bool Wait(std::chrono::nanoseconds timeout) noexcept;

private:
#ifdef _WIN32
    void* event_ = nullptr;
#else
    struct State;
    State* state_ = nullptr;
#endif
};

} // namespace mojorecomp::host
