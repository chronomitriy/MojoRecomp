#include "pipeline_metrics.h"

#include <atomic>
#include <cstdio>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace mojorecomp::host {
namespace {

std::atomic<uint64_t> g_guestCpWaitNs{0};
std::atomic<uint64_t> g_guestCpWaitEpisodes{0};
std::atomic<uint64_t> g_pm4StallNs{0};
std::atomic<uint64_t> g_pm4StallEpisodes{0};
std::atomic<uint64_t> g_pm4StallVblankReleased{0};
std::atomic<uint64_t> g_guestVblanks{0};
std::atomic<uint64_t> g_titleGateReleases{0};
std::atomic<uint64_t> g_titleGateIntervals[kTitleGateIntervalBuckets]{};
std::atomic<uint32_t> g_guestMainThreadId{0};

double PerFrame(uint64_t value, uint64_t frames)
{
    return frames ? double(value) / double(frames) : 0.0;
}

double Ms(double ns)
{
    return ns / 1.0e6;
}

uint64_t Delta(uint64_t current, uint64_t previous)
{
    return current >= previous ? current - previous : 0;
}

} // namespace

void NoteGuestCpWait(uint64_t ns) noexcept
{
    g_guestCpWaitNs.fetch_add(ns, std::memory_order_relaxed);
    g_guestCpWaitEpisodes.fetch_add(1, std::memory_order_relaxed);
}

void NotePm4Stall(uint64_t ns, bool releasedByVblank) noexcept
{
    g_pm4StallNs.fetch_add(ns, std::memory_order_relaxed);
    g_pm4StallEpisodes.fetch_add(1, std::memory_order_relaxed);
    if (releasedByVblank)
        g_pm4StallVblankReleased.fetch_add(1, std::memory_order_relaxed);
}

void NoteGuestVblank() noexcept
{
    g_guestVblanks.fetch_add(1, std::memory_order_release);
}

uint64_t GuestVblankSequence() noexcept
{
    return g_guestVblanks.load(std::memory_order_acquire);
}

void NoteTitleFrameGateRelease(uint32_t intervalUs) noexcept
{
    g_titleGateReleases.fetch_add(1, std::memory_order_relaxed);
    g_titleGateIntervals[TitleGateIntervalBucket(intervalUs)].fetch_add(
        1, std::memory_order_relaxed);
}

void SetGuestMainThread(uint32_t hostThreadId) noexcept
{
    g_guestMainThreadId.store(hostThreadId, std::memory_order_release);
}

uint64_t GuestMainThreadCpuNs() noexcept
{
#if defined(_WIN32)
    static HANDLE thread = nullptr;
    static uint32_t openedId = 0;
    const uint32_t id = g_guestMainThreadId.load(std::memory_order_acquire);
    if (!id)
        return 0;
    if (!thread || openedId != id)
    {
        if (thread)
            CloseHandle(thread);
        thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id);
        openedId = id;
        if (!thread)
            return 0;
    }
    FILETIME create{}, exit{}, kernel{}, user{};
    if (!GetThreadTimes(thread, &create, &exit, &kernel, &user))
        return 0;
    const auto ticks = [](const FILETIME& value) {
        return (uint64_t(value.dwHighDateTime) << 32) | value.dwLowDateTime;
    };
    return (ticks(kernel) + ticks(user)) * 100u;
#else
    return 0;
#endif
}

void ReadPipelineCounters(PipelineCounters& out) noexcept
{
    out.guestCpWaitNs = g_guestCpWaitNs.load(std::memory_order_relaxed);
    out.guestCpWaitEpisodes = g_guestCpWaitEpisodes.load(std::memory_order_relaxed);
    out.pm4StallNs = g_pm4StallNs.load(std::memory_order_relaxed);
    out.pm4StallEpisodes = g_pm4StallEpisodes.load(std::memory_order_relaxed);
    out.pm4StallVblankReleased = g_pm4StallVblankReleased.load(std::memory_order_relaxed);
    out.titleGateReleases = g_titleGateReleases.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < kTitleGateIntervalBuckets; ++i)
        out.titleGateIntervals[i] = g_titleGateIntervals[i].load(std::memory_order_relaxed);
}

std::string FormatPipelineSummary(const PipelineCounters& previous,
                                  const PipelineCounters& current)
{
    const uint64_t frames = Delta(current.frames, previous.frames);
    const uint64_t wallNs = Delta(current.wallNs, previous.wallNs);
    const double frameMs = Ms(PerFrame(wallNs, frames));
    const double fps = wallNs ? double(frames) * 1.0e9 / double(wallNs) : 0.0;

    const uint64_t guestCpu = Delta(current.guestMainCpuNs, previous.guestMainCpuNs);
    const uint64_t pm4Execute = Delta(current.pm4ExecuteNs, previous.pm4ExecuteNs);
    const uint64_t backpressure =
        Delta(current.executorBackpressureNs, previous.executorBackpressureNs);
    const uint64_t pm4Busy = pm4Execute > backpressure ? pm4Execute - backpressure : 0;
    const uint64_t worker = Delta(current.executorActiveNs, previous.executorActiveNs);

    // Name the stage whose busy time is the largest share of the wall time.
    const char* busiest = "pm4";
    uint64_t busiestNs = pm4Busy;
    if (worker > busiestNs)
    {
        busiest = "vulkan";
        busiestNs = worker;
    }
    if (current.guestMainCpuNs && guestCpu > busiestNs)
    {
        busiest = "guest";
        busiestNs = guestCpu;
    }
    const double busiestShare = wallNs ? 100.0 * double(busiestNs) / double(wallNs) : 0.0;

    const uint64_t hits = Delta(current.snapshotCacheHits, previous.snapshotCacheHits);
    const uint64_t misses = Delta(current.snapshotCacheMisses, previous.snapshotCacheMisses);
    const double hitRate = hits + misses ? 100.0 * double(hits) / double(hits + misses) : 0.0;

    char guestText[64];
    if (current.guestMainCpuNs)
        std::snprintf(guestText, sizeof(guestText), "%.2fms(%.0f%%)",
                      Ms(PerFrame(guestCpu, frames)),
                      wallNs ? 100.0 * double(guestCpu) / double(wallNs) : 0.0);
    else
        std::snprintf(guestText, sizeof(guestText), "n/a");

    const uint64_t releases = Delta(current.titleGateReleases, previous.titleGateReleases);
    std::string gate;
    for (uint32_t i = 0; i < kTitleGateIntervalBuckets; ++i)
    {
        const uint64_t n = Delta(current.titleGateIntervals[i], previous.titleGateIntervals[i]);
        char bucket[32];
        std::snprintf(bucket, sizeof(bucket), "%s%s=%.0f%%", i ? " " : "",
                      kTitleGateIntervalBucketNames[i],
                      releases ? 100.0 * double(n) / double(releases) : 0.0);
        gate += bucket;
    }

    char line[1024];
    std::snprintf(
        line, sizeof(line),
        "[pipeline] frames=%llu fps=%.2f frame=%.2fms busiest=%s(%.0f%%) | "
        "guest cpu=%s cpWait=%.2fms/%.1fx | "
        "pm4 busy=%.2fms sink=%.2fms backpressure=%.2fms stall=%.2fms/%.1fx vblankReleased=%.1fx | "
        "vulkan worker=%.2fms | "
        "snap hit=%.1f%% miss=%.1f volatile=%.1f cmd=%.1f copied=%.2fMiB faults=%.1f protects=%.1f | "
        "gate releases=%llu ticks: %s",
        static_cast<unsigned long long>(frames), fps, frameMs, busiest, busiestShare,
        guestText,
        Ms(PerFrame(Delta(current.guestCpWaitNs, previous.guestCpWaitNs), frames)),
        PerFrame(Delta(current.guestCpWaitEpisodes, previous.guestCpWaitEpisodes), frames),
        Ms(PerFrame(pm4Busy, frames)),
        Ms(PerFrame(Delta(current.pm4DrawSinkNs, previous.pm4DrawSinkNs), frames)),
        Ms(PerFrame(backpressure, frames)),
        Ms(PerFrame(Delta(current.pm4StallNs, previous.pm4StallNs), frames)),
        PerFrame(Delta(current.pm4StallEpisodes, previous.pm4StallEpisodes), frames),
        PerFrame(Delta(current.pm4StallVblankReleased, previous.pm4StallVblankReleased), frames),
        Ms(PerFrame(worker, frames)),
        hitRate, PerFrame(misses, frames),
        PerFrame(Delta(current.snapshotVolatileCaptures, previous.snapshotVolatileCaptures), frames),
        PerFrame(Delta(current.snapshotCommandCaptures, previous.snapshotCommandCaptures), frames),
        PerFrame(Delta(current.snapshotCopiedBytes, previous.snapshotCopiedBytes), frames) /
            (1024.0 * 1024.0),
        PerFrame(Delta(current.snapshotWriteFaults, previous.snapshotWriteFaults), frames),
        PerFrame(Delta(current.snapshotProtectCalls, previous.snapshotProtectCalls), frames),
        static_cast<unsigned long long>(releases), gate.c_str());
    return line;
}

} // namespace mojorecomp::host
