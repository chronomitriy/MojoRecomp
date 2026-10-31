#include "shader_replacement.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "shader_metadata.h"

namespace mojorecomp::gpu {
namespace {

constexpr size_t kMaxTitlePixelShaders = 16;

struct ConfiguredPixelShader
{
    TitlePixelShader entry{};
    bool requested = false;
    std::atomic<bool> applied{false};
};

std::array<ConfiguredPixelShader, kMaxTitlePixelShaders> g_titlePixelShaders;
std::atomic<size_t> g_titlePixelShaderCount{0};

ConfiguredPixelShader* FindConfigured(uint64_t hash)
{
    const size_t count = g_titlePixelShaderCount.load(std::memory_order_acquire);
    for (size_t i = 0; i < count; ++i)
        if (g_titlePixelShaders[i].entry.hash == hash)
            return &g_titlePixelShaders[i];
    return nullptr;
}

bool FeatureEnabled(PixelShaderRole role, PixelShaderFeatures features)
{
    switch (role)
    {
        case PixelShaderRole::MotionBlur: return features.motionBlur;
        case PixelShaderRole::DepthOfField: return features.depthOfField;
        default: return true;
    }
}

size_t CountOccurrences(std::string_view text, std::string_view needle)
{
    size_t count = 0;
    for (size_t at = text.find(needle); at != std::string_view::npos;
         at = text.find(needle, at + needle.size()))
        ++count;
    return count;
}

bool Contains(std::string_view text, std::string_view needle)
{
    return text.find(needle) != std::string_view::npos;
}

uint32_t SlotMask(const std::vector<uint32_t>& slots)
{
    uint32_t mask = 0;
    for (uint32_t slot : slots)
        if (slot < 32)
            mask |= 1u << slot;
    return mask;
}

std::string FormatSlots(const std::vector<uint32_t>& slots)
{
    std::string text = "{";
    for (size_t i = 0; i < slots.size(); ++i)
    {
        if (i)
            text += ',';
        text += std::to_string(slots[i]);
    }
    return text + "}";
}

std::string FormatMask(uint32_t mask)
{
    std::vector<uint32_t> slots;
    for (uint32_t bit = 0; bit < 32; ++bit)
        if (mask & (1u << bit))
            slots.push_back(bit);
    return FormatSlots(slots);
}

// Exact spellings of XenosRecomp's pixel-shader emitter (shader_recompiler.cpp)
// as patched by patches/xenosrecomp-mojorecomp-runtime.patch.
constexpr std::string_view kPixelStage = "[shader(\"pixel\")]";
constexpr std::string_view kEntryPoint = "\nvoid main(\n";
constexpr std::string_view kBodyOpen = ")\n{\n";
constexpr std::string_view kTexcoord0Input = "\tin float4 iTexCoord0 : TEXCOORD0,\n";
constexpr std::string_view kColor0Output = "\tout float4 oC0 : SV_Target0";
constexpr std::string_view kOptionalDepthOutput =
    "\n#ifdef MOJORECOMP_FLOAT24_DEPTH\n,\n\tout float oDepth : SV_Depth\n#endif\n";
constexpr std::string_view kDepthPrologue =
    "#ifdef MOJORECOMP_FLOAT24_DEPTH\n\toDepth = MOJORECOMP_DEPTH_OUTPUT(iPos.z);\n#endif\n";
constexpr std::string_view kR0FromTexcoord0 = "\tfloat4 r0 = iTexCoord0;\n";
constexpr std::string_view kS0TextureMacro = "#define s0_Texture2DDescriptorIndex ";
constexpr std::string_view kS0SamplerMacro = "#define s0_SamplerDescriptorIndex ";
constexpr std::string_view kS0Fetch =
    "tfetch2D(s0_Texture2DDescriptorIndex, s0_SamplerDescriptorIndex, ";

// Same alpha test the emitter appends at every pixel-shader return, so a draw
// with alpha testing enabled behaves exactly as before for alpha 1.
constexpr std::string_view kTexture0PassthroughBody =
    "\t// MojoRecomp title shader replacement (Texture0Passthrough): one fetch of\n"
    "\t// the pass's source image at its own coordinate instead of the filter taps.\n"
    "\tfloat4 r0 = iTexCoord0;\n"
    "\toC0 = float4(tfetch2D(s0_Texture2DDescriptorIndex, s0_SamplerDescriptorIndex, "
    "r0.xy, float2(0, 0)).xyz * g_TextureSampleScale(0), 1.0);\n"
    "\t[branch] if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST)\n"
    "\t{\n"
    "\t\tuint alphaFunc = (g_SpecConstants() >> SPEC_CONSTANT_ALPHA_FUNC_SHIFT) & 7u;\n"
    "\t\tbool alphaPass = alphaFunc == 0u ? false :\n"
    "\t\t                 alphaFunc == 1u ? (oC0.w <  g_AlphaThreshold) :\n"
    "\t\t                 alphaFunc == 2u ? (oC0.w == g_AlphaThreshold) :\n"
    "\t\t                 alphaFunc == 3u ? (oC0.w <= g_AlphaThreshold) :\n"
    "\t\t                 alphaFunc == 4u ? (oC0.w >  g_AlphaThreshold) :\n"
    "\t\t                 alphaFunc == 5u ? (oC0.w != g_AlphaThreshold) :\n"
    "\t\t                 alphaFunc == 6u ? (oC0.w >= g_AlphaThreshold) : true;\n"
    "\t\tclip(alphaPass ? 1.0 : -1.0);\n"
    "\t}\n"
    "}\n";

} // namespace

void ConfigureTitlePixelShaders(std::span<const TitlePixelShader> table,
                                PixelShaderFeatures features)
{
    const size_t count = std::min(table.size(), kMaxTitlePixelShaders);
    g_titlePixelShaderCount.store(0, std::memory_order_release);
    for (size_t i = 0; i < count; ++i)
    {
        auto& configured = g_titlePixelShaders[i];
        configured.entry = table[i];
        configured.requested =
            table[i].replacement != PixelShaderReplacement::ObserveOnly &&
            !FeatureEnabled(table[i].role, features);
        configured.applied.store(false, std::memory_order_relaxed);
    }
    g_titlePixelShaderCount.store(count, std::memory_order_release);
}

const TitlePixelShader* FindTitlePixelShader(uint64_t hash)
{
    const ConfiguredPixelShader* configured = FindConfigured(hash);
    return configured ? &configured->entry : nullptr;
}

PixelShaderRole TitlePixelShaderRole(uint64_t hash)
{
    const ConfiguredPixelShader* configured = FindConfigured(hash);
    return configured ? configured->entry.role : PixelShaderRole::None;
}

bool TitlePixelShaderReplacementRequested(uint64_t hash)
{
    const ConfiguredPixelShader* configured = FindConfigured(hash);
    return configured && configured->requested;
}

bool ShaderReplacementApplied(uint64_t hash)
{
    const ConfiguredPixelShader* configured = FindConfigured(hash);
    return configured && configured->applied.load(std::memory_order_acquire);
}

const char* ToString(PixelShaderRole role)
{
    switch (role)
    {
        case PixelShaderRole::MotionBlur: return "motion-blur";
        case PixelShaderRole::DepthOfField: return "depth-of-field";
        default: return "none";
    }
}

bool BuildTexture0PassthroughHlsl(const std::string& translatedHlsl, std::string& out,
                                  std::string& reason)
{
    const std::string_view hlsl = translatedHlsl;
    const size_t entry = hlsl.find(kEntryPoint);
    if (entry == std::string_view::npos)
    {
        reason = "no XenosRecomp entry point";
        return false;
    }
    if (hlsl.find(kEntryPoint, entry + 1) != std::string_view::npos)
    {
        reason = "more than one entry point";
        return false;
    }
    const size_t bodyOpen = hlsl.find(kBodyOpen, entry);
    if (bodyOpen == std::string_view::npos)
    {
        reason = "entry point has no body";
        return false;
    }
    const std::string_view prologue = hlsl.substr(0, entry);
    const std::string_view signature = hlsl.substr(entry, bodyOpen - entry);
    const std::string_view body = hlsl.substr(bodyOpen + kBodyOpen.size());

    if (!Contains(prologue, kPixelStage))
        reason = "not a pixel shader";
    else if (!Contains(prologue, kS0TextureMacro) || !Contains(prologue, kS0SamplerMacro))
        reason = "no texture fetch constant 0";
    else if (!Contains(signature, kTexcoord0Input))
        reason = "no TEXCOORD0 input";
    else if (!Contains(signature, kColor0Output) || CountOccurrences(signature, "SV_Target") != 1)
        reason = "exports other than oC0";
    else if (CountOccurrences(signature, "SV_Depth") != 1 ||
             !Contains(signature, kOptionalDepthOutput))
        reason = "writes depth";
    else if (body.substr(0, kDepthPrologue.size()) != kDepthPrologue)
        reason = "unexpected body prologue";
    else if (!Contains(body, kR0FromTexcoord0))
        reason = "r0 is not read from TEXCOORD0";
    else if (!Contains(body, kS0Fetch))
        reason = "s0 is not sampled as a 2D texture";
    else if (!Contains(body, "oC0"))
        reason = "oC0 is never written";
    else
        reason.clear();
    if (!reason.empty())
        return false;

    std::string result;
    result.reserve(entry + signature.size() + kBodyOpen.size() + kDepthPrologue.size() +
                   kTexture0PassthroughBody.size());
    result.append(prologue);
    result.append(signature);
    result.append(kBodyOpen);
    result.append(kDepthPrologue);
    result.append(kTexture0PassthroughBody);
    out = std::move(result);
    return true;
}

bool CheckTitlePixelShaderInterface(const TitlePixelShader& entry, uint32_t sizeDwords,
                                    const std::string& metaJson, std::string& reason)
{
    ShaderMetadata metadata{};
    if (!ParseShaderMetadata(metaJson, 1, metadata))
    {
        reason = "pixel shader metadata did not parse";
        return false;
    }
    if (entry.sizeDwords && sizeDwords && sizeDwords != entry.sizeDwords)
    {
        reason = "microcode is " + std::to_string(sizeDwords) + " dwords, expected " +
                 std::to_string(entry.sizeDwords);
        return false;
    }
    const uint32_t textureMask = SlotMask(metadata.textureSlots);
    if (entry.textureSlotMask && textureMask != entry.textureSlotMask)
    {
        reason = "texture fetch constants " + FormatSlots(metadata.textureSlots) +
                 ", expected " + FormatMask(entry.textureSlotMask);
        return false;
    }
    const uint32_t inputMask = SlotMask(metadata.interpolators);
    if (entry.interpolatorMask && inputMask != entry.interpolatorMask)
    {
        reason = "input registers " + FormatSlots(metadata.interpolators) + ", expected " +
                 FormatMask(entry.interpolatorMask);
        return false;
    }
    if (entry.replacement == PixelShaderReplacement::Texture0Passthrough)
    {
        bool texture0Is2D = false;
        for (size_t i = 0; i < metadata.textureSlots.size(); ++i)
            if (metadata.textureSlots[i] == 0 && metadata.textureDimensions[i] == 1)
                texture0Is2D = true;
        if (!texture0Is2D)
        {
            reason = "texture fetch constant 0 is not a 2D fetch";
            return false;
        }
        if (!(inputMask & 1u))
        {
            reason = "input register 0 is not interpolated";
            return false;
        }
    }
    reason.clear();
    return true;
}

ShaderReplacementResult ApplyTitlePixelShaderReplacement(
    uint32_t type, uint64_t hash, uint32_t sizeDwords, std::string& hlsl,
    std::vector<uint8_t>& spirv, const std::string& metaJson,
    HostShaderCompiler compile, std::string& detail)
{
    detail.clear();
    if (type != 1)
        return ShaderReplacementResult::NotListed;
    ConfiguredPixelShader* configured = FindConfigured(hash);
    if (!configured)
        return ShaderReplacementResult::NotListed;
    const TitlePixelShader& entry = configured->entry;

    if (entry.replacement == PixelShaderReplacement::ObserveOnly)
    {
        ShaderMetadata metadata{};
        if (ParseShaderMetadata(metaJson, 1, metadata))
            detail = "tfetch=" + FormatSlots(metadata.textureSlots) +
                     " inputs=" + FormatSlots(metadata.interpolators) +
                     " aluConsts=" + std::to_string(metadata.aluConsts.size()) +
                     (metadata.aluDynamic ? "+dynamic" : "") +
                     " dwords=" + std::to_string(sizeDwords);
        else
            detail = "metadata did not parse";
        return ShaderReplacementResult::Observed;
    }
    if (!configured->requested)
        return ShaderReplacementResult::FeatureEnabled;

    if (!CheckTitlePixelShaderInterface(entry, sizeDwords, metaJson, detail))
        return ShaderReplacementResult::Rejected;
    std::string replacementHlsl;
    if (!BuildTexture0PassthroughHlsl(hlsl, replacementHlsl, detail))
        return ShaderReplacementResult::Rejected;

    std::vector<uint8_t> replacementSpirv;
    std::string error;
    if (!compile || !compile(replacementHlsl, replacementSpirv, error))
    {
        detail = "compile failed: " + error;
        return ShaderReplacementResult::Rejected;
    }
    uint32_t magic = 0;
    if (replacementSpirv.size() < sizeof(magic) || (replacementSpirv.size() & 3u) != 0)
    {
        detail = "compiler returned no SPIR-V";
        return ShaderReplacementResult::Rejected;
    }
    std::memcpy(&magic, replacementSpirv.data(), sizeof(magic));
    if (magic != 0x07230203u)
    {
        detail = "compiler returned no SPIR-V";
        return ShaderReplacementResult::Rejected;
    }

    hlsl = std::move(replacementHlsl);
    spirv = std::move(replacementSpirv);
    configured->applied.store(true, std::memory_order_release);
    detail = "texture0 passthrough, " + std::to_string(spirv.size()) + "-byte SPIR-V";
    return ShaderReplacementResult::Replaced;
}

} // namespace mojorecomp::gpu
