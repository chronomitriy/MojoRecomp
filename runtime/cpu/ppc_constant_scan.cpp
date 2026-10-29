#include "ppc_constant_scan.h"

#include <array>
#include <cstdio>
#include <cstring>

namespace mojorecomp::cpu {
namespace {

constexpr uint32_t Op(uint32_t w) noexcept { return w >> 26; }
constexpr uint32_t FieldD(uint32_t w) noexcept { return (w >> 21) & 31u; }
constexpr uint32_t FieldA(uint32_t w) noexcept { return (w >> 16) & 31u; }
constexpr uint32_t FieldB(uint32_t w) noexcept { return (w >> 11) & 31u; }
constexpr uint32_t FieldC(uint32_t w) noexcept { return (w >> 6) & 31u; }
constexpr uint32_t Xo10(uint32_t w) noexcept { return (w >> 1) & 0x3FFu; }
constexpr uint32_t Xo5(uint32_t w) noexcept { return (w >> 1) & 0x1Fu; }
constexpr int32_t Simm(uint32_t w) noexcept { return int16_t(w & 0xFFFFu); }
constexpr uint32_t Uimm(uint32_t w) noexcept { return w & 0xFFFFu; }

constexpr bool IsFpLoad(uint32_t op) noexcept { return op >= 48 && op <= 51; }
constexpr bool IsFpStore(uint32_t op) noexcept { return op >= 52 && op <= 55; }

// D-form update instructions write the effective address back to rA.
constexpr bool IsUpdateForm(uint32_t op) noexcept
{
    switch (op)
    {
        case 33: case 35: case 37: case 39: case 41: case 43: case 45:
        case 49: case 51: case 53: case 55:
            return true;
        default:
            return false;
    }
}

// Opcode 31 extended opcodes that write no GPR other than an update-form rA.
bool Op31WritesNoGpr(uint32_t xo, bool& updatesA) noexcept
{
    updatesA = false;
    switch (xo)
    {
        case 183: case 247: case 439: case 181: case 695: case 759:
            updatesA = true; // stwux, stbux, sthux, stdux, stfsux, stfdux
            return true;
        case 151: case 215: case 407: case 149: case 663: case 727: case 983:
        case 231: case 487: case 135: case 167: case 199: case 918: case 662:
        case 150: case 214: case 725: case 661:
        case 0: case 32: case 4: case 68:
        case 54: case 86: case 246: case 278: case 470: case 758: case 982:
        case 1014: case 598: case 854: case 467: case 144: case 146: case 178:
            return true;
        default:
            return false;
    }
}

// FP opcode 63 A-form instructions have a 5-bit extended opcode >= 16; every
// 63 X-form instruction's low five extended-opcode bits are below 16.
constexpr bool Op63IsAForm(uint32_t w) noexcept { return Xo5(w) >= 16; }

struct FprUse
{
    bool a = false;
    bool b = false;
    bool c = false;
    bool s = false; // stores: frS in the D field
    bool writesD = false;
};

FprUse ClassifyFpr(uint32_t w) noexcept
{
    FprUse use;
    const uint32_t op = Op(w);
    if (IsFpLoad(op))
    {
        use.writesD = true;
        return use;
    }
    if (IsFpStore(op))
    {
        use.s = true;
        return use;
    }
    if (op == 31)
    {
        switch (Xo10(w))
        {
            case 535: case 567: case 599: case 631: // lfsx, lfsux, lfdx, lfdux
                use.writesD = true;
                break;
            case 663: case 695: case 727: case 759: case 983: // stfsx..stfiwx
                use.s = true;
                break;
            default:
                break;
        }
        return use;
    }
    if (op == 59 || (op == 63 && Op63IsAForm(w)))
    {
        use.writesD = true;
        switch (Xo5(w))
        {
            case 18: case 20: case 21: use.a = use.b = true; break; // div, sub, add
            case 22: case 24: case 26: use.b = true; break;          // sqrt, fres, frsqrte
            case 23: use.a = use.b = use.c = true; break;            // fsel
            case 25: use.a = use.c = true; break;                    // mul
            case 28: case 29: case 30: case 31: use.a = use.b = use.c = true; break;
            default: use.a = use.b = use.c = true; break;
        }
        return use;
    }
    if (op == 63)
    {
        switch (Xo10(w))
        {
            case 0: case 32: // fcmpu, fcmpo
                use.a = use.b = true;
                break;
            case 711: // mtfsf
                use.b = true;
                break;
            case 38: case 64: case 70: case 134: // mtfsb1, mcrfs, mtfsb0, mtfsfi
                break;
            case 583: // mffs
                use.writesD = true;
                break;
            default: // frsp, fctiw(z), fneg, fmr, fnabs, fabs, fctid(z), fcfid
                use.b = true;
                use.writesD = true;
                break;
        }
    }
    return use;
}

constexpr bool IsCall(uint32_t w) noexcept
{
    const uint32_t op = Op(w);
    if ((op == 18 || op == 16) && (w & 1u))
        return true;
    return op == 19 && (w & 1u) && (Xo10(w) == 16 || Xo10(w) == 528);
}

constexpr bool IsUnconditionalTransfer(uint32_t w) noexcept
{
    const uint32_t op = Op(w);
    if (op == 18)
        return !(w & 1u);
    if (op == 19 && (Xo10(w) == 16 || Xo10(w) == 528) && !(w & 1u))
        return (FieldD(w) & 0x14u) == 0x14u; // BO "branch always"
    return false;
}

struct KnownGpr
{
    bool valid = false;
    uint32_t value = 0;
    uint32_t setAt = 0;
};

class GprTracker
{
public:
    void Begin(uint32_t address) noexcept
    {
        current_ = address;
        for (auto& reg : regs_)
        {
            if (reg.valid &&
                current_ - reg.setAt > kPpcAddressWindowInstructions * 4u)
                reg.valid = false;
        }
    }

    bool Get(uint32_t reg, uint32_t& value) const noexcept
    {
        if (reg == 0 || !regs_[reg].valid)
            return false;
        value = regs_[reg].value;
        return true;
    }

    void Set(uint32_t reg, uint32_t value) noexcept { regs_[reg] = {true, value, current_}; }
    void Clear(uint32_t reg) noexcept { regs_[reg].valid = false; }
    void ClearAll() noexcept { regs_ = {}; }
    void ClearVolatile() noexcept
    {
        Clear(0);
        for (uint32_t reg = 3; reg <= 12; ++reg)
            Clear(reg);
    }

private:
    std::array<KnownGpr, 32> regs_{};
    uint32_t current_ = 0;
};

void TrackGprWrites(uint32_t w, GprTracker& gprs) noexcept
{
    const uint32_t op = Op(w);
    const uint32_t d = FieldD(w);
    const uint32_t a = FieldA(w);
    uint32_t base = 0;

    switch (op)
    {
        case 14: // addi / li
            if (a == 0)
                gprs.Set(d, uint32_t(Simm(w)));
            else if (gprs.Get(a, base))
                gprs.Set(d, base + uint32_t(Simm(w)));
            else
                gprs.Clear(d);
            return;
        case 15: // addis / lis
            if (a == 0)
                gprs.Set(d, uint32_t(Simm(w)) << 16);
            else if (gprs.Get(a, base))
                gprs.Set(d, base + (uint32_t(Simm(w)) << 16));
            else
                gprs.Clear(d);
            return;
        case 24: // ori rA, rS, UIMM
            if (gprs.Get(d, base))
                gprs.Set(a, base | Uimm(w));
            else
                gprs.Clear(a);
            return;
        case 25: // oris
            if (gprs.Get(d, base))
                gprs.Set(a, base | (Uimm(w) << 16));
            else
                gprs.Clear(a);
            return;
        case 7: case 8: case 12: case 13:
        case 32: case 34: case 40: case 42: case 58:
            gprs.Clear(d);
            if (op == 58 && (w & 3u) == 1u)
                gprs.Clear(a); // ldu
            return;
        case 33: case 35: case 41: case 43: // lwzu, lbzu, lhzu, lhau: rA set by caller
            gprs.Clear(d);
            return;
        case 46: // lmw
            for (uint32_t reg = d; reg < 32; ++reg)
                gprs.Clear(reg);
            return;
        case 20: case 21: case 23: case 26: case 27: case 28: case 29: case 30:
            gprs.Clear(a);
            return;
        case 18:
            if (w & 1u)
                gprs.ClearVolatile();
            else
                gprs.ClearAll();
            return;
        case 16:
            if (w & 1u)
                gprs.ClearVolatile();
            return;
        case 19:
            if (IsCall(w))
                gprs.ClearVolatile();
            else if (IsUnconditionalTransfer(w))
                gprs.ClearAll();
            return;
        case 31:
        {
            bool updatesA = false;
            if (Op31WritesNoGpr(Xo10(w), updatesA))
            {
                if (updatesA)
                    gprs.Clear(a);
            }
            else
            {
                gprs.Clear(d);
                gprs.Clear(a);
            }
            return;
        }
        case 2: case 3: case 4: case 5: case 6: case 10: case 11: case 17:
        case 36: case 38: case 44: case 47: case 52: case 54: case 59: case 63:
        case 48: case 50:
            return;
        case 62: // std / stdu
            if ((w & 3u) == 1u)
                gprs.Clear(a);
            return;
        default:
            break;
    }

    if (IsUpdateForm(op))
        return; // handled by the caller once the effective address is known
    // Unknown or illegal encoding: forget everything rather than guess.
    gprs.ClearAll();
}

const char* FpArithmeticName(uint32_t w) noexcept
{
    const bool single = Op(w) == 59;
    switch (Xo5(w))
    {
        case 18: return single ? "fdivs" : "fdiv";
        case 20: return single ? "fsubs" : "fsub";
        case 21: return single ? "fadds" : "fadd";
        case 22: return single ? "fsqrts" : "fsqrt";
        case 23: return "fsel";
        case 24: return single ? "fres" : "fre";
        case 25: return single ? "fmuls" : "fmul";
        case 26: return "frsqrte";
        case 28: return single ? "fmsubs" : "fmsub";
        case 29: return single ? "fmadds" : "fmadd";
        case 30: return single ? "fnmsubs" : "fnmsub";
        case 31: return single ? "fnmadds" : "fnmadd";
        default: return nullptr;
    }
}

const char* Fp63XName(uint32_t w) noexcept
{
    switch (Xo10(w))
    {
        case 12: return "frsp";
        case 14: return "fctiw";
        case 15: return "fctiwz";
        case 40: return "fneg";
        case 72: return "fmr";
        case 136: return "fnabs";
        case 264: return "fabs";
        case 814: return "fctid";
        case 815: return "fctidz";
        case 846: return "fcfid";
        default: return nullptr;
    }
}

} // namespace

bool PpcInstructionReadsFpr(uint32_t word, uint32_t fpr) noexcept
{
    const FprUse use = ClassifyFpr(word);
    return (use.a && FieldA(word) == fpr) || (use.b && FieldB(word) == fpr) ||
           (use.c && FieldC(word) == fpr) || (use.s && FieldD(word) == fpr);
}

bool PpcInstructionWritesFpr(uint32_t word, uint32_t fpr) noexcept
{
    const FprUse use = ClassifyFpr(word);
    return use.writesD && FieldD(word) == fpr;
}

std::vector<PpcFloatConstantLoad> FindFloatConstantLoads(
    uint32_t codeBegin, uint32_t codeEnd, const GuestWordReader& read,
    const std::function<bool(double)>& matches)
{
    std::vector<PpcFloatConstantLoad> found;
    if (codeEnd <= codeBegin)
        return found;

    GprTracker gprs;
    codeBegin &= ~3u;
    for (uint64_t address64 = codeBegin; address64 + 4u <= codeEnd; address64 += 4u)
    {
        const uint32_t address = static_cast<uint32_t>(address64);
        uint32_t w = 0;
        if (!read(address, w))
        {
            gprs.ClearAll();
            continue;
        }
        gprs.Begin(address);

        const uint32_t op = Op(w);
        const uint32_t a = FieldA(w);
        uint32_t base = 0;
        const bool haveBase = a != 0 && gprs.Get(a, base);
        const uint32_t effective = base + uint32_t(Simm(w));

        if (IsFpLoad(op) && haveBase)
        {
            const bool isDouble = op >= 50;
            double value = 0.0;
            bool loaded = false;
            uint32_t hi = 0;
            uint32_t lo = 0;
            if (isDouble)
            {
                if (read(effective, hi) && read(effective + 4u, lo))
                {
                    const uint64_t bits = (uint64_t(hi) << 32) | lo;
                    std::memcpy(&value, &bits, sizeof(value));
                    loaded = true;
                }
            }
            else if (read(effective, hi))
            {
                float single = 0.0f;
                std::memcpy(&single, &hi, sizeof(single));
                value = single;
                loaded = true;
            }

            if (loaded && matches(value))
            {
                PpcFloatConstantLoad hit;
                hit.loadAddress = address;
                hit.loadWord = w;
                hit.constantAddress = effective;
                hit.fpr = FieldD(w);
                hit.doublePrecision = isDouble;
                hit.value = value;
                for (uint32_t i = 1; i <= kPpcUseWindowInstructions; ++i)
                {
                    const uint64_t next = address64 + uint64_t(i) * 4u;
                    uint32_t nw = 0;
                    if (next + 4u > codeEnd || !read(static_cast<uint32_t>(next), nw))
                        break;
                    if (PpcInstructionReadsFpr(nw, hit.fpr) || IsCall(nw))
                    {
                        hit.useAddress = static_cast<uint32_t>(next);
                        hit.useWord = nw;
                        break;
                    }
                    if (PpcInstructionWritesFpr(nw, hit.fpr) || IsUnconditionalTransfer(nw))
                        break;
                }
                found.push_back(hit);
            }
        }

        if (IsUpdateForm(op))
        {
            if (haveBase)
                gprs.Set(a, effective);
            else
                gprs.Clear(a);
        }
        TrackGprWrites(w, gprs);
    }
    return found;
}

std::string DescribePpcInstruction(uint32_t w, uint32_t address)
{
    char text[96];
    const uint32_t op = Op(w);
    static constexpr const char* kFpMem[] = {"lfs", "lfsu", "lfd", "lfdu",
                                             "stfs", "stfsu", "stfd", "stfdu"};
    if (op >= 48 && op <= 55)
    {
        std::snprintf(text, sizeof(text), "%s f%u,%d(r%u)", kFpMem[op - 48],
                      FieldD(w), Simm(w), FieldA(w));
        return text;
    }
    if (op == 63 && (Xo10(w) == 0 || Xo10(w) == 32))
    {
        std::snprintf(text, sizeof(text), "%s cr%u,f%u,f%u",
                      Xo10(w) == 0 ? "fcmpu" : "fcmpo", FieldD(w) >> 2,
                      FieldA(w), FieldB(w));
        return text;
    }
    if (op == 59 || (op == 63 && Op63IsAForm(w)))
    {
        if (const char* name = FpArithmeticName(w))
        {
            const FprUse use = ClassifyFpr(w);
            int n = std::snprintf(text, sizeof(text), "%s f%u", name, FieldD(w));
            if (use.a)
                n += std::snprintf(text + n, sizeof(text) - n, ",f%u", FieldA(w));
            if (use.c)
                n += std::snprintf(text + n, sizeof(text) - n, ",f%u", FieldC(w));
            if (use.b)
                std::snprintf(text + n, sizeof(text) - n, ",f%u", FieldB(w));
            return text;
        }
    }
    if (op == 63)
    {
        if (const char* name = Fp63XName(w))
        {
            std::snprintf(text, sizeof(text), "%s f%u,f%u", name, FieldD(w), FieldB(w));
            return text;
        }
    }
    if (op == 18)
    {
        int32_t li = int32_t(w & 0x03FFFFFCu);
        if (li & 0x02000000)
            li -= 0x04000000;
        const uint32_t target = (w & 2u) ? uint32_t(li) : address + uint32_t(li);
        std::snprintf(text, sizeof(text), "%s 0x%08X", (w & 1u) ? "bl" : "b", target);
        return text;
    }
    if (op == 19 && (Xo10(w) == 16 || Xo10(w) == 528))
    {
        std::snprintf(text, sizeof(text), "%s%s", Xo10(w) == 16 ? "bclr" : "bcctr",
                      (w & 1u) ? "l" : "");
        return text;
    }
    std::snprintf(text, sizeof(text), ".long 0x%08X", w);
    return text;
}

std::vector<uint32_t> WalkGuestBackChain(uint32_t sp, uint32_t maxDepth,
                                         const GuestWordReader& read,
                                         uint32_t codeBegin, uint32_t codeEnd)
{
    constexpr uint32_t kMaxFrameBytes = 0x100000u;
    std::vector<uint32_t> chain;
    for (uint32_t depth = 0; depth < maxDepth; ++depth)
    {
        uint32_t back = 0;
        uint32_t returnAddress = 0;
        if (!sp || (sp & 3u) || !read(sp, back))
            break;
        if (back <= sp || back - sp > kMaxFrameBytes || (back & 3u) || back < 8u)
            break;
        if (!read(back - 8u, returnAddress))
            break;
        if (returnAddress < codeBegin || returnAddress >= codeEnd || (returnAddress & 3u))
            break;
        chain.push_back(returnAddress);
        sp = back;
    }
    return chain;
}

} // namespace mojorecomp::cpu
