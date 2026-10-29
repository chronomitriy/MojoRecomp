#pragma once

// Static PowerPC helpers used to locate guest code that loads a specific
// floating-point constant. They decode the original big-endian Xenon code that
// the runtime copies into guest memory; they never touch translated host code.
//
// The scan is intentionally simple and conservative. It follows one
// straight-line window of `lis`/`addis`/`addi`/`ori` address materialization
// and reports `lfs`/`lfd` loads whose effective address holds a matching value.
// It is a diagnostic aid that produces candidates for review, not a patcher.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mojorecomp::cpu {

// Reads one aligned big-endian guest word. Returns false when the address is
// outside the range the caller allows.
using GuestWordReader = std::function<bool(uint32_t address, uint32_t& word)>;

struct PpcFloatConstantLoad
{
    uint32_t loadAddress = 0;      // lfs/lfsu/lfd/lfdu instruction
    uint32_t loadWord = 0;
    uint32_t constantAddress = 0;  // effective address of the constant
    uint32_t fpr = 0;              // destination FPR
    bool doublePrecision = false;  // lfd/lfdu
    double value = 0.0;
    // First instruction after the load that reads the loaded FPR, or a call
    // that may consume it as an argument. Zero when the window ends first.
    uint32_t useAddress = 0;
    uint32_t useWord = 0;
};

// Instructions a materialized base register stays valid for. MSVC for Xenon
// emits the `lis` a handful of instructions before the dependent load.
inline constexpr uint32_t kPpcAddressWindowInstructions = 64;
// Instructions searched after a load for its first use.
inline constexpr uint32_t kPpcUseWindowInstructions = 16;

std::vector<PpcFloatConstantLoad> FindFloatConstantLoads(
    uint32_t codeBegin, uint32_t codeEnd, const GuestWordReader& read,
    const std::function<bool(double)>& matches);

bool PpcInstructionReadsFpr(uint32_t word, uint32_t fpr) noexcept;
bool PpcInstructionWritesFpr(uint32_t word, uint32_t fpr) noexcept;

// Short disassembly for the instruction forms the scanner reports (FP loads,
// stores, compares, arithmetic, branches). Other words print as `.long`.
std::string DescribePpcInstruction(uint32_t word, uint32_t address);

// Return addresses of the guest call chain, innermost first. Xenon ABI: 0(sp)
// holds the caller's stack pointer and each function saves its LR at -8 of the
// caller's frame. The walk stops at the first implausible frame or return
// address outside [codeBegin, codeEnd).
std::vector<uint32_t> WalkGuestBackChain(uint32_t sp, uint32_t maxDepth,
                                         const GuestWordReader& read,
                                         uint32_t codeBegin, uint32_t codeEnd);

} // namespace mojorecomp::cpu
