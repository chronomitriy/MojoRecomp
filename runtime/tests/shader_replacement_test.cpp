// Title pixel-shader replacement: the table, the interface checks, and the
// Texture0Passthrough program built from a real XenosRecomp translation.
//
// The input is original synthetic Xenos microcode, not game code. It mirrors the
// interface of Crash of the Titans' motion blur pass as the runtime's translator
// reports it (texture fetch constants {0,2,3}, input registers {0,2,4}, one
// colour export), so the replacement is built from exactly the emitter output it
// will meet in the game. When DXC is available the original and the replacement
// are compiled with the runtime's own arguments and their SPIR-V interfaces are
// compared.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "../gpu/shader_replacement.h"
#include "../gpu/shader_translator.h"

namespace {

using namespace mojorecomp::gpu;

int g_failures = 0;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

uint32_t Bits(std::initializer_list<uint32_t> bits)
{
    uint32_t mask = 0;
    for (uint32_t bit : bits)
        mask |= 1u << bit;
    return mask;
}

// Exec-end with one 2D texture fetch per (source, destination, constant), then
// oC0 = max(first destination, last destination). Big-endian, as PM4 sees it.
std::vector<uint32_t> SyntheticPixelShader(
    const std::vector<std::array<uint32_t, 3>>& fetches)
{
    const uint32_t count = uint32_t(fetches.size()) + 1u;
    uint32_t sequence = 0;
    for (uint32_t i = 0; i < fetches.size(); ++i)
        sequence |= 1u << (2u * i);
    std::vector<uint32_t> code = {1u | (count << 12) | (sequence << 16), 0x00002800u, 0};
    for (const auto& [source, destination, constant] : fetches)
    {
        code.push_back(1u | (source << 5) | (destination << 12) | (constant << 20) | (4u << 26));
        code.push_back(0x688u | (3u << 12) | (3u << 14) | (3u << 16) | (7u << 18) | (1u << 28));
        code.push_back(1u << 14);
    }
    const uint32_t first = fetches.front()[1];
    const uint32_t last = fetches.back()[1];
    code.push_back((1u << 15) | (15u << 16) | (50u << 26));
    code.push_back(0);
    code.push_back((2u << 24) | (3u << 30) | (first << 16) | (last << 8));
    for (auto& word : code)
        word = __builtin_bswap32(word);
    return code;
}

struct Translation
{
    ShaderTranslator::Result result;
    bool spirv = false;
    uint32_t sizeDwords = 0;
};

Translation Translate(const char* name, const std::vector<uint32_t>& code)
{
    Translation translation;
    std::string error;
    translation.spirv = ShaderTranslator::Translate(
        name, reinterpret_cast<const uint8_t*>(code.data()), code.size() * 4,
        translation.result, error);
    translation.sizeDwords = uint32_t(code.size());
    // Without DXC the HLSL and the sidecar are still produced; only the SPIR-V
    // step fails.
    if (!translation.spirv)
        std::printf("note: %s has no SPIR-V (%s)\n", name, error.c_str());
    return translation;
}

bool FakeCompiler(const std::string& hlsl, std::vector<uint8_t>& spirv, std::string& error)
{
    if (hlsl.empty())
    {
        error = "empty";
        return false;
    }
    const uint32_t words[5] = {0x07230203u, 0x00010000u, 0, 1, 0};
    spirv.assign(reinterpret_cast<const uint8_t*>(words),
                 reinterpret_cast<const uint8_t*>(words) + sizeof(words));
    return true;
}

bool FailingCompiler(const std::string&, std::vector<uint8_t>&, std::string& error)
{
    error = "synthetic compiler failure";
    return false;
}

bool RealCompiler(const std::string& hlsl, std::vector<uint8_t>& spirv, std::string& error)
{
    return ShaderTranslator::CompileHostHlsl(hlsl, false, spirv, error);
}

size_t Count(const std::string& text, const std::string& needle)
{
    size_t count = 0;
    for (size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + needle.size()))
        ++count;
    return count;
}

std::string Replace(std::string text, const std::string& from, const std::string& to)
{
    const size_t at = text.find(from);
    if (at != std::string::npos)
        text.replace(at, from.size(), to);
    return text;
}

struct SpirvInterface
{
    std::set<uint32_t> inputLocations;
    std::set<uint32_t> outputLocations;
    std::set<std::pair<uint32_t, uint32_t>> bindings;
    uint32_t pushConstantBlocks = 0;
};

SpirvInterface ReadInterface(const std::vector<uint8_t>& bytes)
{
    SpirvInterface result;
    std::vector<uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), words.size() * 4);
    std::map<uint32_t, uint32_t> location, set, binding;
    std::map<uint32_t, uint32_t> storage;
    for (size_t i = 5; i < words.size();)
    {
        const uint32_t count = words[i] >> 16;
        const uint32_t opcode = words[i] & 0xFFFFu;
        if (!count || i + count > words.size())
            break;
        if (opcode == 71 && count >= 4) // OpDecorate
        {
            const uint32_t target = words[i + 1];
            const uint32_t decoration = words[i + 2];
            if (decoration == 30) location[target] = words[i + 3];
            if (decoration == 34) set[target] = words[i + 3];
            if (decoration == 33) binding[target] = words[i + 3];
        }
        if (opcode == 59 && count >= 4) // OpVariable
            storage[words[i + 2]] = words[i + 3];
        i += count;
    }
    for (const auto& [id, cls] : storage)
    {
        if (cls == 1 && location.count(id)) result.inputLocations.insert(location[id]);
        if (cls == 3 && location.count(id)) result.outputLocations.insert(location[id]);
        if (cls == 9) ++result.pushConstantBlocks;
        if (set.count(id) && binding.count(id))
            result.bindings.insert({set[id], binding[id]});
    }
    return result;
}

bool IsSubset(const std::set<uint32_t>& a, const std::set<uint32_t>& b)
{
    for (uint32_t value : a)
        if (!b.count(value))
            return false;
    return true;
}

} // namespace

int main()
{
    // The motion-blur-shaped synthetic shader.
    const auto blurCode = SyntheticPixelShader({{0, 1, 0}, {2, 3, 2}, {4, 5, 3}});
    const Translation blur = Translate("ps_0000000000001234", blurCode);
    Check(!blur.result.hlsl.empty(), "synthetic shader translated to HLSL");
    Check(blur.result.metaJson.find("\"interpolators\"") != std::string::npos,
          "sidecar lists the input registers");

    constexpr uint64_t kHash = 0x1234;
    TitlePixelShader entry{};
    entry.hash = kHash;
    entry.name = "synthetic-blur";
    entry.role = PixelShaderRole::MotionBlur;
    entry.replacement = PixelShaderReplacement::Texture0Passthrough;
    entry.sizeDwords = blur.sizeDwords;
    entry.textureSlotMask = Bits({0, 2, 3});
    entry.interpolatorMask = Bits({0, 2, 4});
    TitlePixelShader observed{};
    observed.hash = 0x5678;
    observed.name = "synthetic-dof";
    observed.role = PixelShaderRole::DepthOfField;
    observed.replacement = PixelShaderReplacement::ObserveOnly;
    const TitlePixelShader table[] = {entry, observed};

    std::string detail;
    auto apply = [&](uint64_t hash, uint32_t type, uint32_t size, std::string& hlsl,
                     std::vector<uint8_t>& spirv, HostShaderCompiler compiler) {
        return ApplyTitlePixelShaderReplacement(type, hash, size, hlsl, spirv,
                                                blur.result.metaJson, compiler, detail);
    };

    // Nothing configured: nothing is listed.
    ConfigureTitlePixelShaders({}, {});
    {
        std::string hlsl = blur.result.hlsl;
        std::vector<uint8_t> spirv = blur.result.spirv;
        Check(apply(kHash, 1, blur.sizeDwords, hlsl, spirv, FakeCompiler) ==
                  ShaderReplacementResult::NotListed,
              "unconfigured table lists nothing");
        Check(hlsl == blur.result.hlsl, "unconfigured table changed the shader");
    }

    // Motion blur on: the original is kept.
    ConfigureTitlePixelShaders(table, {true, true});
    Check(TitlePixelShaderRole(kHash) == PixelShaderRole::MotionBlur, "role lookup");
    Check(TitlePixelShaderRole(0x9999) == PixelShaderRole::None, "unknown hash has no role");
    Check(!TitlePixelShaderReplacementRequested(kHash), "feature on requests no replacement");
    {
        std::string hlsl = blur.result.hlsl;
        std::vector<uint8_t> spirv = blur.result.spirv;
        Check(apply(kHash, 1, blur.sizeDwords, hlsl, spirv, FakeCompiler) ==
                  ShaderReplacementResult::FeatureEnabled,
              "feature on keeps the original");
        Check(hlsl == blur.result.hlsl && spirv == blur.result.spirv,
              "feature on changed the shader");
        Check(apply(kHash, 0, blur.sizeDwords, hlsl, spirv, FakeCompiler) ==
                  ShaderReplacementResult::NotListed,
              "a vertex shader with a listed hash is not a pixel shader");
    }

    // Motion blur off: replaced, keeping the original prologue and signature.
    ConfigureTitlePixelShaders(table, {false, true});
    Check(TitlePixelShaderReplacementRequested(kHash), "feature off requests a replacement");
    Check(!TitlePixelShaderReplacementRequested(0x5678), "observe-only entry is never replaced");
    {
        std::string hlsl = blur.result.hlsl;
        std::vector<uint8_t> spirv = blur.result.spirv;
        const auto result = apply(kHash, 1, blur.sizeDwords, hlsl, spirv, FakeCompiler);
        Check(result == ShaderReplacementResult::Replaced, detail.c_str());
        Check(ShaderReplacementApplied(kHash), "applied flag set");
        const size_t entryAt = blur.result.hlsl.find("\nvoid main(\n");
        const size_t bodyAt = blur.result.hlsl.find(")\n{\n", entryAt);
        Check(entryAt != std::string::npos && bodyAt != std::string::npos, "original layout");
        Check(hlsl.compare(0, bodyAt + 4, blur.result.hlsl, 0, bodyAt + 4) == 0,
              "prologue and signature are byte-identical");
        Check(Count(hlsl, "tfetch2D(") == Count(blur.result.hlsl.substr(0, entryAt), "tfetch2D(") + 1,
              "replacement body has exactly one fetch");
        Check(hlsl.find("tfetch2D(s0_Texture2DDescriptorIndex, s0_SamplerDescriptorIndex, r0.xy") !=
                  std::string::npos,
              "replacement fetches s0 at r0.xy");
        Check(hlsl.find("float4 r0 = iTexCoord0;", bodyAt) != std::string::npos,
              "replacement reads TEXCOORD0");
        Check(hlsl.find("tfetch2D(s2_", bodyAt) == std::string::npos &&
                  hlsl.find("tfetch2D(s3_", bodyAt) == std::string::npos,
              "replacement drops the other fetches");
        Check(hlsl.find("* g_TextureSampleScale(0), 1.0);") != std::string::npos,
              "replacement scales like the original fetch and writes alpha 1");
        Check(hlsl.find("SPEC_CONSTANT_ALPHA_TEST)", bodyAt) != std::string::npos,
              "replacement keeps the alpha test");
        Check(hlsl.find("oDepth = MOJORECOMP_DEPTH_OUTPUT(iPos.z);", bodyAt) != std::string::npos,
              "replacement keeps the float24 depth prologue");
        Check(Count(hlsl, "SV_Target") == Count(blur.result.hlsl, "SV_Target") &&
                  Count(hlsl, "SV_Depth") == Count(blur.result.hlsl, "SV_Depth"),
              "presenter output detection sees the same exports");
    }
    {
        std::string hlsl = blur.result.hlsl;
        std::vector<uint8_t> spirv = blur.result.spirv;
        Check(apply(kHash, 1, blur.sizeDwords, hlsl, spirv, FailingCompiler) ==
                  ShaderReplacementResult::Rejected,
              "compile failure rejects");
        Check(hlsl == blur.result.hlsl && spirv == blur.result.spirv,
              "compile failure kept the original");
    }

    // Interface mismatches keep the original.
    {
        TitlePixelShader wrong = entry;
        wrong.sizeDwords = 165;
        std::string reason;
        Check(!CheckTitlePixelShaderInterface(wrong, blur.sizeDwords, blur.result.metaJson, reason),
              "size mismatch rejected");
        Check(CheckTitlePixelShaderInterface(wrong, 0, blur.result.metaJson, reason),
              "unknown size skips the size check");
        wrong = entry;
        wrong.textureSlotMask = Bits({0, 1});
        Check(!CheckTitlePixelShaderInterface(wrong, blur.sizeDwords, blur.result.metaJson, reason),
              "texture slot mismatch rejected");
        wrong = entry;
        wrong.interpolatorMask = Bits({0, 1});
        Check(!CheckTitlePixelShaderInterface(wrong, blur.sizeDwords, blur.result.metaJson, reason),
              "input register mismatch rejected");
        Check(!CheckTitlePixelShaderInterface(entry, blur.sizeDwords, "{}", reason),
              "unparsable metadata rejected");
        Check(CheckTitlePixelShaderInterface(entry, blur.sizeDwords, blur.result.metaJson, reason),
              reason.c_str());

        const TitlePixelShader mismatched[] = {wrong};
        ConfigureTitlePixelShaders(mismatched, {false, true});
        std::string hlsl = blur.result.hlsl;
        std::vector<uint8_t> spirv = blur.result.spirv;
        Check(apply(kHash, 1, blur.sizeDwords, hlsl, spirv, FakeCompiler) ==
                  ShaderReplacementResult::Rejected,
              "mismatched entry rejected");
        Check(hlsl == blur.result.hlsl, "mismatched entry kept the original");
        Check(!ShaderReplacementApplied(kHash), "rejected replacement is not reported applied");
    }

    // Observe-only entries report the interface.
    ConfigureTitlePixelShaders(table, {false, false});
    {
        std::string hlsl = blur.result.hlsl;
        std::vector<uint8_t> spirv = blur.result.spirv;
        Check(apply(0x5678, 1, 504, hlsl, spirv, FakeCompiler) == ShaderReplacementResult::Observed,
              "observe-only entry observed");
        Check(detail.find("tfetch={0,2,3}") != std::string::npos &&
                  detail.find("inputs={0,2,4}") != std::string::npos,
              detail.c_str());
        Check(hlsl == blur.result.hlsl, "observe-only entry changed the shader");
    }

    // The builder refuses anything but the expected emitter layout.
    {
        std::string out, reason;
        const std::string& source = blur.result.hlsl;
        Check(BuildTexture0PassthroughHlsl(source, out, reason), reason.c_str());
        const std::pair<std::string, std::string> mutations[] = {
            {"\tin float4 iTexCoord0 : TEXCOORD0,\n", ""},
            {"\tout float4 oC0 : SV_Target0", "\tout float4 oC0 : SV_Target0,\n\tout float4 oC1 : SV_Target1"},
            {"\n#ifdef MOJORECOMP_FLOAT24_DEPTH\n,\n\tout float oDepth : SV_Depth\n#endif\n",
             ",\n\tout float oDepth : SV_Depth\n"},
            {"\tfloat4 r0 = iTexCoord0;\n", "\tfloat4 r0 = iTexCoord1;\n"},
            {"#define s0_Texture2DDescriptorIndex ", "#define sX_Texture2DDescriptorIndex "},
            {"tfetch2D(s0_Texture2DDescriptorIndex, s0_SamplerDescriptorIndex, ",
             "tfetch3D(s0_Texture3DDescriptorIndex, s0_SamplerDescriptorIndex, "},
            {"[shader(\"pixel\")]", "[shader(\"vertex\")]"},
            {"\nvoid main(\n", "\nvoid main_(\n"},
        };
        for (const auto& [from, to] : mutations)
        {
            const std::string mutated = Replace(source, from, to);
            Check(mutated != source, "mutation applies to the emitter output");
            Check(!BuildTexture0PassthroughHlsl(mutated, out, reason),
                  ("mutated HLSL accepted: " + from).c_str());
        }
        Check(!BuildTexture0PassthroughHlsl(source + "\nvoid main(\n)\n{\n}", out, reason),
              "two entry points accepted");

        // A real translation without texture constant 0 (fetch from s3 only).
        const Translation noS0 = Translate("ps_0000000000000003",
                                           SyntheticPixelShader({{0, 1, 3}}));
        Check(!BuildTexture0PassthroughHlsl(noS0.result.hlsl, out, reason),
              "shader without s0 accepted");
        // The builder checks structure only: here s0 is sampled at r2, yet r0 is
        // still TEXCOORD0 and the build succeeds. Which register addresses s0 in
        // a given shader is title knowledge, pinned by the table entry's hash
        // and interface.
        const Translation otherCoord = Translate("ps_0000000000000004",
                                                 SyntheticPixelShader({{2, 1, 0}, {0, 3, 2}}));
        Check(BuildTexture0PassthroughHlsl(otherCoord.result.hlsl, out, reason),
              "structurally valid shader rejected");
    }

    // With DXC: compile both programs, also as the float24 depth variant, and
    // compare their interfaces.
    if (blur.spirv)
    {
        ConfigureTitlePixelShaders(table, {false, true});
        std::string hlsl = blur.result.hlsl;
        std::vector<uint8_t> spirv = blur.result.spirv;
        const auto result = apply(kHash, 1, blur.sizeDwords, hlsl, spirv, RealCompiler);
        Check(result == ShaderReplacementResult::Replaced, detail.c_str());
        const SpirvInterface original = ReadInterface(blur.result.spirv);
        const SpirvInterface replacement = ReadInterface(spirv);
        Check(original.inputLocations.count(0) && replacement.inputLocations.count(0),
              "TEXCOORD0 is input location 0 in both programs");
        Check(IsSubset(replacement.inputLocations, original.inputLocations),
              "replacement reads no input the original does not declare");
        Check(replacement.outputLocations == original.outputLocations,
              "replacement writes the same colour outputs");
        for (const auto& bindingPair : replacement.bindings)
            Check(original.bindings.count(bindingPair) != 0,
                  "replacement binds only descriptors the original binds");
        Check(replacement.pushConstantBlocks == 1 && original.pushConstantBlocks == 1,
              "both programs use the shared push-constant block");
        std::printf("SPIR-V: original %zu bytes, inputs %zu, bindings %zu; replacement %zu bytes, "
                    "inputs %zu, bindings %zu\n",
                    blur.result.spirv.size(), original.inputLocations.size(),
                    original.bindings.size(), spirv.size(), replacement.inputLocations.size(),
                    replacement.bindings.size());

        std::vector<uint8_t> depthVariant;
        std::string error;
        Check(ShaderTranslator::CompileHostHlsl("#define MOJORECOMP_FLOAT24_DEPTH 1\n" + hlsl,
                                                false, depthVariant, error),
              ("float24 depth variant of the replacement: " + error).c_str());
    }
    else
        std::puts("note: DXC not found; SPIR-V interface checks skipped");

    if (g_failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("OK: title pixel-shader replacement table, interface checks and passthrough builder.");
    return 0;
}
