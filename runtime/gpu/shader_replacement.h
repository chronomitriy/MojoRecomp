#pragma once

// Title pixel shaders the runtime knows by hash, and optional host replacements
// for them. A title supplies its own table at startup (see
// title_pixel_shaders.cpp); no title inherits another's entries. A replacement
// is only ever built from the real translation of the listed shader and only
// after its interface matches what the table expects, so a hash that is absent,
// a translator change or an unexpected interface leaves the original shader in
// place.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace mojorecomp::gpu {

enum class PixelShaderRole : uint8_t
{
    None,
    MotionBlur,
    DepthOfField,
};

enum class PixelShaderReplacement : uint8_t
{
    // Never replaced. The runtime only reports the shader's interface and draws.
    ObserveOnly,
    // One fetch from texture constant 0 at TEXCOORD0, scaled like the original
    // fetches, written to oC0 with alpha 1. Meant for a full-screen pass that
    // filters its source image: the result is the unfiltered source.
    Texture0Passthrough,
};

struct TitlePixelShader
{
    // FNV-1a over the guest microcode, as PM4 hashes it at IM_LOAD.
    uint64_t hash = 0;
    const char* name = "";
    // The title asset the microcode was identified in (documentation only).
    const char* source = "";
    PixelShaderRole role = PixelShaderRole::None;
    PixelShaderReplacement replacement = PixelShaderReplacement::ObserveOnly;
    // Expected interface of the translation; 0 skips a check.
    uint32_t sizeDwords = 0;
    // Bit n: texture fetch constant n.
    uint32_t textureSlotMask = 0;
    // Bit n: input register n (read through TEXCOORD<n>).
    uint32_t interpolatorMask = 0;
};

struct PixelShaderFeatures
{
    bool motionBlur = true;
    bool depthOfField = true;
};

// Called once at startup, before PM4 binds the first shader. Without this call
// no shader is listed and nothing is replaced.
void ConfigureTitlePixelShaders(std::span<const TitlePixelShader> table,
                                PixelShaderFeatures features);

const TitlePixelShader* FindTitlePixelShader(uint64_t hash);
PixelShaderRole TitlePixelShaderRole(uint64_t hash);
// The listed shader's feature is turned off and it has a replacement. This is
// the request; whether the replacement was built is ShaderReplacementApplied.
bool TitlePixelShaderReplacementRequested(uint64_t hash);
bool ShaderReplacementApplied(uint64_t hash);
const char* ToString(PixelShaderRole role);

// Builds the Texture0Passthrough program from XenosRecomp's translation of the
// original shader. The prologue (shared header, constant and sampler macros) and
// the entry-point signature are kept byte for byte, so the replacement has the
// same push constants, descriptor heaps, input locations and outputs. Fails,
// with a reason, unless the translation reads TEXCOORD0 into r0, samples s0 as a
// 2D texture and exports only oC0 (plus the optional float24 depth variant).
bool BuildTexture0PassthroughHlsl(const std::string& translatedHlsl, std::string& out,
                                  std::string& reason);

// Compares a translation's sidecar metadata (and its microcode size, when
// known) with the table entry.
bool CheckTitlePixelShaderInterface(const TitlePixelShader& entry, uint32_t sizeDwords,
                                    const std::string& metaJson, std::string& reason);

enum class ShaderReplacementResult
{
    NotListed,
    // Listed, observe-only.
    Observed,
    // Listed with a replacement, but its feature is on.
    FeatureEnabled,
    Replaced,
    // Requested, but the interface did not match or compilation failed; the
    // original stays.
    Rejected,
};

using HostShaderCompiler = bool (*)(const std::string& hlsl, std::vector<uint8_t>& spirv,
                                    std::string& error);

// The shader cache calls this for every new translation, after its disk cache
// has stored the original. On Replaced, `hlsl` and `spirv` hold the replacement
// and the metadata stays the original's, so descriptor and constant uploads do
// not change. `sizeDwords` may be 0 when the size is not known.
ShaderReplacementResult ApplyTitlePixelShaderReplacement(
    uint32_t type, uint64_t hash, uint32_t sizeDwords, std::string& hlsl,
    std::vector<uint8_t>& spirv, const std::string& metaJson,
    HostShaderCompiler compile, std::string& detail);

} // namespace mojorecomp::gpu
