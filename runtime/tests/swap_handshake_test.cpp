// Model of D3D's swap handshake running through the real PM4 parser, the real
// asynchronous executor and real draw snapshots (docs/HFR-PHASE3.md section 5).
//
// A synthetic guest thread writes frames into a ring: draws that read a
// double-buffered vertex buffer, a fence, and the handshake D3D emits before
// every swap (guest 0x82273BD8, docs/HFR-PHASE2.md section 4.2):
//   1. SCRATCH_REG1 = 1, then WAIT_REG_MEM until its write-back reads 1;
//   2. SCRATCH_REG4/5 = callback and parameter, SCRATCH_REG0 = 4, INTERRUPT,
//      then WAIT_REG_MEM until the interrupt handler clears SCRATCH_REG0;
//   3. WAIT_REG_MEM until SCRATCH_REG1 reads 0;
// then XE_SWAP. The interrupt handler stands in for D3D's ISR running callback
// 0x82273A40 with the immediate interval: it reads the write-backs and flips at
// once by writing SCRATCH_REG1 = 0. A pump thread runs Pm4_Execute like
// gpu/vd.cpp, and the executor's worker stands in for the Vulkan thread with a
// fixed cost per draw. The guest reuses a vertex buffer only once the fence of
// the frame that last drew from it is visible, as D3D titles do.
//
// It runs the phase 2 ordering (every store and interrupt queued behind the
// worker), CP-time signals, and a run whose policy flips at random points of
// the stream, including inside the handshake (Bink video starting and
// stopping does this at swaps), and checks that:
//   - the handler always finds SCRATCH_REG0 = 4 and that frame's REG4/REG5;
//   - stores to one address apply in stream order, so a fence never goes back;
//   - every draw renders the bytes its frame wrote, although the guest rewrites
//     a buffer as soon as the fences allow;
//   - presents arrive in order, and with CP-time signals no more than
//     kFramesAhead are outstanding after a swap;
//   - CP-time signals remove the swap stall and overlap the three stages.
//
// --mutate=<name> breaks one rule (see kMutations); each makes the run fail.
// No executable, shader or other game data is used.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "../gpu/cp_time_signals.h"
#include "../gpu/gpu_executor.h"
#include "../gpu/guest_memory_snapshot.h"
#include "../gpu/pm4.h"
#include "../host/pipeline_metrics.h"
#include "../host/timing.h"

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
namespace gpu = mojorecomp::gpu;
namespace host = mojorecomp::host;

// Guest memory, cached physical view.
constexpr uint32_t kPhysicalView = 0xA0000000u;
constexpr uint32_t kBackingBytes = 0x00200000u;
constexpr uint32_t kRing = 0xA0010000u;
constexpr uint32_t kRingDwords = 2048;
constexpr uint32_t kScratchPhysical = 0x00020000u;
constexpr uint32_t kScratch = kPhysicalView | kScratchPhysical;
constexpr uint32_t kFencePhysical = 0x00020100u;
constexpr uint32_t kFence = kPhysicalView | kFencePhysical;
constexpr uint32_t kRptrSlot = 0xA0020200u;
constexpr uint32_t kVertexBuffers = 0xA0100000u;
constexpr uint32_t kVertexBytes = 1024;
constexpr uint32_t kEndian8in32 = 2;

// Registers: the scratch block PM4 writes back, and two registers the draw
// sink reads to learn which buffer and frame a draw belongs to.
constexpr uint32_t kRegScratchUmsk = 0x01DCu; // followed by SCRATCH_ADDR
constexpr uint32_t kRegScratch0 = 0x0578u;
constexpr uint32_t kRegVertexBuffer = 0x0100u; // followed by the frame
// Flipping runs: the policy as a register, so a flip lands at an exact stream
// position the PM4 thread reaches.
constexpr uint32_t kRegPolicy = 0x0102u;
constexpr uint32_t kCallback = 0x82273A40u;

constexpr const char* kMutations[] = {
    "no-drain-guard",   // a CP-time signal may overtake a queued one
    "hybrid-interrupt", // interrupts at CP time, stores still queued
    "live-reads",       // the worker reads guest memory instead of the snapshot
    "no-lag-bound",     // swaps never wait for an earlier present
};

enum class Policy
{
    Deferred,
    CpTime,
    Flipping,
};

struct Config
{
    const char* name = "";
    Policy policy = Policy::Deferred;
    uint32_t frames = 60;
    uint32_t drawsPerFrame = 48;
    uint32_t padDwords = 61;
    std::chrono::microseconds guestPerFrame{1000};
    std::chrono::microseconds pm4PerFrame{1500};
    std::chrono::microseconds workerPerFrame{2500};
    bool promptWake = true;
    std::chrono::microseconds pumpTimeout{250};
    size_t framesAhead = 1;
    // Flipping runs: a queued store takes this long on the worker, which
    // widens the window in which one is still pending when the policy opens.
    std::chrono::microseconds workerStoreCost{0};
};

struct Run
{
    Config config;
    std::string mutation;
    uint8_t* base = nullptr;
    std::unique_ptr<gpu::OrderedGpuCommandStream> stream;
    gpu::CpTimeSignalGate gate;
    gpu::PresentLagBound lag;
    std::atomic<bool> policy{false};
    std::atomic<uint32_t> wptr{0};
    std::atomic<bool> stopPump{false};
    std::atomic<bool> abort{false};
    std::atomic<bool> guestDone{false};
    host::WakeableTimedWait pumpWait;

    // PM4 thread.
    uint32_t nextIsrFrame = 0;
    uint64_t storeIndex = 0;
    size_t maxOutstanding = 0;
    uint64_t cpTimeSignals = 0;
    uint64_t queuedSignals = 0;
    // Worker thread.
    uint32_t nextPresentFrame = 0;
    std::vector<Clock::time_point> presentTimes;
    // Guest thread.
    Clock::duration guestRingWait{};

    std::atomic<uint32_t> isrCalls{0};
    std::atomic<uint32_t> isrViolations{0};
    std::atomic<uint32_t> drawViolations{0};
    std::atomic<uint32_t> orderViolations{0};
    std::atomic<uint32_t> fenceRegressions{0};
    std::atomic<uint32_t> presentViolations{0};

    // Stream index of every queued store, in queue order, and the last index
    // applied per address.
    std::mutex storeMutex;
    std::deque<uint64_t> queuedStoreIndices;
    std::unordered_map<uint32_t, uint64_t> lastApplied;
};

Run* g_run = nullptr;

uint32_t LoadBe(uint32_t address)
{
    return __builtin_bswap32(*reinterpret_cast<volatile uint32_t*>(g_run->base + address));
}

void StoreBe(uint32_t address, uint32_t value)
{
    *reinterpret_cast<volatile uint32_t*>(g_run->base + address) = __builtin_bswap32(value);
}

void SpinFor(Clock::duration duration)
{
    const auto end = Clock::now() + duration;
    while (Clock::now() < end)
    {
    }
}

bool Mutated(const char* name)
{
    return g_run->mutation == name;
}

bool PolicyNow()
{
    if (g_run->config.policy == Policy::Flipping)
        return Pm4_Registers()[kRegPolicy] != 0;
    return g_run->policy.load(std::memory_order_relaxed);
}

// The decision renderer_probe.cpp makes for every store and interrupt.
bool CpTimeNow()
{
    const bool policy = PolicyNow();
    if (Mutated("no-drain-guard"))
        return policy;
    return g_run->gate.Allow(policy, g_run->stream->CompletedSequence());
}

void ApplyStore(uint32_t address, uint32_t value, uint64_t index)
{
    auto& run = *g_run;
    std::lock_guard<std::mutex> lock(run.storeMutex);
    uint64_t& last = run.lastApplied[address];
    if (index <= last)
        run.orderViolations.fetch_add(1, std::memory_order_relaxed);
    last = index;
    StoreBe(address, value);
}

void WakePump()
{
    g_run->pumpWait.Wake();
}

void OnStore(uint8_t*, uint32_t address, uint32_t value)
{
    auto& run = *g_run;
    const uint64_t index = ++run.storeIndex;
    if (!Mutated("hybrid-interrupt") && CpTimeNow())
    {
        ApplyStore(address, value, index);
        ++run.cpTimeSignals;
        return;
    }
    {
        std::lock_guard<std::mutex> lock(run.storeMutex);
        run.queuedStoreIndices.push_back(index);
    }
    run.gate.NoteQueued(run.stream->Enqueue(gpu::GuestStoreCommand{run.base, address, value}));
    ++run.queuedSignals;
}

bool OnInterruptCommand()
{
    auto& run = *g_run;
    const bool cpTime = Mutated("hybrid-interrupt") ? PolicyNow() : CpTimeNow();
    if (cpTime)
    {
        ++run.cpTimeSignals;
        return false;
    }
    run.gate.NoteQueued(run.stream->Enqueue(gpu::InterruptCommand{}));
    ++run.queuedSignals;
    return true;
}

// D3D's ISR with callback 0x82273A40 at the immediate interval: the flip is
// due now, so the callback clears the flip-pending value itself.
void Isr()
{
    auto& run = *g_run;
    run.isrCalls.fetch_add(1, std::memory_order_relaxed);
    const uint32_t reg0 = LoadBe(kScratch + 0);
    const uint32_t callback = LoadBe(kScratch + 16);
    const uint32_t frame = LoadBe(kScratch + 20);
    if (reg0 != 4 || callback != kCallback || frame != run.nextIsrFrame)
        run.isrViolations.fetch_add(1, std::memory_order_relaxed);
    ++run.nextIsrFrame;
    StoreBe(kScratch + 4, 0);
    StoreBe(kScratch + 0, 0);
}

void OnDraw(uint8_t* base, const Pm4Draw& draw)
{
    auto& run = *g_run;
    const uint32_t* regs = Pm4_Registers();
    gpu::DrawCommand command{};
    command.guestBase = base;
    command.draw = draw;
    command.draw.indexVa = regs[kRegVertexBuffer];
    command.vsHash = regs[kRegVertexBuffer + 1];
    // As renderer_probe.cpp OnDraw: an immutable snapshot plus the GPU stores
    // still queued ahead of this draw.
    auto snapshot = std::make_unique<gpu::GuestMemorySnapshot>();
    const auto pending = run.stream->CapturePendingStores();
    if (!snapshot->Capture(base, command.draw.indexVa, kVertexBytes))
        run.drawViolations.fetch_add(1, std::memory_order_relaxed);
    run.stream->ApplyPendingStores(*snapshot, pending);
    command.guestSnapshot = std::move(snapshot);
    run.stream->Enqueue(std::move(command));
    SpinFor(run.config.pm4PerFrame / run.config.drawsPerFrame);
}

void OnSwap(uint8_t*, uint32_t frame, uint32_t width, uint32_t height)
{
    auto& run = *g_run;
    const auto sequence = run.stream->Enqueue(gpu::PresentCommand{frame, width, height});
    run.lag.Retire(run.stream->CompletedSequence());
    run.lag.Add(sequence);
    const bool cpTime = CpTimeNow();
    if (cpTime && !Mutated("no-lag-bound"))
    {
        while (const auto oldest = run.lag.MustWaitFor(run.config.framesAhead))
        {
            run.stream->WaitFor(oldest);
            run.lag.PopOldest();
        }
    }
    if (cpTime)
        run.maxOutstanding = std::max(run.maxOutstanding,
                                      run.lag.Retire(run.stream->CompletedSequence()));
}

gpu::GpuExecutorBackend MakeBackend()
{
    gpu::GpuExecutorBackend backend{};
    backend.draw = [](const gpu::DrawCommand& command, const uint32_t*) {
        auto& run = *g_run;
        // The cost comes first, so a live read sees anything the guest wrote
        // while this draw was queued.
        SpinFor(run.config.workerPerFrame / run.config.drawsPerFrame);
        const uint8_t* bytes = Mutated("live-reads")
            ? command.guestBase + command.draw.indexVa
            : gpu::GuestReadPtr(command.guestBase, command.draw.indexVa, kVertexBytes);
        const uint8_t expected = static_cast<uint8_t>(command.vsHash);
        for (uint32_t i = 0; i < kVertexBytes; ++i)
        {
            if (bytes[i] != expected)
            {
                run.drawViolations.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
        return true;
    };
    backend.present = [](const gpu::PresentCommand& command) {
        auto& run = *g_run;
        if (command.frontBuffer != run.nextPresentFrame)
            run.presentViolations.fetch_add(1, std::memory_order_relaxed);
        run.nextPresentFrame = command.frontBuffer + 1;
        run.presentTimes.push_back(Clock::now());
        return true;
    };
    backend.interrupt = [](const gpu::InterruptCommand&) {
        Pm4_RendererInterruptHandshake();
        return true;
    };
    backend.store = [](const gpu::GuestStoreCommand& command) {
        auto& run = *g_run;
        uint64_t index = 0;
        {
            std::lock_guard<std::mutex> lock(run.storeMutex);
            if (!run.queuedStoreIndices.empty())
            {
                index = run.queuedStoreIndices.front();
                run.queuedStoreIndices.pop_front();
            }
        }
        SpinFor(run.config.workerStoreCost);
        ApplyStore(command.guestAddress, command.value, index);
        if (run.config.promptWake)
            WakePump();
        return true;
    };
    backend.producerService = [] { Pm4_ServiceRendererInterrupts(); };
    return backend;
}

// gpu/vd.cpp GraphicsProgressPump without the diagnostics.
void PumpMain()
{
    auto& run = *g_run;
    while (!run.stopPump.load(std::memory_order_acquire))
    {
        const uint32_t before = Pm4_Cursor();
        const uint32_t wptr = run.wptr.load(std::memory_order_acquire);
        const uint32_t consumed = Pm4_Execute(run.base, wptr);
        StoreBe(kRptrSlot, consumed);
        const bool advanced = consumed != before;
        const bool backlogged = consumed % kRingDwords != wptr % kRingDwords;
        if (backlogged || !advanced)
            run.pumpWait.Wait(run.config.pumpTimeout);
    }
}

uint32_t Type0(uint32_t reg, uint32_t count)
{
    return ((count - 1u) << 16) | reg;
}

uint32_t Type3(uint32_t opcode, uint32_t bodyCount)
{
    return 0xC0000000u | ((bodyCount - 1u) << 16) | (opcode << 8);
}

void GuestMain()
{
    auto& run = *g_run;
    const Config& config = run.config;
    std::mt19937 random(0x5EED);
    uint32_t write = 0;
    uint32_t lastFence = 0;

    auto space = [&] {
        const uint32_t read = LoadBe(kRptrSlot) % kRingDwords;
        const uint32_t used = (write + kRingDwords - read) % kRingDwords;
        return kRingDwords - 1u - used;
    };
    auto kick = [&] { run.wptr.store(write, std::memory_order_release); };
    // Whole packets only: the PM4 thread never sees a partial one.
    auto packetSpan = [&](const uint32_t* dwords, uint32_t needed) {
        if (space() < needed)
        {
            kick();
            const auto start = Clock::now();
            auto lastWake = start - 1s;
            while (space() < needed && !run.abort.load(std::memory_order_relaxed))
            {
                // As MojoRecompGpuPollBackoff in HFR: wake a sleeping pump.
                const auto now = Clock::now();
                if (config.promptWake && now - lastWake >= 50us)
                {
                    lastWake = now;
                    WakePump();
                }
            }
            run.guestRingWait += Clock::now() - start;
        }
        for (uint32_t i = 0; i < needed; ++i)
        {
            StoreBe(kRing + write * 4u, dwords[i]);
            write = (write + 1u) % kRingDwords;
        }
    };
    auto packet = [&](std::initializer_list<uint32_t> dwords) {
        packetSpan(dwords.begin(), static_cast<uint32_t>(dwords.size()));
    };
    // Stand-in for the constants and state each real draw writes: a NOP.
    std::vector<uint32_t> padding(config.padDwords + 1u, 0u);
    padding[0] = Type3(0x10, config.padDwords);

    packet({Type0(kRegScratchUmsk, 2), 0xFFu, kScratchPhysical});
    kick();
    // Flipping runs: at about half of these points, switch the policy on or
    // off where the PM4 thread will meet it.
    auto maybeFlip = [&] {
        if (config.policy == Policy::Flipping && (random() & 1u))
            packet({Type0(kRegPolicy, 1), static_cast<uint32_t>(random() & 1u)});
    };
    for (uint32_t k = 0; k < config.frames && !run.abort.load(); ++k)
    {
        // Buffer k % 2 was last drawn by frame k - 2, whose fence is k - 1.
        const uint32_t vertexBuffer = kVertexBuffers + (k % 2u) * 0x1000u;
        if (k >= 2)
        {
            for (;;)
            {
                const uint32_t fence = LoadBe(kFence);
                if (fence < lastFence)
                    run.fenceRegressions.fetch_add(1, std::memory_order_relaxed);
                lastFence = fence;
                if (fence >= k - 1u || run.abort.load(std::memory_order_relaxed))
                    break;
            }
        }
        std::memset(run.base + vertexBuffer, static_cast<int>(k & 0xFFu), kVertexBytes);

        packet({Type0(kRegVertexBuffer, 2), vertexBuffer, k});
        maybeFlip();
        for (uint32_t d = 0; d < config.drawsPerFrame; ++d)
        {
            packetSpan(padding.data(), static_cast<uint32_t>(padding.size()));
            packet({Type3(0x36, 1), 4u | (2u << 6) | (3u << 16)}); // DRAW_INDX_2, auto
            SpinFor(config.guestPerFrame / config.drawsPerFrame);
            kick();
        }
        packet({Type3(0x3D, 2), kFencePhysical | kEndian8in32, k + 1u}); // MEM_WRITE fence
        maybeFlip();

        const uint32_t slot0 = kScratchPhysical | kEndian8in32;
        const uint32_t slot1 = (kScratchPhysical + 4u) | kEndian8in32;
        packet({Type0(kRegScratch0 + 1u, 1), 1u});
        maybeFlip();
        packet({Type3(0x3C, 4), 0x13u, slot1, 1u, 0xFFFFFFFFu});
        packet({Type0(kRegScratch0 + 4u, 2), kCallback, k});
        maybeFlip();
        packet({Type0(kRegScratch0, 1), 4u});
        maybeFlip();
        packet({Type3(0x54, 1), 1u});
        maybeFlip();
        packet({Type3(0x3C, 4), 0x13u, slot0, 0u, 0xFFFFFFFFu});
        packet({Type3(0x3C, 4), 0x13u, slot1, 0u, 0xFFFFFFFFu});
        maybeFlip();
        packet({Type3(0x64, 4), 0x53574150u, k, 1280u, 720u});
        kick();
    }
    run.guestDone.store(true, std::memory_order_release);
}

uint8_t* ReserveGuestMemory()
{
#if defined(_WIN32)
    // The three physical views alias one backing, so guest writes reach the
    // snapshot layer's write protection the way they do in the runtime.
    HANDLE process = GetCurrentProcess();
    auto* base = static_cast<uint8_t*>(VirtualAlloc2(
        process, nullptr, 1ull << 32, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
        PAGE_NOACCESS, nullptr, 0));
    if (!base)
        return nullptr;
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                        PAGE_READWRITE | SEC_COMMIT, 0, kBackingBytes, nullptr);
    if (!mapping)
        return nullptr;
    for (uintptr_t view : gpu::kGuestMemoryPhysicalViews)
    {
        if (!VirtualFree(base + view, kBackingBytes, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) ||
            !MapViewOfFile3(mapping, process, base + view, 0, kBackingBytes,
                            MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0))
            return nullptr;
    }
    CloseHandle(mapping);
    return base;
#else
    void* reserved = mmap(nullptr, 1ull << 32, PROT_NONE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (reserved == MAP_FAILED)
        return nullptr;
    auto* base = static_cast<uint8_t*>(reserved);
    if (mprotect(base + kPhysicalView, kBackingBytes, PROT_READ | PROT_WRITE) != 0)
        return nullptr;
    return base;
#endif
}

struct Result
{
    bool finished = false;
    double frameMs = 0.0;
    double guestRingWaitMs = 0.0;
    double stallMs = 0.0;
    double swapStallMs = 0.0;
    size_t maxOutstanding = 0;
    uint64_t cpTimeSignals = 0;
    uint64_t queuedSignals = 0;
    uint32_t isrCalls = 0;
    uint32_t violations = 0;
};

Result Execute(uint8_t* base, const Config& config, const std::string& mutation)
{
    Run run;
    run.config = config;
    run.mutation = mutation;
    run.base = base;
    run.policy.store(config.policy == Policy::CpTime, std::memory_order_relaxed);
    g_run = &run;

    std::memset(base + kPhysicalView, 0, kBackingBytes);
    Pm4_SetRingBuffer(kRing, kRingDwords * 4u);
    Pm4_SetReadPointerSlot(kRptrSlot);
    Pm4_SetReadPointerUpdateFrequency(16);
    Pm4_SetInterruptSink(Isr);
    Pm4_SetInterruptCommandSink(OnInterruptCommand);
    Pm4_SetInterruptWakeSink(WakePump);
    Pm4_SetStoreSink(OnStore);
    Pm4_SetDrawSink(OnDraw);
    Pm4_SetSwapSink(OnSwap);
    run.stream = std::make_unique<gpu::OrderedGpuCommandStream>(
        std::make_unique<gpu::AsynchronousGpuExecutor>(MakeBackend()));

    host::PipelineCounters before{};
    host::ReadPipelineCounters(before);
    std::thread pump(PumpMain);
    std::thread guest(GuestMain);

    // The guest is done once the last swap is written; the run once the
    // worker has presented it. A broken ordering can deadlock the handshake.
    const auto deadline = Clock::now() + 20s;
    Result result;
    while (Clock::now() < deadline)
    {
        if (run.guestDone.load(std::memory_order_acquire) &&
            Pm4_Cursor() == run.wptr.load(std::memory_order_acquire) &&
            run.stream->CompletedSequence() == run.stream->Stats().submitted)
        {
            result.finished = true;
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    run.abort.store(true, std::memory_order_relaxed);
    guest.join();
    run.stopPump.store(true, std::memory_order_release);
    run.pumpWait.Wake();
    pump.join();
    run.stream->Shutdown();

    host::PipelineCounters after{};
    host::ReadPipelineCounters(after);
    const double frames = config.frames;
    result.stallMs = double(after.pm4StallNs - before.pm4StallNs) / 1e6 / frames;
    const auto swap = static_cast<uint32_t>(host::Pm4StallReason::SwapHandshake);
    result.swapStallMs =
        double(after.pm4StallReasonNs[swap] - before.pm4StallReasonNs[swap]) / 1e6 / frames;
    result.guestRingWaitMs =
        std::chrono::duration<double, std::milli>(run.guestRingWait).count() / frames;
    // Steady state: skip the first quarter of the presents.
    const size_t first = run.presentTimes.size() / 4;
    if (run.presentTimes.size() > first + 1)
        result.frameMs = std::chrono::duration<double, std::milli>(
                             run.presentTimes.back() - run.presentTimes[first]).count() /
                         double(run.presentTimes.size() - 1 - first);
    result.maxOutstanding = run.maxOutstanding;
    result.cpTimeSignals = run.cpTimeSignals;
    result.queuedSignals = run.queuedSignals;
    result.isrCalls = run.isrCalls.load();
    result.violations = run.isrViolations + run.drawViolations + run.orderViolations +
                        run.fenceRegressions + run.presentViolations +
                        (run.nextPresentFrame != config.frames ? 1u : 0u) +
                        (run.isrCalls.load() != config.frames ? 1u : 0u);
    if (result.violations || !result.finished)
        std::printf("  %s%s%s: isr=%u/%u order=%u fence=%u draw=%u present=%u (%u/%u) %s\n",
                    config.name, mutation.empty() ? "" : " mutated ", mutation.c_str(),
                    run.isrViolations.load(), run.isrCalls.load(),
                    run.orderViolations.load(), run.fenceRegressions.load(),
                    run.drawViolations.load(), run.presentViolations.load(),
                    run.nextPresentFrame, config.frames,
                    result.finished ? "" : "(did not finish)");

    Pm4_SetSwapSink(nullptr);
    Pm4_SetDrawSink(nullptr);
    Pm4_SetStoreSink(nullptr);
    Pm4_SetInterruptWakeSink(nullptr);
    Pm4_SetInterruptCommandSink(nullptr);
    Pm4_SetInterruptSink(nullptr);
    g_run = nullptr;
    return result;
}

int Fail(const char* message)
{
    std::fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

void Report(const Config& config, const Result& result)
{
    std::printf("INFO: %-13s frame=%.2f ms guestRingWait=%.2f ms/f pm4 stall=%.2f ms/f "
                "(swap %.2f) signals cpTime=%llu queued=%llu maxOutstanding=%zu\n",
                config.name, result.frameMs, result.guestRingWaitMs, result.stallMs,
                result.swapStallMs, static_cast<unsigned long long>(result.cpTimeSignals),
                static_cast<unsigned long long>(result.queuedSignals), result.maxOutstanding);
}

} // namespace

int main(int argc, char** argv)
{
    std::string mutation;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg.rfind("--mutate=", 0) == 0)
            mutation = arg.substr(9);
    }
    if (!mutation.empty() &&
        std::none_of(std::begin(kMutations), std::end(kMutations),
                     [&](const char* name) { return mutation == name; }))
        return Fail("unknown --mutate name");

    // The ordering rules on their own.
    {
        gpu::CpTimeSignalGate gate;
        if (!gate.Allow(true, 0) || gate.Allow(false, 100))
            return Fail("the CP-time gate does not follow the policy");
        gate.NoteQueued(7);
        gate.NoteQueued(5); // sequences only grow
        if (gate.Allow(true, 6) || !gate.Allow(true, 7) || gate.LastQueued() != 7)
            return Fail("the CP-time gate opened before every queued signal completed");

        gpu::PresentLagBound lag;
        lag.Add(10);
        lag.Add(0); // a skipped present is not tracked
        lag.Add(20);
        if (lag.MustWaitFor(1) != 10 || lag.MustWaitFor(2) != 0 || lag.Outstanding() != 2)
            return Fail("the present bound waits for the wrong present");
        if (lag.Retire(10) != 1 || lag.MustWaitFor(1) != 0)
            return Fail("a completed present still counts as outstanding");
        lag.PopOldest();
        if (lag.Outstanding() != 0)
            return Fail("the present bound did not drop its oldest present");
    }

    uint8_t* base = ReserveGuestMemory();
    if (!base)
        return Fail("could not reserve guest memory");

    // Phase 2 as shipped: every signal queued, and a pump that sleeps a whole
    // millisecond (Windows rounds the 250 us condition-variable wait up) and
    // is never woken by a store landing.
    Config phase2{"phase2"};
    phase2.policy = Policy::Deferred;
    phase2.promptWake = false;
    phase2.pumpTimeout = 1000us;
    Config phase2Wake{"phase2+wake"};
    phase2Wake.policy = Policy::Deferred;
    Config cpTime{"cp-time"};
    cpTime.policy = Policy::CpTime;
    // A slow worker makes ordering mistakes and missing bounds visible.
    Config flipping{"flipping"};
    flipping.policy = Policy::Flipping;
    flipping.frames = 240;
    flipping.drawsPerFrame = 16;
    flipping.guestPerFrame = 100us;
    flipping.pm4PerFrame = 100us;
    flipping.workerPerFrame = 1500us;
    flipping.workerStoreCost = 300us;
    Config slowWorker{"slow-worker"};
    slowWorker.policy = Policy::CpTime;
    slowWorker.frames = 40;
    slowWorker.guestPerFrame = 100us;
    slowWorker.pm4PerFrame = 100us;
    slowWorker.workerPerFrame = 3000us;

    if (!mutation.empty())
    {
        // A mutation must break at least one check in the run that targets it.
        const Config& target = mutation == "no-drain-guard" ? flipping : slowWorker;
        const Result result = Execute(base, target, mutation);
        Report(target, result);
        const bool broken = !result.finished || result.violations ||
                            result.maxOutstanding > target.framesAhead;
        if (broken)
            return Fail("the mutated ordering broke the swap handshake model (expected)");
        std::puts("PASS: the mutation went unnoticed");
        return 0;
    }

    struct Case
    {
        const Config* config;
        Result result;
    };
    std::vector<Case> cases = {{&phase2, {}}, {&phase2Wake, {}}, {&cpTime, {}},
                               {&flipping, {}}, {&slowWorker, {}}};
    for (auto& c : cases)
    {
        c.result = Execute(base, *c.config, {});
        Report(*c.config, c.result);
        if (!c.result.finished)
            return Fail("a run did not finish");
        if (c.result.violations)
            return Fail("a run broke a handshake, ordering or snapshot invariant");
    }
    const Result& p2 = cases[0].result;
    const Result& p2w = cases[1].result;
    const Result& cp = cases[2].result;
    const Result& flip = cases[3].result;
    const Result& slow = cases[4].result;

    // Phase 2 ordering: the PM4 thread stalls in the swap handshake every
    // frame, for at least most of the worker's frame.
    if (p2w.swapStallMs < 0.5 * phase2Wake.workerPerFrame.count() / 1000.0 ||
        p2w.swapStallMs < 0.5 * p2w.stallMs || p2w.queuedSignals == 0 || p2w.cpTimeSignals != 0)
        return Fail("the phase 2 model no longer stalls in the swap handshake");
    // CP-time signals: no swap stall, nothing queued, and the stages overlap.
    if (cp.swapStallMs != 0.0 || cp.queuedSignals != 0 || cp.cpTimeSignals == 0)
        return Fail("CP-time signals still queued a signal or stalled in the handshake");
    if (!(cp.frameMs < 0.8 * p2w.frameMs) || !(p2w.frameMs <= p2.frameMs * 1.1))
        return Fail("CP-time signals did not overlap the guest, PM4 and worker stages");
    if (cp.maxOutstanding > cpTime.framesAhead || slow.maxOutstanding > slowWorker.framesAhead)
        return Fail("more presents outstanding than MOJORECOMP_PM4_FRAMES_AHEAD allows");
    // The flipping run really used both orderings.
    if (!flip.cpTimeSignals || !flip.queuedSignals)
        return Fail("the flipping run did not exercise both orderings");

    std::printf("PASS: swap handshake holds its ordering with queued and CP-time signals; "
                "CP-time frame %.2f ms vs %.2f ms queued\n", cp.frameMs, p2w.frameMs);
    return 0;
}
