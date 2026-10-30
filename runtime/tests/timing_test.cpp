#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#include "../host/timing.h"

namespace {

int Fail(const char* message)
{
    std::fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

} // namespace

int main()
{
    using namespace std::chrono_literals;
    using mojorecomp::host::PeriodicDeadline;

    PeriodicDeadline timer;
    const PeriodicDeadline::TimePoint zero{};

    if (timer.Active())
        return Fail("new periodic deadline should be inactive");

    timer.Reset(zero, 16ms);
    if (!timer.Active() || timer.Period() != 16ms || timer.Deadline() != zero + 16ms)
        return Fail("periodic deadline reset produced the wrong first deadline");

    timer.Advance();
    if (timer.Deadline() != zero + 32ms)
        return Fail("periodic deadline did not advance by one period");

    const uint64_t skipped = timer.SkipPast(zero + 81ms);
    if (skipped != 4 || timer.Deadline() != zero + 96ms)
        return Fail("periodic deadline catch-up arithmetic is incorrect");

    timer.RebaseDeadline(zero + 100ms);
    timer.Advance();
    if (timer.Deadline() != zero + 116ms)
        return Fail("periodic deadline rebase did not preserve its period");

    timer.Clear();
    if (timer.Active() || timer.SkipPast(zero + 1s) != 0)
        return Fail("cleared periodic deadline should stay inactive");

    std::puts("PASS: periodic deadline reset, advance, catch-up and rebase are deterministic.");

    // WakeableTimedWait: the PM4 pump's idle poll (docs/HFR-PHASE3.md).
    using mojorecomp::host::WakeableTimedWait;
    using Clock = std::chrono::steady_clock;
    auto elapsedMs = [](Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    };
    {
        WakeableTimedWait wait;
        wait.Wake();
        const auto start = Clock::now();
        if (!wait.Wait(1s) || elapsedMs(start) > 200.0)
            return Fail("a Wake before Wait did not end the next wait at once");
        // The wake was consumed: the next wait runs to its timeout.
        if (wait.Wait(250us))
            return Fail("one Wake ended two waits");
    }
    {
        WakeableTimedWait wait;
        std::thread waker([&] {
            std::this_thread::sleep_for(20ms);
            wait.Wake();
        });
        const auto start = Clock::now();
        const bool woken = wait.Wait(2s);
        const double ms = elapsedMs(start);
        waker.join();
        if (!woken || ms > 1000.0)
            return Fail("Wake from another thread did not end a long wait");
    }
    {
        // A 250 us timeout must not be stretched to a timer tick. The median
        // keeps a loaded machine from failing the check; the condition variable
        // it replaces is printed for comparison.
        WakeableTimedWait wait;
        std::vector<double> waits, cvWaits;
        std::mutex mutex;
        std::condition_variable cv;
        for (int i = 0; i < 21; ++i)
        {
            auto start = Clock::now();
            if (wait.Wait(250us))
                return Fail("an idle wait reported a wake nobody sent");
            waits.push_back(elapsedMs(start));
            start = Clock::now();
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait_for(lock, 250us, [] { return false; });
            cvWaits.push_back(elapsedMs(start));
        }
        std::sort(waits.begin(), waits.end());
        std::sort(cvWaits.begin(), cvWaits.end());
        const double median = waits[waits.size() / 2];
        std::printf("INFO: 250 us idle wait: median %.3f ms (condition_variable::wait_for %.3f ms)\n",
                    median, cvWaits[cvWaits.size() / 2]);
        if (median < 0.2 || median > 5.0)
            return Fail("a 250 us idle wait did not last about 250 us");
    }
    std::puts("PASS: wakeable timed wait ends on Wake or on a high-resolution timeout.");
    return 0;
}
