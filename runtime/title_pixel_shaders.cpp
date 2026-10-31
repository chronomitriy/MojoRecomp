#include "title_pixel_shaders.h"

#include <cstdint>
#include <initializer_list>

namespace {

using mojorecomp::gpu::PixelShaderReplacement;
using mojorecomp::gpu::PixelShaderRole;
using mojorecomp::gpu::TitlePixelShader;

constexpr uint32_t Bits(std::initializer_list<uint32_t> bits)
{
    uint32_t mask = 0;
    for (uint32_t bit : bits)
        mask |= 1u << bit;
    return mask;
}

// Identified on the owner's PC from the game's own files; no game data is part
// of this repository. Hashes are PM4's FNV-1a over the microcode, the interface
// is what the runtime's translator reports for it.
constexpr TitlePixelShader kCrashOfTheTitansPixelShaders[] = {
    // The single full-screen "[front pass]" draw per frame (with VS
    // 821F51E34929F4DD). It samples s0 several times along a per-pixel
    // direction from TEXCOORD0, scaled by c251-c255, and writes the weighted
    // average with alpha 1 to the front buffer, so with motion blur off it must
    // still draw: as one fetch of s0 at TEXCOORD0.
    {
        0x89B4E4C8E904ABD5ull, "motion-blur", "shaders/motionblurfp.out",
        PixelShaderRole::MotionBlur, PixelShaderReplacement::Texture0Passthrough,
        165, Bits({0, 2, 3}), Bits({0, 2, 4}),
    },
    // Loaded, but not seen drawing in gameplay. Which input holds the sharp
    // image is unknown, so it is only observed: the log reports its interface
    // and its first draw.
    {
        0x62F77CF564D38C35ull, "depth-of-field", "DOF.out",
        // Drawn in gameplay (the distance blur): 8 depth-weighted taps of s0 around
        // the centre tap, s0 at TEXCOORD0. Off keeps only the centre tap.
        PixelShaderRole::DepthOfField, PixelShaderReplacement::Texture0Passthrough,
        504, Bits({0, 1}), Bits({0, 1}),
    },
};

} // namespace

std::span<const TitlePixelShader> CrashOfTheTitansPixelShaders()
{
    return kCrashOfTheTitansPixelShaders;
}
