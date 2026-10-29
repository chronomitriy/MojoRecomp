// Manual microbenchmark (not a CTest): cost per draw of the presenter's shader
// module and pipeline lookups, linear scan versus the hash index. The records
// mirror the field layout of ShaderModuleRec and PipelineRec in
// gpu/vk_presenter.cpp so the scan touches the same amount of memory. Every
// value is synthetic; no game data is involved.
//
// Usage: MojoRecompDrawLookupBenchmark [modules] [pipelines]

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <random>
#include <unordered_map>
#include <vector>

namespace {

struct Attribute
{
    int32_t location = 0;
    uint32_t fetchSlot = 0;
    uint32_t format = 0;
    bool isSigned = false;
    bool isInteger = false;
    uint32_t strideDwords = 0;
    uint32_t offsetDwords = 0;
    bool indirect = false;
};

struct ModuleRec
{
    uint32_t type = 0;
    uint64_t hash = 0;
    void* module = nullptr;
    std::vector<Attribute> attributes;
    bool usesTextures = false;
    std::vector<uint32_t> textureSlots;
    std::vector<uint32_t> textureDimensions;
    std::vector<uint32_t> aluConsts;
    bool aluDynamic = false;
    bool usesAlu = true;
    uint32_t colorOutputMask = 0;
    bool writesDepth = false;
};

struct PipelineRec
{
    uint64_t vs = 0;
    uint64_t ps = 0;
    uint32_t prim = 0;
    uint32_t colorMask = 0;
    uint32_t blendControl0 = 0;
    uint32_t blendControl1 = 0;
    uint32_t depthControl = 0;
    uint32_t rasterState = 0;
    uint32_t alphaTest = 0;
    uint32_t mode = 0;
    bool primitiveRestart = false;
    uint32_t samples = 1;
    uint32_t depthFormat = 0;
    uint64_t layoutHash = 0;
    void* pipeline = nullptr;
};

bool SameKey(const PipelineRec& a, const PipelineRec& b)
{
    return a.vs == b.vs && a.ps == b.ps && a.prim == b.prim &&
           a.colorMask == b.colorMask && a.blendControl0 == b.blendControl0 &&
           a.blendControl1 == b.blendControl1 && a.depthControl == b.depthControl &&
           a.rasterState == b.rasterState && a.alphaTest == b.alphaTest &&
           a.mode == b.mode && a.primitiveRestart == b.primitiveRestart &&
           a.samples == b.samples && a.depthFormat == b.depthFormat &&
           a.layoutHash == b.layoutHash;
}

uint64_t KeyHash(const PipelineRec& p)
{
    uint64_t h = 0x9E3779B97F4A7C15ull;
    const auto mix = [&h](uint64_t value) {
        h ^= value + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    };
    mix(p.vs); mix(p.ps); mix(p.prim); mix(p.colorMask); mix(p.blendControl0);
    mix(p.blendControl1); mix(p.depthControl); mix(p.rasterState); mix(p.alphaTest);
    mix(p.mode); mix(p.primitiveRestart ? 1u : 0u); mix(p.samples); mix(p.depthFormat);
    mix(p.layoutHash);
    return h;
}

uint64_t AttributeLayoutHash(const ModuleRec& vs)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (const auto& a : vs.attributes)
    {
        const uint64_t words[] = {uint64_t(uint32_t(a.location)), a.fetchSlot, a.format,
                                  a.isSigned, a.isInteger, a.strideDwords, a.offsetDwords};
        for (uint64_t v : words)
            h = (h ^ v) * 0x100000001B3ull;
    }
    return h;
}

template <typename F>
double NsPerCall(uint64_t calls, F&& body)
{
    const auto start = std::chrono::steady_clock::now();
    body();
    const auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::nano>(end - start).count() / double(calls);
}

} // namespace

int main(int argc, char** argv)
{
    const size_t moduleCount = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 1024;
    const size_t pipelineCount = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 4096;
    constexpr uint64_t kDraws = 200000; // ~150 frames of 1300 draws

    std::mt19937_64 rng(0x4D6F6A6Full);
    std::deque<ModuleRec> modules;
    std::array<std::unordered_map<uint64_t, ModuleRec*>, 2> moduleIndex;
    for (size_t i = 0; i < moduleCount; ++i)
    {
        ModuleRec rec;
        rec.type = uint32_t(i & 1u);
        rec.hash = rng();
        rec.attributes.resize(1 + (i % 6));
        for (auto& a : rec.attributes)
            a = {int32_t(i % 8), uint32_t(i % 96), 37, false, false, 8, 0, false};
        rec.textureSlots.resize(i % 4);
        modules.push_back(std::move(rec));
        moduleIndex[modules.back().type][modules.back().hash] = &modules.back();
    }

    std::vector<PipelineRec> pipelines;
    std::unordered_multimap<uint64_t, size_t> pipelineIndex;
    for (size_t i = 0; i < pipelineCount; ++i)
    {
        PipelineRec rec;
        rec.vs = modules[(i * 2) % moduleCount].hash;
        rec.ps = modules[(i * 2 + 1) % moduleCount].hash;
        rec.prim = uint32_t(i % 5);
        rec.colorMask = 0xF;
        rec.blendControl0 = uint32_t(rng());
        rec.depthControl = uint32_t(rng());
        rec.layoutHash = AttributeLayoutHash(modules[(i * 2) % moduleCount]);
        pipelines.push_back(rec);
        pipelineIndex.emplace(KeyHash(rec), pipelines.size() - 1u);
    }

    // A frame's working set is a few hundred states spread over the whole
    // history; pipelines prewarmed from the manifest come first.
    std::vector<size_t> drawPipelines(kDraws);
    std::uniform_int_distribution<size_t> pick(0, pipelineCount - 1);
    std::vector<size_t> workingSet(400);
    for (auto& w : workingSet)
        w = pick(rng);
    for (auto& d : drawPipelines)
        d = workingSet[rng() % workingSet.size()];

    volatile uintptr_t sink = 0;
    const double moduleLinear = NsPerCall(kDraws * 2, [&] {
        for (uint64_t i = 0; i < kDraws; ++i)
        {
            const PipelineRec& want = pipelines[drawPipelines[i]];
            for (uint32_t type = 0; type < 2; ++type)
            {
                const uint64_t hash = type ? want.ps : want.vs;
                for (auto& m : modules)
                    if (m.type == type && m.hash == hash)
                    {
                        sink = sink + uintptr_t(&m);
                        break;
                    }
            }
        }
    });
    const double moduleIndexed = NsPerCall(kDraws * 2, [&] {
        for (uint64_t i = 0; i < kDraws; ++i)
        {
            const PipelineRec& want = pipelines[drawPipelines[i]];
            for (uint32_t type = 0; type < 2; ++type)
            {
                const auto found = moduleIndex[type].find(type ? want.ps : want.vs);
                if (found != moduleIndex[type].end())
                    sink = sink + uintptr_t(found->second);
            }
        }
    });
    const ModuleRec& sampleVs = modules[0];
    const double layoutHashNs = NsPerCall(kDraws, [&] {
        for (uint64_t i = 0; i < kDraws; ++i)
            sink = sink + AttributeLayoutHash(sampleVs);
    });
    const double pipelineLinear = NsPerCall(kDraws, [&] {
        for (uint64_t i = 0; i < kDraws; ++i)
        {
            const PipelineRec& want = pipelines[drawPipelines[i]];
            const auto found = std::find_if(pipelines.begin(), pipelines.end(),
                [&](const PipelineRec& p) { return SameKey(p, want); });
            sink = sink + uintptr_t(found - pipelines.begin());
        }
    });
    const double pipelineIndexed = NsPerCall(kDraws, [&] {
        for (uint64_t i = 0; i < kDraws; ++i)
        {
            const PipelineRec& want = pipelines[drawPipelines[i]];
            const auto range = pipelineIndex.equal_range(KeyHash(want));
            for (auto it = range.first; it != range.second; ++it)
                if (SameKey(pipelines[it->second], want))
                {
                    sink = sink + it->second;
                    break;
                }
        }
    });

    std::printf("modules=%zu pipelines=%zu draws=%llu\n", moduleCount, pipelineCount,
                static_cast<unsigned long long>(kDraws));
    std::printf("GetModule   linear %8.1f ns/lookup  indexed %6.1f ns/lookup\n",
                moduleLinear, moduleIndexed);
    std::printf("GetPipeline linear %8.1f ns/lookup  indexed %6.1f ns/lookup  "
                "(+ layout hash %.1f ns, now cached)\n",
                pipelineLinear, pipelineIndexed, layoutHashNs);
    const double perDrawBefore = 2.0 * moduleLinear + pipelineLinear + layoutHashNs;
    const double perDrawAfter = 2.0 * moduleIndexed + pipelineIndexed;
    std::printf("per draw: %.2f us -> %.2f us; at 1300 draws/frame %.2f ms -> %.2f ms\n",
                perDrawBefore / 1000.0, perDrawAfter / 1000.0,
                perDrawBefore * 1300.0 / 1.0e6, perDrawAfter * 1300.0 / 1.0e6);
    return sink == 0 ? 1 : 0;
}
