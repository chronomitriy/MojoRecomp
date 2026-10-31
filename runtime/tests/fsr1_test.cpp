#include <cmath>
#include <cstdio>
#include <limits>

#include "../gpu/fsr1.h"

namespace {

int Fail(const char* message)
{
    std::fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

// Within one float rounding of the expected value (the compiler may contract
// a * b - c into an FMA in either place).
bool Same(uint32_t bits, float expected)
{
    const float value = mojorecomp::gpu::fsr1::BitsFloat(bits);
    return std::fabs(value - expected) <= std::fabs(expected) * 2.0e-7f;
}

} // namespace

int main()
{
    namespace fsr1 = mojorecomp::gpu::fsr1;

    // FsrEasuCon for 1280x720 -> 2560x1440 (the 1x-to-1440p case), written out
    // from ffx_fsr1.h's definitions rather than from the function under test.
    const auto easu = fsr1::ComputeEasuConstants(1280.0f, 720.0f, 1280.0f, 720.0f,
                                                 2560.0f, 1440.0f);
    if (easu.con0[0] != 0x3F000000u || easu.con0[2] != 0xBE800000u ||
        !Same(easu.con0[1], 0.5f) || !Same(easu.con0[3], -0.25f))
        return Fail("EASU con0 must be {0.5, 0.5, -0.25, -0.25} for a 2x upscale");
    if (!Same(easu.con1[0], 1.0f / 1280.0f) || !Same(easu.con1[1], 1.0f / 720.0f) ||
        !Same(easu.con1[2], 1.0f / 1280.0f) || !Same(easu.con1[3], -1.0f / 720.0f))
        return Fail("EASU con1 must hold the texel size and the first gather offset");
    if (!Same(easu.con2[0], -1.0f / 1280.0f) || !Same(easu.con2[1], 2.0f / 720.0f) ||
        !Same(easu.con2[2], 1.0f / 1280.0f) || !Same(easu.con2[3], 2.0f / 720.0f))
        return Fail("EASU con2 must hold the second and third gather offsets");
    if (easu.con3[0] != 0u || !Same(easu.con3[1], 4.0f / 720.0f) || easu.con3[2] != 0u ||
        easu.con3[3] != 0u)
        return Fail("EASU con3 must hold the fourth gather offset and zero padding");

    // FsrRcasCon: sharpness = 2^-stops in con[0], packed halves in con[1].
    const auto rcas0 = fsr1::ComputeRcasConstants(0.0f);
    if (rcas0.con[0] != 0x3F800000u || rcas0.con[1] != 0x3C003C00u || rcas0.con[2] ||
        rcas0.con[3])
        return Fail("RCAS at 0 stops must be 1.0 / half 0x3C00");
    const auto rcas1 = fsr1::ComputeRcasConstants(1.0f);
    if (rcas1.con[0] != 0x3F000000u || rcas1.con[1] != 0x38003800u)
        return Fail("RCAS at 1 stop must be 0.5 / half 0x3800");
    const auto rcas2 = fsr1::ComputeRcasConstants(2.0f);
    if (rcas2.con[0] != 0x3E800000u || rcas2.con[1] != 0x34003400u)
        return Fail("RCAS at 2 stops must be 0.25 / half 0x3400");

    // Half conversion: rounding, overflow and the subnormal range.
    if (fsr1::FloatToHalf(0.0f) != 0x0000u || fsr1::FloatToHalf(-0.0f) != 0x8000u ||
        fsr1::FloatToHalf(1.0f + 1.0f / 1024.0f) != 0x3C01u ||
        fsr1::FloatToHalf(1.0f + 1.0f / 2048.0f) != 0x3C00u ||
        fsr1::FloatToHalf(1.0f + 3.0f / 2048.0f) != 0x3C02u ||
        fsr1::FloatToHalf(65504.0f) != 0x7BFFu || fsr1::FloatToHalf(1.0e6f) != 0x7C00u ||
        fsr1::FloatToHalf(std::ldexp(1.0f, -24)) != 0x0001u ||
        fsr1::FloatToHalf(std::ldexp(1.0f, -14)) != 0x0400u)
        return Fail("FloatToHalf must round to nearest even and handle range limits");

    // Runtime 0..1 sharpness to AMD stops: 1 is sharpest, 0 is two stops softer.
    if (fsr1::SharpnessToStops(1.0f) != 0.0f || fsr1::SharpnessToStops(0.0f) != 2.0f ||
        std::fabs(fsr1::SharpnessToStops(0.8f) - 0.4f) > 1.0e-6f ||
        fsr1::SharpnessToStops(-1.0f) != 2.0f || fsr1::SharpnessToStops(2.0f) != 0.0f ||
        fsr1::SharpnessToStops(std::numeric_limits<float>::quiet_NaN()) != 2.0f)
        return Fail("SharpnessToStops must clamp to 0..2 stops");

    // EASU only scales up, and only when at least one dimension grows.
    if (!fsr1::EasuApplies(1280, 720, 2560, 1440) || fsr1::EasuApplies(2560, 1440, 2560, 1440) ||
        !fsr1::EasuApplies(1280, 720, 1280, 1440) || fsr1::EasuApplies(1280, 720, 1000, 1440) ||
        fsr1::EasuApplies(3840, 2160, 2560, 1440) || fsr1::EasuApplies(0, 720, 2560, 1440))
        return Fail("EasuApplies must accept upscales only");

    std::puts("PASS: FSR 1 EASU/RCAS constants, half packing, sharpness mapping and EASU gating.");
    return 0;
}
