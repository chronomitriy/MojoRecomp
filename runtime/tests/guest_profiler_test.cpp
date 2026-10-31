#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "../host/guest_profiler.h"

namespace {

using mojorecomp::host::ClassifySample;
using mojorecomp::host::GuestActivity;
using mojorecomp::host::GuestProfileAggregate;
using mojorecomp::host::GuestSpin;
using mojorecomp::host::HostCodeMap;
using mojorecomp::host::RawSample;
using mojorecomp::host::RuntimeFunctionEntry;
using mojorecomp::host::SampleKey;
using mojorecomp::host::SampleKind;

int Fail(const char* message)
{
    std::fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

void Put32(std::vector<uint8_t>& image, size_t at, uint32_t value)
{
    std::memcpy(image.data() + at, &value, sizeof(value));
}

void PutEntry(std::vector<uint8_t>& image, size_t at, const RuntimeFunctionEntry& entry)
{
    Put32(image, at, entry.begin);
    Put32(image, at + 4, entry.end);
    Put32(image, at + 8, entry.unwindInfo);
}

struct CodeBuffer
{
    uint64_t base = 0;
    std::vector<uint8_t> bytes;
};

bool ReadCode(uint64_t address, uint8_t* out, size_t size, void* context)
{
    const auto& code = *static_cast<const CodeBuffer*>(context);
    if (address < code.base || address - code.base + size > code.bytes.size())
        return false;
    std::memcpy(out, code.bytes.data() + (address - code.base), size);
    return true;
}

void PutCode(CodeBuffer& code, uint64_t address, std::initializer_list<uint8_t> bytes)
{
    std::memcpy(code.bytes.data() + (address - code.base), bytes.begin(), bytes.size());
}

bool Is(const SampleKey& key, SampleKind kind, uint64_t id)
{
    return key.kind == kind && key.id == id;
}

constexpr char kWaitName[] = "__imp__KeWaitForSingleObject";

// Same spellings as the runtime's namer, without DbgHelp.
std::string Name(const SampleKey& key)
{
    char text[64];
    switch (key.kind)
    {
        case SampleKind::Guest:
            std::snprintf(text, sizeof(text), "sub_%08X", uint32_t(key.id));
            return text;
        case SampleKind::Wait:
            return std::string("wait:") +
                   (reinterpret_cast<const char*>(static_cast<uintptr_t>(key.id)) + 7);
        case SampleKind::Host:
            return "host:test";
        default:
            return "?";
    }
}

} // namespace

// Blocking imports are counted as waits at compile time in GUEST_FUNCTION_HOOK.
static_assert(mojorecomp::host::ImportActivityFor("__imp__KeWaitForSingleObject") ==
              GuestActivity::Wait);
static_assert(mojorecomp::host::ImportActivityFor("__imp__KeDelayExecutionThread") ==
              GuestActivity::Wait);
static_assert(mojorecomp::host::ImportActivityFor("__imp__NtCreateFile") ==
              GuestActivity::Import);

int main()
{
    // .pdata chains: B and C are chained parts of A (C through B). Bit 0 of the
    // unwind RVA and an unwind record outside the image both stop the walk.
    std::vector<uint8_t> image(0x400, 0);
    const RuntimeFunctionEntry a{0x1000, 0x1100, 0x100};
    const RuntimeFunctionEntry b{0x1100, 0x1180, 0x200};
    const RuntimeFunctionEntry c{0x1180, 0x1200, 0x300};
    image[0x100] = 0x01;          // version 1, no flags
    image[0x200] = 0x01 | (4 << 3); // UNW_FLAG_CHAININFO
    image[0x202] = 1;             // one unwind code, padded to two
    PutEntry(image, 0x208, a);
    image[0x300] = 0x01 | (4 << 3);
    image[0x302] = 0;
    PutEntry(image, 0x304, b);
    using mojorecomp::host::PrimaryFunctionBegin;
    if (PrimaryFunctionBegin(image.data(), image.size(), a) != 0x1000 ||
        PrimaryFunctionBegin(image.data(), image.size(), b) != 0x1000 ||
        PrimaryFunctionBegin(image.data(), image.size(), c) != 0x1000)
        return Fail("chained .pdata entries must resolve to their primary function");
    if (PrimaryFunctionBegin(image.data(), image.size(), {0x5000, 0x5100, 0x3FE}) != 0x5000 ||
        PrimaryFunctionBegin(image.data(), image.size(), {0x6000, 0x6100, 0x201}) != 0x6000)
        return Fail("out-of-image or indirect unwind info must keep the entry's own start");

    // XenonRecomp thunks: jmp rel32, endbr64 + jmp rel8, backward rel8, a self
    // loop bounded by maxHops, and an unreadable address.
    CodeBuffer code{0x10000, std::vector<uint8_t>(0x200, 0x90)};
    PutCode(code, 0x10000, {0xE9, 0xFB, 0x00, 0x00, 0x00});
    PutCode(code, 0x10100, {0xF3, 0x0F, 0x1E, 0xFA, 0xEB, 0x10});
    PutCode(code, 0x10116, {0x48, 0x89, 0x5C});
    PutCode(code, 0x10180, {0xEB, 0xF0});
    PutCode(code, 0x10172, {0xC3});
    PutCode(code, 0x101A0, {0xE9, 0xFB, 0xFF, 0xFF, 0xFF});
    using mojorecomp::host::ResolveJumpThunk;
    if (ResolveJumpThunk(0x10000, ReadCode, &code) != 0x10116)
        return Fail("thunk chain must reach the body");
    if (ResolveJumpThunk(0x10180, ReadCode, &code) != 0x10172)
        return Fail("a backward jmp rel8 must be followed");
    if (ResolveJumpThunk(0x101A0, ReadCode, &code, 4) != 0x101A0)
        return Fail("a self loop must stop after maxHops");
    if (ResolveJumpThunk(0x20000, ReadCode, &code) != 0x20000)
        return Fail("an unreadable address must be returned unchanged");

    // Function lookup, including the gaps between .pdata ranges (leaf code).
    HostCodeMap map;
    map.SetFunctions({a, b, c, {0x2000, 0x2400, 0x100}, {0x3000, 0x3100, 0x100}},
                     {0x1000, 0x1000, 0x1000, 0x2000, 0x3000});
    if (map.FunctionStart(0x1050) != 0x1000 || map.FunctionStart(0x1150) != 0x1000 ||
        map.FunctionStart(0x11FF) != 0x1000 || map.FunctionStart(0x2000) != 0x2000 ||
        map.FunctionStart(0x23FF) != 0x2000)
        return Fail("FunctionStart must return the primary function");
    if (map.FunctionStart(0x0FFF) != HostCodeMap::kNoFunction ||
        map.FunctionStart(0x1200) != HostCodeMap::kNoFunction ||
        map.FunctionStart(0x2400) != HostCodeMap::kNoFunction)
        return Fail("addresses outside every range must have no function");
    // Identical-code folding: two guest functions share host body 0x2000.
    map.SetGuestFunctions({{0x2000, 0x82100000}, {0x2000, 0x820F0000}, {0x1000, 0x82200000}});
    if (map.GuestFunctionCount() != 2 || map.GuestAtHostStart(0x2000) != 0x820F0000 ||
        map.GuestAtHostStart(0x1000) != 0x82200000 || map.GuestAtHostStart(0x3000) != 0)
        return Fail("a folded body must keep the lower guest address");
    if (map.GuestFunctionContaining(0x82100010) != 0x82100000 ||
        map.GuestFunctionContaining(0x820F0000) != 0x820F0000 ||
        map.GuestFunctionContaining(0x82200500) != 0x82200000 ||
        map.GuestFunctionContaining(0x820E0000) != 0)
        return Fail("guest call sites must map to the guest function containing them");

    // Precedence: spin tags, translated guest code, import/wait/native tags,
    // other image code, other modules.
    const uint64_t base = 0x140000000ull;
    const uint64_t size = 0x10000;
    RawSample sample{};
    sample.ip = base + 0x2010;
    sample.spin = uint8_t(GuestSpin::CpPoll);
    if (!Is(ClassifySample(sample, map, base, size), SampleKind::Spin, uint64_t(GuestSpin::CpPoll)))
        return Fail("a spin tag must win");
    sample.spin = 0;
    sample.activity = uint8_t(GuestActivity::Import);
    sample.name = kWaitName;
    if (!Is(ClassifySample(sample, map, base, size), SampleKind::Guest, 0x820F0000))
        return Fail("translated guest code must win over an import tag");
    sample.ip = base + 0x1150;
    if (!Is(ClassifySample(sample, map, base, size), SampleKind::Guest, 0x82200000))
        return Fail("a chained part of a guest body must count as that guest function");
    sample.ip = base + 0x3010;
    sample.activity = uint8_t(GuestActivity::Wait);
    const uint64_t nameId = uint64_t(reinterpret_cast<uintptr_t>(kWaitName));
    if (!Is(ClassifySample(sample, map, base, size), SampleKind::Wait, nameId))
        return Fail("runtime code under a wait tag must count as that wait");
    sample.activity = uint8_t(GuestActivity::NativeCrt);
    if (!Is(ClassifySample(sample, map, base, size), SampleKind::Native, nameId))
        return Fail("runtime code under a native tag must count as native");
    sample.activity = 0;
    sample.name = nullptr;
    if (!Is(ClassifySample(sample, map, base, size), SampleKind::Host, 0x3000))
        return Fail("untagged runtime code must count as its host function");
    sample.ip = base + 0x1500;
    if (!Is(ClassifySample(sample, map, base, size), SampleKind::Host, HostCodeMap::kNoFunction))
        return Fail("image code outside .pdata must be an unknown host leaf");
    sample.ip = 0x7FF000000000ull;
    if (!Is(ClassifySample(sample, map, base, size), SampleKind::System, 0))
        return Fail("code outside the image must count as system");
    sample.activity = uint8_t(GuestActivity::Import);
    sample.name = kWaitName;
    if (!Is(ClassifySample(sample, map, base, size), SampleKind::Import, nameId))
        return Fail("a system module under an import tag must count as that import");

    // The activity scope restores the enclosing tag; nothing is written without
    // a registered slot.
    mojorecomp::host::GuestProfileSlot slot;
    {
        const mojorecomp::host::GuestActivityScope unregistered(GuestActivity::Wait, "x", 1);
        mojorecomp::host::SetGuestSpin(GuestSpin::CpPoll);
    }
    mojorecomp::host::t_guestProfileSlot = &slot;
    {
        const mojorecomp::host::GuestActivityScope outer(GuestActivity::Import, kWaitName,
                                                         0x82100000);
        {
            const mojorecomp::host::GuestActivityScope inner(GuestActivity::NativeCrt, "memcpy", 0);
            if (slot.activity.load() != uint8_t(GuestActivity::NativeCrt) ||
                std::strcmp(slot.name.load(), "memcpy") != 0)
                return Fail("the inner scope must set its tag");
        }
        if (slot.activity.load() != uint8_t(GuestActivity::Import) ||
            slot.name.load() != kWaitName || slot.guestCaller.load() != 0x82100000)
            return Fail("leaving a scope must restore the enclosing tag");
        mojorecomp::host::SetGuestSpin(GuestSpin::FrameGateIdle);
    }
    mojorecomp::host::t_guestProfileSlot = nullptr;
    if (slot.activity.load() != 0 || slot.name.load() != nullptr ||
        slot.spin.load() != uint8_t(GuestSpin::FrameGateIdle))
        return Fail("scopes must end untagged and spin must be set on the slot");

    // Report: kind shares, top functions, buckets and wait callers.
    GuestProfileAggregate aggregate;
    aggregate.Add({SampleKind::Guest, 0x82100000}, 0, 3);
    aggregate.Add({SampleKind::Wait, nameId}, 0x82200000, 2);
    aggregate.Add({SampleKind::Host, 0x3000});
    const auto lines = mojorecomp::host::FormatGuestProfileReport(
        aggregate, "window", "main", 2.0, Name);
    const std::vector<std::string> expected = {
        "[guest-profile] summary scope=window thread=main seconds=2.0 samples=6 rate=3/s "
        "guest=50.00 spin=0.00 wait=33.33 import=0.00 native=0.00 host=16.67 system=0.00",
        "[guest-profile] fn scope=window thread=main rank=1 pct=50.00 samples=3 name=sub_82100000",
        "[guest-profile] bucket scope=window thread=main rank=1 pct=33.33 samples=2 "
        "name=wait:KeWaitForSingleObject",
        "[guest-profile] caller scope=window thread=main bucket=wait:KeWaitForSingleObject "
        "pct=33.33 samples=2 name=sub_82200000",
        "[guest-profile] bucket scope=window thread=main rank=2 pct=16.67 samples=1 name=host:test",
    };
    if (lines != expected)
    {
        for (const auto& line : lines)
            std::fprintf(stderr, "%s\n", line.c_str());
        return Fail("report lines differ from the documented format");
    }

    GuestProfileAggregate total;
    total.Merge(aggregate);
    total.Merge(aggregate);
    if (total.Total() != 12 || total.KindTotal(SampleKind::Guest) != 6 ||
        total.SortedCallers({SampleKind::Wait, nameId}).front().second != 4)
        return Fail("Merge must add counts, kind totals and callers");
    total.Clear();
    if (total.Total() != 0 || !total.Sorted().empty() ||
        mojorecomp::host::FormatGuestProfileReport(total, "total", "main", 1.0, Name).size() != 1)
        return Fail("a cleared aggregate must report only its summary");

    std::puts("PASS: guest profiler .pdata chains, thunks, classification, tags and report.");
    return 0;
}
