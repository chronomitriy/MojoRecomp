#pragma once

// Native equivalents of the title's libc copy and fill routines. Each is a
// mid-asm hook at the guest function's entry (config/CrashOfTheTitans.toml)
// that returns true when it did the whole job, and false to let the original
// translated code run. They decline every case in which a flat host copy could
// differ from the guest routine (overlap, physical aliases, address wrap), so
// the result is always the original routine's result.

#include <cstdint>

namespace mojorecomp::guest_native {

// MOJORECOMP_NATIVE_GUEST_CRT=0 makes every hook decline (original code only).
bool Enabled() noexcept;
// Tests run the original and the native path on the same inputs.
void SetEnabledForTesting(bool enabled) noexcept;

// Copies bytes forward from src to dst in the flat guest mapping. Returns false,
// without touching memory, when the ranges overlap (also through the physical
// alias views) or leave the 32-bit guest space.
bool CopyNonOverlapping(uint8_t* base, uint32_t dst, uint32_t src, uint32_t bytes) noexcept;
// Fills bytes at dst. Returns false when the range leaves the guest space.
bool Fill(uint8_t* base, uint32_t dst, uint8_t value, uint32_t bytes) noexcept;

} // namespace mojorecomp::guest_native
