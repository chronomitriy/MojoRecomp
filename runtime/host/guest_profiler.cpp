#include "guest_profiler.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

#include "../config/runtime_config.h"
#include "../kernel/klog.h"
#include "host_paths.h"
#include "pipeline_metrics.h"
#include "timing.h"
#endif

namespace mojorecomp::host {

// ---------------------------------------------------------------------------
// Portable core.

uint32_t PrimaryFunctionBegin(const uint8_t* image, size_t imageSize,
                              const RuntimeFunctionEntry& entry)
{
    // UNWIND_INFO: version:3 flags:5, prolog size, code count, frame
    // register/offset, then the 2-byte unwind codes padded to an even count.
    // With UNW_FLAG_CHAININFO a RUNTIME_FUNCTION of the parent follows them.
    constexpr uint8_t kChainInfo = 0x4;
    RuntimeFunctionEntry current = entry;
    for (int depth = 0; depth < 32; ++depth)
    {
        const uint32_t unwind = current.unwindInfo & ~1u;
        // Bit 0 set marks an indirect entry (ARM64/CHPE); not used on x64.
        if ((current.unwindInfo & 1u) || unwind + 4 > imageSize)
            return current.begin;
        const uint8_t flags = image[unwind] >> 3;
        if (!(flags & kChainInfo))
            return current.begin;
        const uint32_t codes = image[unwind + 2];
        const size_t chained = unwind + 4 + size_t((codes + 1u) & ~1u) * 2u;
        if (chained + 12 > imageSize)
            return current.begin;
        RuntimeFunctionEntry parent{};
        std::memcpy(&parent.begin, image + chained, 4);
        std::memcpy(&parent.end, image + chained + 4, 4);
        std::memcpy(&parent.unwindInfo, image + chained + 8, 4);
        current = parent;
    }
    return current.begin;
}

uint64_t ResolveJumpThunk(uint64_t address, CodeReader read, void* context, int maxHops)
{
    for (int hop = 0; hop < maxHops; ++hop)
    {
        uint8_t code[9]{};
        if (!read(address, code, sizeof(code), context))
            return address;
        size_t at = 0;
        // endbr64
        if (code[0] == 0xF3 && code[1] == 0x0F && code[2] == 0x1E && code[3] == 0xFA)
            at = 4;
        if (code[at] == 0xE9)
        {
            int32_t displacement = 0;
            std::memcpy(&displacement, code + at + 1, 4);
            address = address + at + 5 + int64_t(displacement);
        }
        else if (code[at] == 0xEB)
            address = address + at + 2 + int64_t(int8_t(code[at + 1]));
        else
            return address;
    }
    return address;
}

void HostCodeMap::SetFunctions(std::vector<RuntimeFunctionEntry> functions,
                               std::vector<uint32_t> primaryBegins)
{
    functions_ = std::move(functions);
    primaryBegins_ = std::move(primaryBegins);
    if (primaryBegins_.size() != functions_.size())
    {
        primaryBegins_.resize(functions_.size());
        for (size_t i = 0; i < functions_.size(); ++i)
            primaryBegins_[i] = functions_[i].begin;
    }
}

uint32_t HostCodeMap::FunctionStart(uint32_t rva) const
{
    auto it = std::upper_bound(functions_.begin(), functions_.end(), rva,
                               [](uint32_t value, const RuntimeFunctionEntry& entry) {
                                   return value < entry.begin;
                               });
    if (it == functions_.begin())
        return kNoFunction;
    --it;
    if (rva >= it->end)
        return kNoFunction;
    return primaryBegins_[size_t(it - functions_.begin())];
}

void HostCodeMap::SetGuestFunctions(const std::vector<std::pair<uint32_t, uint32_t>>& hostToGuest)
{
    hostToGuest_.clear();
    guestStarts_.clear();
    hostToGuest_.reserve(hostToGuest.size());
    guestStarts_.reserve(hostToGuest.size());
    for (const auto& [host, guest] : hostToGuest)
    {
        // Identical-code folding can give two guest functions one body; keep
        // the lower guest address so the name is stable.
        auto [it, inserted] = hostToGuest_.emplace(host, guest);
        if (!inserted && guest < it->second)
            it->second = guest;
        guestStarts_.push_back(guest);
    }
    std::sort(guestStarts_.begin(), guestStarts_.end());
    guestStarts_.erase(std::unique(guestStarts_.begin(), guestStarts_.end()), guestStarts_.end());
}

uint32_t HostCodeMap::GuestAtHostStart(uint32_t rvaStart) const
{
    const auto it = hostToGuest_.find(rvaStart);
    return it == hostToGuest_.end() ? 0u : it->second;
}

uint32_t HostCodeMap::GuestFunctionContaining(uint32_t guestAddress) const
{
    auto it = std::upper_bound(guestStarts_.begin(), guestStarts_.end(), guestAddress);
    if (it == guestStarts_.begin())
        return 0;
    return *(it - 1);
}

const char* ToString(SampleKind kind)
{
    switch (kind)
    {
        case SampleKind::Guest: return "guest";
        case SampleKind::Spin: return "spin";
        case SampleKind::Wait: return "wait";
        case SampleKind::Import: return "import";
        case SampleKind::Native: return "native";
        case SampleKind::Host: return "host";
        case SampleKind::System: return "system";
        default: return "?";
    }
}

SampleKey ClassifySample(const RawSample& sample, const HostCodeMap& map,
                         uint64_t imageBase, uint64_t imageSize)
{
    if (sample.spin != uint8_t(GuestSpin::None))
        return {SampleKind::Spin, sample.spin};
    const bool inImage = sample.ip >= imageBase && sample.ip - imageBase < imageSize;
    uint32_t start = HostCodeMap::kNoFunction;
    if (inImage)
    {
        start = map.FunctionStart(uint32_t(sample.ip - imageBase));
        if (start != HostCodeMap::kNoFunction)
            if (const uint32_t guest = map.GuestAtHostStart(start))
                return {SampleKind::Guest, guest};
    }
    const auto named = [&](SampleKind kind) {
        return SampleKey{kind, uint64_t(reinterpret_cast<uintptr_t>(sample.name))};
    };
    switch (static_cast<GuestActivity>(sample.activity))
    {
        case GuestActivity::Wait: return named(SampleKind::Wait);
        case GuestActivity::Import: return named(SampleKind::Import);
        case GuestActivity::NativeCrt: return named(SampleKind::Native);
        default: break;
    }
    if (inImage)
        return {SampleKind::Host, start};
    return {SampleKind::System, 0};
}

void GuestProfileAggregate::Add(const SampleKey& key, uint32_t callerGuestFunction, uint64_t count)
{
    counts_[key] += count;
    if (callerGuestFunction)
        callers_[key][callerGuestFunction] += count;
    if (size_t(key.kind) < size_t(SampleKind::Count))
        kindTotals_[size_t(key.kind)] += count;
    total_ += count;
}

void GuestProfileAggregate::Merge(const GuestProfileAggregate& other)
{
    for (const auto& [key, count] : other.counts_)
        counts_[key] += count;
    for (const auto& [key, callers] : other.callers_)
        for (const auto& [caller, count] : callers)
            callers_[key][caller] += count;
    for (size_t i = 0; i < size_t(SampleKind::Count); ++i)
        kindTotals_[i] += other.kindTotals_[i];
    total_ += other.total_;
}

void GuestProfileAggregate::Clear()
{
    counts_.clear();
    callers_.clear();
    std::fill(std::begin(kindTotals_), std::end(kindTotals_), 0);
    total_ = 0;
}

std::vector<std::pair<SampleKey, uint64_t>> GuestProfileAggregate::Sorted() const
{
    std::vector<std::pair<SampleKey, uint64_t>> sorted(counts_.begin(), counts_.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second)
            return a.second > b.second;
        if (a.first.kind != b.first.kind)
            return a.first.kind < b.first.kind;
        return a.first.id < b.first.id;
    });
    return sorted;
}

std::vector<std::pair<uint32_t, uint64_t>> GuestProfileAggregate::SortedCallers(
    const SampleKey& key) const
{
    std::vector<std::pair<uint32_t, uint64_t>> sorted;
    const auto it = callers_.find(key);
    if (it == callers_.end())
        return sorted;
    sorted.assign(it->second.begin(), it->second.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    return sorted;
}

std::vector<std::string> FormatGuestProfileReport(
    const GuestProfileAggregate& aggregate, const char* scope, const char* thread,
    double seconds, const SampleNamer& namer, size_t topFunctions, size_t topBuckets,
    size_t topCallers)
{
    std::vector<std::string> lines;
    const uint64_t total = aggregate.Total();
    const double scale = total ? 100.0 / double(total) : 0.0;
    char line[512];
    int at = std::snprintf(line, sizeof(line),
                           "[guest-profile] summary scope=%s thread=%s seconds=%.1f samples=%" PRIu64
                           " rate=%.0f/s",
                           scope, thread, seconds, total, seconds > 0.0 ? double(total) / seconds : 0.0);
    for (size_t kind = 0; kind < size_t(SampleKind::Count) && at > 0 && size_t(at) < sizeof(line); ++kind)
        at += std::snprintf(line + at, sizeof(line) - size_t(at), " %s=%.2f",
                            ToString(SampleKind(kind)),
                            double(aggregate.KindTotal(SampleKind(kind))) * scale);
    lines.emplace_back(line);
    if (!total)
        return lines;

    const auto sorted = aggregate.Sorted();
    size_t functions = 0;
    size_t buckets = 0;
    for (const auto& [key, count] : sorted)
    {
        const bool guest = key.kind == SampleKind::Guest;
        size_t& rank = guest ? functions : buckets;
        if (rank >= (guest ? topFunctions : topBuckets))
            continue;
        ++rank;
        const std::string name = namer(key);
        std::snprintf(line, sizeof(line),
                      "[guest-profile] %s scope=%s thread=%s rank=%zu pct=%.2f samples=%" PRIu64
                      " name=%s",
                      guest ? "fn" : "bucket", scope, thread, rank, double(count) * scale, count,
                      name.c_str());
        lines.emplace_back(line);
        if (key.kind == SampleKind::Wait || key.kind == SampleKind::Import ||
            key.kind == SampleKind::Native)
        {
            const auto callers = aggregate.SortedCallers(key);
            for (size_t i = 0; i < callers.size() && i < topCallers; ++i)
            {
                std::snprintf(line, sizeof(line),
                              "[guest-profile] caller scope=%s thread=%s bucket=%s pct=%.2f samples=%" PRIu64
                              " name=sub_%08X",
                              scope, thread, name.c_str(), double(callers[i].second) * scale,
                              callers[i].second, callers[i].first);
                lines.emplace_back(line);
            }
        }
    }
    return lines;
}

// ---------------------------------------------------------------------------
// Windows sampler.

#if defined(_WIN32)
namespace {

struct Registration
{
    GuestProfileSlot slot;
    HANDLE thread = nullptr;
    uint32_t hostThreadId = 0;
    uint32_t guestThreadId = 0;

    ~Registration()
    {
        if (thread)
            CloseHandle(thread);
    }
};

struct ProfilerState
{
    std::mutex registryMutex;
    std::vector<std::shared_ptr<Registration>> threads;
    std::atomic<bool> running{false};
    bool allThreads = false;
    std::chrono::microseconds interval{1000};
    std::chrono::seconds reportPeriod{15};

    HostCodeMap map;
    uint64_t imageBase = 0;
    uint64_t imageSize = 0;

    std::mutex dataMutex;
    GuestProfileAggregate windowMain;
    GuestProfileAggregate windowOthers;
    GuestProfileAggregate totalMain;
    GuestProfileAggregate totalOthers;
    std::chrono::steady_clock::time_point started{};
    std::chrono::steady_clock::time_point windowStart{};
    uint64_t failedSamples = 0;
    std::atomic<bool> finalReported{false};

    std::mutex namesMutex;
    std::unordered_map<uint64_t, std::string> hostNames;
    bool symbolsTried = false;
    HMODULE dbghelp = nullptr;
};

ProfilerState& State()
{
    static ProfilerState* state = new ProfilerState();
    return *state;
}

bool ReadCode(uint64_t address, uint8_t* out, size_t size, void*)
{
    // Function entry points of this image: always mapped and readable.
    std::memcpy(out, reinterpret_cast<const void*>(static_cast<uintptr_t>(address)), size);
    return true;
}

bool BuildCodeMap(ProfilerState& state, const std::vector<std::pair<uint64_t, uint32_t>>& hostToGuest)
{
    const auto* image = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    if (!image)
        return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return false;
    state.imageBase = reinterpret_cast<uint64_t>(image);
    state.imageSize = nt->OptionalHeader.SizeOfImage;
    const IMAGE_DATA_DIRECTORY& exceptions =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    const size_t count = exceptions.Size / sizeof(RuntimeFunctionEntry);
    if (!exceptions.VirtualAddress || !count)
        return false;
    std::vector<RuntimeFunctionEntry> functions(count);
    std::memcpy(functions.data(), image + exceptions.VirtualAddress,
                count * sizeof(RuntimeFunctionEntry));
    // .pdata is sorted by the linker; keep the lookup correct if it is not.
    if (!std::is_sorted(functions.begin(), functions.end(),
                        [](const auto& a, const auto& b) { return a.begin < b.begin; }))
        std::sort(functions.begin(), functions.end(),
                  [](const auto& a, const auto& b) { return a.begin < b.begin; });
    std::vector<uint32_t> primary(count);
    for (size_t i = 0; i < count; ++i)
        primary[i] = PrimaryFunctionBegin(image, state.imageSize, functions[i]);
    state.map.SetFunctions(std::move(functions), std::move(primary));

    std::vector<std::pair<uint32_t, uint32_t>> guestBodies;
    guestBodies.reserve(hostToGuest.size());
    for (const auto& [host, guest] : hostToGuest)
    {
        if (!host || host < state.imageBase || host - state.imageBase >= state.imageSize)
            continue;
        const uint64_t body = ResolveJumpThunk(host, ReadCode, nullptr);
        if (body < state.imageBase || body - state.imageBase >= state.imageSize)
            continue;
        // The function containing the body's first byte is the guest function.
        const uint32_t start = state.map.FunctionStart(uint32_t(body - state.imageBase));
        guestBodies.emplace_back(start != HostCodeMap::kNoFunction
                                     ? start : uint32_t(body - state.imageBase),
                                 guest);
    }
    state.map.SetGuestFunctions(guestBodies);
    return true;
}

using SymInitializeWFn = BOOL(WINAPI*)(HANDLE, PCWSTR, BOOL);
using SymSetOptionsFn = DWORD(WINAPI*)(DWORD);
using SymLoadModuleExWFn = DWORD64(WINAPI*)(HANDLE, HANDLE, PCWSTR, PCWSTR, DWORD64, DWORD,
                                            void*, DWORD);
using SymFromAddrFn = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD64, void*);

// Mirrors SYMBOL_INFO without requiring dbghelp.h.
struct SymbolInfo
{
    ULONG SizeOfStruct;
    ULONG TypeIndex;
    ULONG64 Reserved[2];
    ULONG Index;
    ULONG Size;
    ULONG64 ModBase;
    ULONG Flags;
    ULONG64 Value;
    ULONG64 Address;
    ULONG Register;
    ULONG Scope;
    ULONG Tag;
    ULONG NameLen;
    ULONG MaxNameLen;
    CHAR Name[1];
};

std::string HostFunctionName(ProfilerState& state, uint32_t rva)
{
    std::lock_guard lock(state.namesMutex);
    if (const auto it = state.hostNames.find(rva); it != state.hostNames.end())
        return it->second;
    static SymFromAddrFn symFromAddr = nullptr;
    if (!state.symbolsTried)
    {
        state.symbolsTried = true;
        state.dbghelp = LoadLibraryW(L"dbghelp.dll");
        auto symSetOptions = state.dbghelp
            ? reinterpret_cast<SymSetOptionsFn>(GetProcAddress(state.dbghelp, "SymSetOptions")) : nullptr;
        auto symInitialize = state.dbghelp
            ? reinterpret_cast<SymInitializeWFn>(GetProcAddress(state.dbghelp, "SymInitializeW")) : nullptr;
        auto symLoadModule = state.dbghelp
            ? reinterpret_cast<SymLoadModuleExWFn>(GetProcAddress(state.dbghelp, "SymLoadModuleExW")) : nullptr;
        symFromAddr = state.dbghelp
            ? reinterpret_cast<SymFromAddrFn>(GetProcAddress(state.dbghelp, "SymFromAddr")) : nullptr;
        if (symSetOptions && symInitialize && symLoadModule && symFromAddr)
        {
            // SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS
            symSetOptions(0x00000002u | 0x00000004u | 0x00000200u);
            const std::wstring exeDir = HostPaths::ExeDir().wstring();
            wchar_t exePath[MAX_PATH]{};
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            if (!symInitialize(GetCurrentProcess(), exeDir.c_str(), FALSE) ||
                !symLoadModule(GetCurrentProcess(), nullptr, exePath, nullptr, state.imageBase,
                               DWORD(state.imageSize), nullptr, 0))
                symFromAddr = nullptr;
        }
        else
            symFromAddr = nullptr;
        if (!symFromAddr)
            KLOG("[guest-profile] DbgHelp symbols unavailable; host functions are shown by address\n");
    }
    char name[64];
    std::snprintf(name, sizeof(name), "host+0x%X", rva);
    std::string result = name;
    if (symFromAddr && rva != HostCodeMap::kNoFunction)
    {
        alignas(SymbolInfo) char buffer[sizeof(SymbolInfo) + 512]{};
        auto* symbol = reinterpret_cast<SymbolInfo*>(buffer);
        symbol->SizeOfStruct = sizeof(SymbolInfo);
        symbol->MaxNameLen = 511;
        DWORD64 displacement = 0;
        if (symFromAddr(GetCurrentProcess(), state.imageBase + rva, &displacement, symbol))
            result = std::string(symbol->Name, strnlen(symbol->Name, 511));
    }
    if (rva == HostCodeMap::kNoFunction)
        result = "host:unknown-leaf";
    state.hostNames.emplace(rva, result);
    return result;
}

std::string SampleName(ProfilerState& state, const SampleKey& key)
{
    char text[96];
    switch (key.kind)
    {
        case SampleKind::Guest:
            std::snprintf(text, sizeof(text), "sub_%08X", uint32_t(key.id));
            return text;
        case SampleKind::Spin:
            return key.id == uint64_t(GuestSpin::CpPoll) ? "spin:cp-poll" : "spin:frame-gate-idle";
        case SampleKind::Wait:
        case SampleKind::Import:
        case SampleKind::Native:
        {
            const char* name = reinterpret_cast<const char*>(static_cast<uintptr_t>(key.id));
            std::string_view view = name ? name : "unnamed";
            if (view.rfind("__imp__", 0) == 0)
                view.remove_prefix(7);
            return std::string(ToString(key.kind)) + ":" + std::string(view);
        }
        case SampleKind::Host:
            return "host:" + HostFunctionName(state, uint32_t(key.id));
        case SampleKind::System:
        {
            if (!key.id)
                return "system:unknown";
            wchar_t path[MAX_PATH]{};
            if (!GetModuleFileNameW(reinterpret_cast<HMODULE>(static_cast<uintptr_t>(key.id)),
                                    path, MAX_PATH))
                return "system:unknown";
            const wchar_t* base = std::wcsrchr(path, L'\\');
            base = base ? base + 1 : path;
            std::string ascii;
            for (const wchar_t* c = base; *c; ++c)
                ascii += *c < 128 ? char(*c) : '?';
            return "system:" + ascii;
        }
        default:
            return "?";
    }
}

// A host function whose symbol is a translated body (__imp__sub_XXXXXXXX) is
// guest code whose thunk could not be followed; report it as the guest function.
GuestProfileAggregate FoldTranslatedBodies(ProfilerState& state, const GuestProfileAggregate& in)
{
    GuestProfileAggregate out;
    for (const auto& [key, count] : in.Sorted())
    {
        SampleKey mapped = key;
        if (key.kind == SampleKind::Host)
        {
            const std::string name = HostFunctionName(state, uint32_t(key.id));
            unsigned int guest = 0;
            if ((std::sscanf(name.c_str(), "__imp__sub_%8X", &guest) == 1 ||
                 std::sscanf(name.c_str(), "sub_%8X", &guest) == 1) && guest)
                mapped = {SampleKind::Guest, guest};
        }
        uint64_t attributed = 0;
        for (const auto& [caller, callerCount] : in.SortedCallers(key))
        {
            out.Add(mapped, caller, callerCount);
            attributed += callerCount;
        }
        if (count > attributed)
            out.Add(mapped, 0, count - attributed);
    }
    return out;
}

void LogReport(ProfilerState& state, const GuestProfileAggregate& main,
               const GuestProfileAggregate& others, const char* scope, double seconds)
{
    const SampleNamer namer = [&state](const SampleKey& key) { return SampleName(state, key); };
    for (const auto& line : FormatGuestProfileReport(FoldTranslatedBodies(state, main), scope,
                                                     "main", seconds, namer))
        KLOG("%s\n", line.c_str());
    if (state.allThreads)
        for (const auto& line : FormatGuestProfileReport(FoldTranslatedBodies(state, others),
                                                         scope, "others", seconds, namer))
            KLOG("%s\n", line.c_str());
}

void SampleThread(ProfilerState& state, Registration& registration, bool isMain)
{
    if (SuspendThread(registration.thread) == DWORD(-1))
        return;
    CONTEXT context{};
    context.ContextFlags = CONTEXT_CONTROL;
    // GetThreadContext completes the suspension before reading the registers.
    const bool ok = GetThreadContext(registration.thread, &context) != FALSE;
    RawSample sample{};
    sample.ip = context.Rip;
    sample.spin = registration.slot.spin.load(std::memory_order_relaxed);
    sample.activity = registration.slot.activity.load(std::memory_order_relaxed);
    sample.name = registration.slot.name.load(std::memory_order_relaxed);
    sample.guestCaller = registration.slot.guestCaller.load(std::memory_order_relaxed);
    ResumeThread(registration.thread);
    // Nothing above may allocate or take a lock the suspended thread could hold.
    if (!ok)
    {
        std::lock_guard lock(state.dataMutex);
        ++state.failedSamples;
        return;
    }
    SampleKey key = ClassifySample(sample, state.map, state.imageBase, state.imageSize);
    if (key.kind == SampleKind::System)
    {
        HMODULE module = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(static_cast<uintptr_t>(sample.ip)),
                               &module))
            key.id = reinterpret_cast<uint64_t>(module);
    }
    uint32_t caller = 0;
    if ((key.kind == SampleKind::Wait || key.kind == SampleKind::Import ||
         key.kind == SampleKind::Native) && sample.guestCaller)
        caller = state.map.GuestFunctionContaining(sample.guestCaller);
    std::lock_guard lock(state.dataMutex);
    (isMain ? state.windowMain : state.windowOthers).Add(key, caller);
}

void SamplerLoop(ProfilerState& state)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    std::vector<std::shared_ptr<Registration>> threads;
    while (state.running.load(std::memory_order_acquire))
    {
        mojorecomp::host::WaitFor(state.interval);
        const uint32_t mainId = GuestMainThreadId();
        {
            std::lock_guard lock(state.registryMutex);
            threads = state.threads;
        }
        for (const auto& registration : threads)
        {
            const bool isMain = registration->hostThreadId == mainId;
            if (isMain || state.allThreads)
                SampleThread(state, *registration, isMain);
        }
        threads.clear();

        const auto now = std::chrono::steady_clock::now();
        if (now - state.windowStart >= state.reportPeriod)
        {
            GuestProfileAggregate main;
            GuestProfileAggregate others;
            {
                std::lock_guard lock(state.dataMutex);
                std::swap(main, state.windowMain);
                std::swap(others, state.windowOthers);
                state.totalMain.Merge(main);
                state.totalOthers.Merge(others);
            }
            const double seconds =
                std::chrono::duration<double>(now - state.windowStart).count();
            state.windowStart = now;
            LogReport(state, main, others, "window", seconds);
        }
    }
}

} // namespace

void GuestProfilerRegisterCurrentThread(uint32_t guestThreadId)
{
    // Unregistered threads keep t_guestProfileSlot null, so the activity tags
    // stay a single thread-local load when profiling is off.
    if (mojorecomp::config::Get().guestProfile == mojorecomp::config::GuestProfileMode::Off)
        return;
    auto& state = State();
    auto registration = std::make_shared<Registration>();
    registration->hostThreadId = GetCurrentThreadId();
    registration->guestThreadId = guestThreadId;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                         &registration->thread,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                         FALSE, 0))
        return;
    t_guestProfileSlot = &registration->slot;
    std::lock_guard lock(state.registryMutex);
    state.threads.push_back(std::move(registration));
}

void GuestProfilerUnregisterCurrentThread()
{
    auto& state = State();
    GuestProfileSlot* slot = t_guestProfileSlot;
    t_guestProfileSlot = nullptr;
    if (!slot)
        return;
    std::lock_guard lock(state.registryMutex);
    std::erase_if(state.threads, [slot](const auto& registration) {
        return &registration->slot == slot;
    });
}

void GuestProfilerStart(const std::vector<std::pair<uint64_t, uint32_t>>& hostToGuest)
{
    const auto mode = mojorecomp::config::Get().guestProfile;
    if (mode == mojorecomp::config::GuestProfileMode::Off)
        return;
    auto& state = State();
    if (state.running.exchange(true))
        return;
    state.allThreads = mode == mojorecomp::config::GuestProfileMode::AllThreads;
    if (const char* value = std::getenv("MOJORECOMP_GUEST_PROFILE_INTERVAL_US"); value && *value)
        state.interval = std::chrono::microseconds(std::clamp(std::atol(value), 100L, 100000L));
    if (const char* value = std::getenv("MOJORECOMP_GUEST_PROFILE_REPORT_S"); value && *value)
        state.reportPeriod = std::chrono::seconds(std::clamp(std::atol(value), 1L, 3600L));
    if (!BuildCodeMap(state, hostToGuest))
    {
        state.running.store(false);
        KLOG("[guest-profile] could not read this executable's function table; profiler off\n");
        return;
    }
    state.started = state.windowStart = std::chrono::steady_clock::now();
    KLOG("[guest-profile] ACTIVE: threads=%s interval=%lldus report=%llds functions=%zu "
         "guest=%zu of %zu mapped\n",
         state.allThreads ? "all" : "main", static_cast<long long>(state.interval.count()),
         static_cast<long long>(state.reportPeriod.count()), state.map.FunctionCount(),
         state.map.GuestFunctionCount(), hostToGuest.size());
    std::thread([&state] { SamplerLoop(state); }).detach();
}

void GuestProfilerReportFinal()
{
    auto& state = State();
    if (!state.running.load(std::memory_order_acquire) || state.finalReported.exchange(true))
        return;
    GuestProfileAggregate main;
    GuestProfileAggregate others;
    uint64_t failed = 0;
    {
        std::lock_guard lock(state.dataMutex);
        main = state.totalMain;
        main.Merge(state.windowMain);
        others = state.totalOthers;
        others.Merge(state.windowOthers);
        failed = state.failedSamples;
    }
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - state.started).count();
    LogReport(state, main, others, "total", seconds);
    KLOG("[guest-profile] end failedSamples=%llu\n", static_cast<unsigned long long>(failed));
    std::fflush(stderr);
}

#else

void GuestProfilerRegisterCurrentThread(uint32_t) {}
void GuestProfilerUnregisterCurrentThread() {}
void GuestProfilerStart(const std::vector<std::pair<uint64_t, uint32_t>>&) {}
void GuestProfilerReportFinal() {}

#endif

} // namespace mojorecomp::host
