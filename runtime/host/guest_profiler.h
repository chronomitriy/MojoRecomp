#pragma once

// Sampling profiler for guest CPU time (MOJORECOMP_GUEST_PROFILE=1 or all).
//
// A sampler thread suspends the main guest thread (or every registered guest
// thread) about once per millisecond, reads its instruction pointer and maps
// it to the translated guest function (sub_XXXXXXXX) that contains it. Host
// time is counted in separate buckets: kernel imports and waits (tagged by the
// import dispatcher), spinning in the CP-poll backoff and idle passes of the
// title's frame gate (tagged by their hooks), other runtime functions (named
// through DbgHelp and cot-runtime.pdb) and system modules.
//
// The tags below cost one thread-local load and a branch when profiling is off.

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mojorecomp::host {

enum class GuestActivity : uint8_t
{
    None,
    Import,
    Wait,
    NativeCrt,
};

enum class GuestSpin : uint8_t
{
    None,
    // The guest is polling the CP read pointer (MojoRecompGpuPollBackoff).
    CpPoll,
    // Game::Tick declined the update this pass (accumulated time below the
    // floor): the title loop is spinning until the next update.
    FrameGateIdle,
};

// Written by the owning thread, read by the sampler while that thread is
// suspended.
struct GuestProfileSlot
{
    std::atomic<uint8_t> activity{0};
    std::atomic<uint8_t> spin{0};
    std::atomic<const char*> name{nullptr};
    std::atomic<uint32_t> guestCaller{0};
};

// Set for registered guest threads while the profiler runs.
inline thread_local GuestProfileSlot* t_guestProfileSlot = nullptr;

class GuestActivityScope
{
public:
    GuestActivityScope(GuestActivity activity, const char* name, uint32_t guestCaller) noexcept
        : slot_(t_guestProfileSlot)
    {
        if (!slot_)
            return;
        previousActivity_ = slot_->activity.load(std::memory_order_relaxed);
        previousName_ = slot_->name.load(std::memory_order_relaxed);
        previousCaller_ = slot_->guestCaller.load(std::memory_order_relaxed);
        slot_->name.store(name, std::memory_order_relaxed);
        slot_->guestCaller.store(guestCaller, std::memory_order_relaxed);
        slot_->activity.store(static_cast<uint8_t>(activity), std::memory_order_relaxed);
    }
    ~GuestActivityScope()
    {
        if (!slot_)
            return;
        slot_->activity.store(previousActivity_, std::memory_order_relaxed);
        slot_->name.store(previousName_, std::memory_order_relaxed);
        slot_->guestCaller.store(previousCaller_, std::memory_order_relaxed);
    }
    GuestActivityScope(const GuestActivityScope&) = delete;
    GuestActivityScope& operator=(const GuestActivityScope&) = delete;

private:
    GuestProfileSlot* slot_;
    uint8_t previousActivity_ = 0;
    const char* previousName_ = nullptr;
    uint32_t previousCaller_ = 0;
};

inline void SetGuestSpin(GuestSpin spin) noexcept
{
    if (GuestProfileSlot* slot = t_guestProfileSlot)
        slot->spin.store(static_cast<uint8_t>(spin), std::memory_order_relaxed);
}

// Imports whose host implementation blocks count as waits.
constexpr GuestActivity ImportActivityFor(std::string_view guestName)
{
    constexpr std::string_view kBlocking[] = {"Wait", "DelayExecution", "Sleep", "YieldExecution"};
    for (std::string_view blocking : kBlocking)
        if (guestName.find(blocking) != std::string_view::npos)
            return GuestActivity::Wait;
    return GuestActivity::Import;
}

// Called by every thread that runs translated code (GuestThreadContext).
void GuestProfilerRegisterCurrentThread(uint32_t guestThreadId);
void GuestProfilerUnregisterCurrentThread();
// Starts the sampler when the configuration asks for it. `hostToGuest` lists
// each translated function's host entry point (the address PPCFuncMappings
// holds) and its guest address. Threads registered before or after the start
// are both sampled.
void GuestProfilerStart(const std::vector<std::pair<uint64_t, uint32_t>>& hostToGuest);
// Logs the whole-session report once; called before the process exits.
void GuestProfilerReportFinal();

// ---------------------------------------------------------------------------
// Portable core, also used by the unit test.

struct RuntimeFunctionEntry
{
    // Image-relative, as in IMAGE_RUNTIME_FUNCTION_ENTRY (.pdata).
    uint32_t begin = 0;
    uint32_t end = 0;
    uint32_t unwindInfo = 0;
};

// The primary function an x64 .pdata entry belongs to: follows
// UNW_FLAG_CHAININFO in the entry's UNWIND_INFO through the mapped image.
uint32_t PrimaryFunctionBegin(const uint8_t* image, size_t imageSize,
                              const RuntimeFunctionEntry& entry);

using CodeReader = bool (*)(uint64_t address, uint8_t* out, size_t size, void* context);

// Follows `jmp rel32` / `jmp rel8`, each optionally after endbr64, from
// `address`. XenonRecomp's sub_XXXXXXXX symbols are weak thunks that tail-jump
// to __imp__sub_XXXXXXXX; this finds the body the instruction pointer is in.
uint64_t ResolveJumpThunk(uint64_t address, CodeReader read, void* context, int maxHops = 4);

class HostCodeMap
{
public:
    static constexpr uint32_t kNoFunction = 0xFFFFFFFFu;

    // `functions` sorted by begin; `primaryBegins[i]` is PrimaryFunctionBegin
    // of functions[i].
    void SetFunctions(std::vector<RuntimeFunctionEntry> functions,
                      std::vector<uint32_t> primaryBegins);
    // Image-relative start of the primary function containing `rva`.
    uint32_t FunctionStart(uint32_t rva) const;
    // Host body start (image-relative) -> guest function address.
    void SetGuestFunctions(const std::vector<std::pair<uint32_t, uint32_t>>& hostToGuest);
    uint32_t GuestAtHostStart(uint32_t rvaStart) const;
    // The guest function containing a guest code address (for import callers).
    uint32_t GuestFunctionContaining(uint32_t guestAddress) const;
    size_t FunctionCount() const { return functions_.size(); }
    size_t GuestFunctionCount() const { return hostToGuest_.size(); }

private:
    std::vector<RuntimeFunctionEntry> functions_;
    std::vector<uint32_t> primaryBegins_;
    std::unordered_map<uint32_t, uint32_t> hostToGuest_;
    std::vector<uint32_t> guestStarts_;
};

enum class SampleKind : uint8_t
{
    Guest,
    Spin,
    Wait,
    Import,
    Native,
    Host,
    System,
    Count,
};

const char* ToString(SampleKind kind);

struct SampleKey
{
    SampleKind kind = SampleKind::System;
    // Guest: guest address. Spin: GuestSpin. Wait/Import/Native: name pointer.
    // Host: image-relative function start. System: module base (0 if unknown).
    uint64_t id = 0;
    bool operator==(const SampleKey& other) const { return kind == other.kind && id == other.id; }
};

struct SampleKeyHash
{
    size_t operator()(const SampleKey& key) const
    {
        return std::hash<uint64_t>{}(key.id * 0x9E3779B97F4A7C15ull + uint64_t(key.kind));
    }
};

struct RawSample
{
    uint64_t ip = 0;
    uint8_t spin = 0;
    uint8_t activity = 0;
    const char* name = nullptr;
    uint32_t guestCaller = 0;
};

// Precedence: spin tags, then translated guest code, then import/wait/native
// tags, then other code in the image, then other modules.
SampleKey ClassifySample(const RawSample& sample, const HostCodeMap& map,
                         uint64_t imageBase, uint64_t imageSize);

class GuestProfileAggregate
{
public:
    void Add(const SampleKey& key, uint32_t callerGuestFunction = 0, uint64_t count = 1);
    void Merge(const GuestProfileAggregate& other);
    void Clear();
    uint64_t Total() const { return total_; }
    uint64_t KindTotal(SampleKind kind) const { return kindTotals_[size_t(kind)]; }
    // Entries by descending count, then by key for a stable order.
    std::vector<std::pair<SampleKey, uint64_t>> Sorted() const;
    std::vector<std::pair<uint32_t, uint64_t>> SortedCallers(const SampleKey& key) const;

private:
    std::unordered_map<SampleKey, uint64_t, SampleKeyHash> counts_;
    std::unordered_map<SampleKey, std::unordered_map<uint32_t, uint64_t>, SampleKeyHash> callers_;
    uint64_t kindTotals_[size_t(SampleKind::Count)]{};
    uint64_t total_ = 0;
};

using SampleNamer = std::function<std::string(const SampleKey&)>;

// [guest-profile] lines: a summary with the share of each kind, the top
// `topFunctions` guest functions by self samples, the top `topBuckets` host
// buckets, and for each wait/import bucket its top guest callers.
std::vector<std::string> FormatGuestProfileReport(
    const GuestProfileAggregate& aggregate, const char* scope, const char* thread,
    double seconds, const SampleNamer& namer, size_t topFunctions = 30,
    size_t topBuckets = 15, size_t topCallers = 3);

} // namespace mojorecomp::host
