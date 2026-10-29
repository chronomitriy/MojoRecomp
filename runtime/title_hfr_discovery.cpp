// COT-specific discovery for the unlocked frame-rate modes. See the header.

#include "title_hfr_discovery.h"

#include "ppc_recomp_shared.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "cpu/ppc_constant_scan.h"
#include "kernel/guestcall.h"
#include "kernel/memory.h"

namespace {

using mojorecomp::cpu::DescribePpcInstruction;
using mojorecomp::cpu::PpcFloatConstantLoad;

constexpr uint32_t kCodeBegin = static_cast<uint32_t>(PPC_CODE_BASE);
constexpr uint32_t kCodeEnd = static_cast<uint32_t>(PPC_CODE_BASE + PPC_CODE_SIZE);
constexpr uint32_t kFunctionSearchBytes = 0x40000;
constexpr uint32_t kChainDepth = 8;
constexpr uint32_t kChainsPerSite = 3;
constexpr size_t kMaxListedSixtieths = 64;

struct Candidate
{
    PpcFloatConstantLoad load;
    bool sixtieth = false; // otherwise 60.0
};

std::once_flag g_scanOnce;
std::vector<Candidate> g_candidates;
std::mutex g_reportMutex;

struct SiteReport
{
    const char* site = nullptr;
    uint32_t firstReturns[kChainsPerSite]{};
    uint32_t chains = 0;
};
std::vector<SiteReport> g_siteReports;

bool ReadGuestWord(uint32_t address, uint32_t& word)
{
    // The whole 4 GiB guest range is mapped (see kernel/memory.cpp).
    if (!g_guestMemory.base || (address & 3u))
        return false;
    uint32_t raw = 0;
    std::memcpy(&raw, g_guestMemory.base + address, sizeof(raw));
    word = __builtin_bswap32(raw);
    return true;
}

bool IsSixtieth(double value)
{
    return std::fabs(value - 1.0 / 60.0) < 1.0e-6;
}

bool IsSixty(double value)
{
    return std::fabs(value - 60.0) < 1.0e-4;
}

uint32_t FunctionStart(uint32_t address)
{
    address &= ~3u;
    for (uint32_t back = 0; back <= kFunctionSearchBytes && address >= kCodeBegin + back;
         back += 4)
    {
        if (g_guestMemory.FindFunction(address - back))
            return address - back;
    }
    return 0;
}

uint32_t FunctionEnd(uint32_t start)
{
    for (uint32_t offset = 4; offset <= kFunctionSearchBytes; offset += 4)
    {
        const uint64_t next = uint64_t(start) + offset;
        if (next >= kCodeEnd)
            return kCodeEnd;
        if (g_guestMemory.FindFunction(static_cast<uint32_t>(next)))
            return static_cast<uint32_t>(next);
    }
    return std::min<uint64_t>(uint64_t(start) + kFunctionSearchBytes, kCodeEnd);
}

void LogCandidate(const char* prefix, const Candidate& c, uint32_t function)
{
    std::fprintf(stderr,
        "[hfr-discovery] %s load=%08X %s -> [%08X]=%.9g in function %08X; next use %s%s%s\n",
        prefix, c.load.loadAddress,
        DescribePpcInstruction(c.load.loadWord, c.load.loadAddress).c_str(),
        c.load.constantAddress, c.load.value, function,
        c.load.useAddress ? "" : "none",
        c.load.useAddress
            ? DescribePpcInstruction(c.load.useWord, c.load.useAddress).c_str()
            : "",
        c.load.useAddress ? "" : " (within 16 instructions)");
    if (c.load.useAddress)
        std::fprintf(stderr, "[hfr-discovery]   at %08X\n", c.load.useAddress);
}

void ScanImage()
{
    const auto read = [](uint32_t address, uint32_t& word) {
        return ReadGuestWord(address, word);
    };
    const auto matches = [](double value) { return IsSixtieth(value) || IsSixty(value); };
    const auto loads = mojorecomp::cpu::FindFloatConstantLoads(kCodeBegin, kCodeEnd, read, matches);
    size_t sixtieths = 0;
    for (const auto& load : loads)
    {
        Candidate c{load, IsSixtieth(load.value)};
        sixtieths += c.sixtieth ? 1u : 0u;
        g_candidates.push_back(c);
    }
    std::fprintf(stderr,
        "[hfr-discovery] scanned %u KiB of guest code: %zu loads of 1/60, %zu loads of 60.0\n",
        (kCodeEnd - kCodeBegin) / 1024u, sixtieths, g_candidates.size() - sixtieths);
    size_t listed = 0;
    for (const auto& c : g_candidates)
    {
        if (!c.sixtieth)
            continue;
        if (listed++ == kMaxListedSixtieths)
        {
            std::fprintf(stderr, "[hfr-discovery] (further 1/60 loads not listed)\n");
            break;
        }
        LogCandidate("image 1/60", c, FunctionStart(c.load.loadAddress));
    }
}

bool NewChain(const char* site, uint32_t firstReturn)
{
    for (auto& report : g_siteReports)
    {
        if (std::strcmp(report.site, site) != 0)
            continue;
        for (uint32_t i = 0; i < report.chains; ++i)
            if (report.firstReturns[i] == firstReturn)
                return false;
        if (report.chains == kChainsPerSite)
            return false;
        report.firstReturns[report.chains++] = firstReturn;
        return true;
    }
    SiteReport report{};
    report.site = site;
    report.firstReturns[0] = firstReturn;
    report.chains = 1;
    g_siteReports.push_back(report);
    return true;
}

} // namespace

bool MojoRecompHfrDiscoveryEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_HFR_DISCOVERY");
        return value && value[0] == '1';
    }();
    return enabled;
}

void MojoRecompHfrDiscoveryObserve(const char* site, uint32_t siteAddress)
{
    if (!MojoRecompHfrDiscoveryEnabled() || !g_ppcContext || !g_guestMemory.base)
        return;
    std::call_once(g_scanOnce, ScanImage);

    const auto read = [](uint32_t address, uint32_t& word) {
        return ReadGuestWord(address, word);
    };
    const std::vector<uint32_t> chain = mojorecomp::cpu::WalkGuestBackChain(
        g_ppcContext->r1.u32, kChainDepth, read, kCodeBegin, kCodeEnd);

    std::lock_guard<std::mutex> lock(g_reportMutex);
    if (!NewChain(site, chain.empty() ? 0u : chain.front()))
        return;

    std::vector<uint32_t> functions;
    functions.push_back(FunctionStart(siteAddress));
    for (uint32_t returnAddress : chain)
        functions.push_back(FunctionStart(returnAddress));

    std::fprintf(stderr, "[hfr-discovery] site=%s at %08X in function %08X; callers:",
                 site, siteAddress, functions.front());
    for (size_t i = 0; i < chain.size(); ++i)
        std::fprintf(stderr, " %08X(fn %08X)", chain[i], functions[i + 1]);
    std::fprintf(stderr, "\n");

    for (size_t depth = 0; depth < functions.size(); ++depth)
    {
        const uint32_t start = functions[depth];
        if (!start)
            continue;
        const uint32_t end = FunctionEnd(start);
        for (const auto& c : g_candidates)
        {
            if (c.load.loadAddress < start || c.load.loadAddress >= end)
                continue;
            char prefix[64];
            std::snprintf(prefix, sizeof(prefix), "site=%s depth=%zu %s", site, depth,
                          c.sixtieth ? "1/60" : "60.0");
            LogCandidate(prefix, c, start);
            if (!c.sixtieth)
                continue;
            std::fprintf(stderr,
                "[hfr-discovery] toml| [[midasm_hook]]\n"
                "[hfr-discovery] toml| address = 0x%08X\n"
                "[hfr-discovery] toml| name = \"MojoRecompHfrSchedulerFloor\"\n"
                "[hfr-discovery] toml| registers = [\"f%u\"]\n"
                "[hfr-discovery] toml| after_instruction = true\n",
                c.load.loadAddress, c.load.fpr);
        }
    }
}
