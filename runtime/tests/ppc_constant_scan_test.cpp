#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include "../cpu/ppc_constant_scan.h"

namespace {

using mojorecomp::cpu::DescribePpcInstruction;
using mojorecomp::cpu::FindFloatConstantLoads;
using mojorecomp::cpu::GuestWordReader;
using mojorecomp::cpu::PpcInstructionReadsFpr;
using mojorecomp::cpu::PpcInstructionWritesFpr;
using mojorecomp::cpu::WalkGuestBackChain;

int Fail(const char* message)
{
    std::fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

// Minimal synthetic Xenon encodings. No game code or data is involved.
constexpr uint32_t D(uint32_t op, uint32_t d, uint32_t a, int32_t imm)
{
    return (op << 26) | (d << 21) | (a << 16) | (uint32_t(imm) & 0xFFFFu);
}
constexpr uint32_t Lis(uint32_t d, uint32_t hi) { return D(15, d, 0, int32_t(int16_t(hi))); }
constexpr uint32_t Addi(uint32_t d, uint32_t a, int32_t imm) { return D(14, d, a, imm); }
constexpr uint32_t Lfs(uint32_t f, uint32_t a, int32_t imm) { return D(48, f, a, imm); }
constexpr uint32_t Lfsu(uint32_t f, uint32_t a, int32_t imm) { return D(49, f, a, imm); }
constexpr uint32_t Lfd(uint32_t f, uint32_t a, int32_t imm) { return D(50, f, a, imm); }
constexpr uint32_t Stfs(uint32_t f, uint32_t a, int32_t imm) { return D(52, f, a, imm); }
constexpr uint32_t Fcmpu(uint32_t crf, uint32_t fa, uint32_t fb)
{
    return (63u << 26) | ((crf << 2) << 21) | (fa << 16) | (fb << 11);
}
constexpr uint32_t FpA(uint32_t op, uint32_t xo, uint32_t d, uint32_t a, uint32_t b, uint32_t c)
{
    return (op << 26) | (d << 21) | (a << 16) | (b << 11) | (c << 6) | (xo << 1);
}
constexpr uint32_t Fsub(uint32_t d, uint32_t a, uint32_t b) { return FpA(63, 20, d, a, b, 0); }
constexpr uint32_t Fsel(uint32_t d, uint32_t a, uint32_t c, uint32_t b) { return FpA(63, 23, d, a, b, c); }
constexpr uint32_t Fmuls(uint32_t d, uint32_t a, uint32_t c) { return FpA(59, 25, d, a, 0, c); }
constexpr uint32_t Bl(int32_t offset) { return (18u << 26) | (uint32_t(offset) & 0x03FFFFFCu) | 1u; }
constexpr uint32_t B(int32_t offset) { return (18u << 26) | (uint32_t(offset) & 0x03FFFFFCu); }
constexpr uint32_t kNop = 0x60000000u;
constexpr uint32_t kBlr = 0x4E800020u;

constexpr uint32_t kCode = 0x82100000u;
constexpr uint32_t kFloor = 0x82001000u;       // float 1/60
constexpr uint32_t kFloorDouble = 0x82001008u; // double 1/60
constexpr uint32_t kHalf = 0x82001010u;        // float 0.5
constexpr uint32_t kFloorHigh = 0x82009000u;   // float 1/60 reached with a negative displacement

struct Guest
{
    std::map<uint32_t, uint32_t> words;
    uint32_t codeEnd = kCode;

    Guest()
    {
        float floor = 1.0f / 60.0f;
        uint32_t bits = 0;
        std::memcpy(&bits, &floor, sizeof(bits));
        words[kFloor] = bits;
        words[kFloorHigh] = bits;
        const double floorDouble = 1.0 / 60.0;
        uint64_t dbits = 0;
        std::memcpy(&dbits, &floorDouble, sizeof(dbits));
        words[kFloorDouble] = uint32_t(dbits >> 32);
        words[kFloorDouble + 4] = uint32_t(dbits);
        const float half = 0.5f;
        std::memcpy(&bits, &half, sizeof(bits));
        words[kHalf] = bits;
    }

    uint32_t Emit(uint32_t word)
    {
        const uint32_t at = codeEnd;
        words[at] = word;
        codeEnd += 4;
        return at;
    }

    GuestWordReader Reader() const
    {
        return [this](uint32_t address, uint32_t& word) {
            const auto it = words.find(address);
            if (it == words.end())
                return false;
            word = it->second;
            return true;
        };
    }
};

bool IsSixtieth(double value)
{
    return std::fabs(value - 1.0 / 60.0) < 1.0e-6;
}

} // namespace

int main()
{
    {
        Guest g;
        g.Emit(Lis(11, 0x8201));
        const uint32_t load = g.Emit(Lfs(0, 11, -0x7000)); // 0x82010000 - 0x7000
        const uint32_t compare = g.Emit(Fcmpu(6, 1, 0));
        g.Emit(kBlr);
        const auto hits = FindFloatConstantLoads(kCode, g.codeEnd, g.Reader(), IsSixtieth);
        if (hits.size() != 1 || hits[0].loadAddress != load ||
            hits[0].constantAddress != kFloorHigh || hits[0].fpr != 0 ||
            hits[0].doublePrecision || hits[0].useAddress != compare)
            return Fail("lis + negative-displacement lfs + fcmpu was not reported exactly");
        if (DescribePpcInstruction(hits[0].loadWord, load) != "lfs f0,-28672(r11)" ||
            DescribePpcInstruction(hits[0].useWord, compare) != "fcmpu cr6,f1,f0")
            return Fail("scanner disassembly of the floor load or compare changed");
    }

    {
        Guest g;
        g.Emit(Lis(10, 0x8200));
        g.Emit(Addi(10, 10, 0x1008));
        const uint32_t load = g.Emit(Lfd(13, 10, 0));
        const uint32_t use = g.Emit(Fsub(12, 1, 13));
        const auto hits = FindFloatConstantLoads(kCode, g.codeEnd, g.Reader(), IsSixtieth);
        if (hits.size() != 1 || hits[0].loadAddress != load || !hits[0].doublePrecision ||
            hits[0].constantAddress != kFloorDouble || hits[0].useAddress != use ||
            DescribePpcInstruction(hits[0].useWord, use) != "fsub f12,f1,f13")
            return Fail("lis + addi + lfd double constant was not reported");
    }

    {
        Guest g;
        g.Emit(Lis(11, 0x8200));
        g.Emit(Bl(0x100));
        g.Emit(Lfs(0, 11, 0x1000));
        g.Emit(Lis(31, 0x8200));
        g.Emit(Bl(0x100));
        const uint32_t kept = g.Emit(Lfs(0, 31, 0x1000));
        const auto hits = FindFloatConstantLoads(kCode, g.codeEnd, g.Reader(), IsSixtieth);
        if (hits.size() != 1 || hits[0].loadAddress != kept)
            return Fail("calls must clobber volatile bases but keep non-volatile r31");
    }

    {
        Guest g;
        g.Emit(Lis(11, 0x8200));
        for (int i = 0; i < 70; ++i)
            g.Emit(kNop);
        g.Emit(Lfs(0, 11, 0x1000));
        g.Emit(Lis(11, 0x8200));
        g.Emit(B(8));
        g.Emit(Lfs(0, 11, 0x1000));
        if (!FindFloatConstantLoads(kCode, g.codeEnd, g.Reader(), IsSixtieth).empty())
            return Fail("stale or branched-over base registers produced a candidate");
    }

    {
        Guest g;
        g.Emit(Lis(11, 0x8200));
        g.Emit(Lfs(0, 11, 0x1000));
        g.Emit(Lfs(0, 11, 0x1010)); // overwrites f0 with 0.5 before any use
        g.Emit(Fcmpu(6, 1, 0));
        const auto hits = FindFloatConstantLoads(kCode, g.codeEnd, g.Reader(), IsSixtieth);
        if (hits.size() != 1 || hits[0].useAddress != 0)
            return Fail("an overwritten FPR was reported as used, or 0.5 matched 1/60");
    }

    {
        Guest g;
        g.Emit(Lis(11, 0x8200));
        g.Emit(Lfs(2, 11, 0x1000));
        g.Emit(Fmuls(3, 4, 5));
        const uint32_t sel = g.Emit(Fsel(1, 13, 2, 1));
        g.Emit(Lfsu(4, 11, 0x1000)); // update form: r11 becomes 0x82001000
        const uint32_t store = g.Emit(Stfs(4, 31, 12));
        g.Emit(Lfs(5, 11, 0x10));    // 0x82001010 is 0.5
        const auto hits = FindFloatConstantLoads(kCode, g.codeEnd, g.Reader(), IsSixtieth);
        if (hits.size() != 2 || hits[0].useAddress != sel || hits[1].useAddress != store)
            return Fail("fsel frC and stfs uses, or lfsu base update, were not tracked");
        if (DescribePpcInstruction(hits[0].useWord, sel) != "fsel f1,f13,f2,f1" ||
            DescribePpcInstruction(hits[1].useWord, store) != "stfs f4,12(r31)")
            return Fail("fsel/stfs disassembly changed");
    }

    if (PpcInstructionReadsFpr(Fmuls(3, 1, 2), 0) || !PpcInstructionReadsFpr(Fmuls(3, 1, 2), 2) ||
        !PpcInstructionWritesFpr(Fmuls(3, 1, 2), 3) || PpcInstructionWritesFpr(Fcmpu(6, 1, 0), 0))
        return Fail("FP operand classification reads unused fields or writes on compare");
    if (DescribePpcInstruction(Bl(0x100), 0x82000010u) != "bl 0x82000110" ||
        DescribePpcInstruction(Bl(-0x10), 0x82000010u) != "bl 0x82000000")
        return Fail("relative call targets are wrong");

    {
        std::map<uint32_t, uint32_t> stack = {
            {0x70000000u, 0x70000080u}, {0x70000078u, 0x82100010u},
            {0x70000080u, 0x70000100u}, {0x700000F8u, 0x82200020u},
            {0x70000100u, 0x70000200u}, {0x700001F8u, 0x00000000u},
        };
        const GuestWordReader read = [&](uint32_t address, uint32_t& word) {
            const auto it = stack.find(address);
            if (it == stack.end())
                return false;
            word = it->second;
            return true;
        };
        const auto chain = WalkGuestBackChain(0x70000000u, 8, read, 0x82000000u, 0x83000000u);
        if (chain.size() != 2 || chain[0] != 0x82100010u || chain[1] != 0x82200020u)
            return Fail("guest back-chain walk did not follow saved LR slots");
        stack[0x70000000u] = 0x6FFFFF00u;
        if (!WalkGuestBackChain(0x70000000u, 8, read, 0x82000000u, 0x83000000u).empty())
            return Fail("a back chain that points downwards was followed");
    }

    std::puts("PASS: PowerPC float-constant scan and back-chain walk.");
    return 0;
}
