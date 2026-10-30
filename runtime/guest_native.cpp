// Native equivalents of the title's libc copy and fill routines. See the header.

#include "guest_native.h"

#include "ppc_recomp_shared.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "runtime_state.h"

namespace mojorecomp::guest_native {
namespace {

// Physical memory is mapped three times (0xA0000000, 0xC0000000, 0xE0000000),
// so two different guest addresses can name the same bytes.
constexpr uint32_t kPhysicalViewBase = 0xA0000000u;
constexpr uint32_t kPhysicalViewBytes = 0x20000000u;
constexpr uint64_t kGuestSpaceEnd = 0x100000000ull;

// -1 until read from the environment.
std::atomic<int> g_enabled{-1};

struct Span
{
    bool physical = false;
    uint64_t begin = 0;
    uint64_t end = 0;
};

bool Normalize(uint32_t address, uint32_t bytes, Span& out) noexcept
{
    const uint64_t end = uint64_t(address) + bytes;
    if (end > kGuestSpaceEnd)
        return false;
    if (address < kPhysicalViewBase)
    {
        if (end > kPhysicalViewBase)
            return false;
        out = {false, address, end};
        return true;
    }
    const uint64_t offset = (address - kPhysicalViewBase) % kPhysicalViewBytes;
    if (offset + bytes > kPhysicalViewBytes)
        return false;
    out = {true, offset, offset + bytes};
    return true;
}

} // namespace

bool Enabled() noexcept
{
    int value = g_enabled.load(std::memory_order_relaxed);
    if (value < 0)
    {
        const char* setting = std::getenv("MOJORECOMP_NATIVE_GUEST_CRT");
        value = setting && setting[0] == '0' ? 0 : 1;
        g_enabled.store(value, std::memory_order_relaxed);
    }
    return value != 0;
}

void SetEnabledForTesting(bool enabled) noexcept
{
    g_enabled.store(enabled ? 1 : 0, std::memory_order_relaxed);
}

bool CopyNonOverlapping(uint8_t* base, uint32_t dst, uint32_t src, uint32_t bytes) noexcept
{
    if (!base)
        return false;
    if (!bytes)
        return true;
    Span to;
    Span from;
    if (!Normalize(dst, bytes, to) || !Normalize(src, bytes, from))
        return false;
    // The guest routines copy forward in 1-, 4-, 8- and 128-byte steps, so an
    // overlapping copy depends on their exact order: leave it to them.
    if (to.physical == from.physical && to.begin < from.end && from.begin < to.end)
        return false;
    std::memcpy(base + dst, base + src, bytes);
    return true;
}

bool Fill(uint8_t* base, uint32_t dst, uint8_t value, uint32_t bytes) noexcept
{
    if (!base || uint64_t(dst) + bytes > kGuestSpaceEnd)
        return false;
    std::memset(base + dst, value, bytes);
    return true;
}

} // namespace mojorecomp::guest_native

// libc memcpy at 0x823CEC90: 8-byte and 128-byte block copy with prefetch hints.
bool MojoRecompGuestMemcpy(PPCRegister& dst, PPCRegister& src, PPCRegister& size)
{
    return mojorecomp::guest_native::Enabled() &&
           mojorecomp::guest_native::CopyNonOverlapping(g_mojoGuestBase, dst.u32, src.u32,
                                                        size.u32);
}

// Second forward copy at 0x823CF0E8: word copy with an unaligned-source path.
bool MojoRecompGuestCopyForward(PPCRegister& dst, PPCRegister& src, PPCRegister& size)
{
    return mojorecomp::guest_native::Enabled() &&
           mojorecomp::guest_native::CopyNonOverlapping(g_mojoGuestBase, dst.u32, src.u32,
                                                        size.u32);
}

// libc memset at 0x823CF440. It stores only the low byte of r4.
bool MojoRecompGuestMemset(PPCRegister& dst, PPCRegister& value, PPCRegister& size)
{
    return mojorecomp::guest_native::Enabled() &&
           mojorecomp::guest_native::Fill(g_mojoGuestBase, dst.u32, uint8_t(value.u32),
                                          size.u32);
}
