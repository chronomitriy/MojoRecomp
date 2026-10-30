// Differential tests of the Vulkan thread's vertex-path helpers
// (docs/HFR-PHASE3.md section 7):
//   - gpu/index_scan.h against the per-index code in gpu/vk_presenter.cpp
//     (CopySwapped, ReadIndexValue and the loops of PrepareVertexBindings,
//     copied verbatim below). For every endian mode, both index widths,
//     primitive restart, index offsets and vertex clamps it checks the bounds
//     PrepareVertexBindings derives and the host index buffer it writes. Each
//     source buffer ends at an inaccessible page, so a read past the guest's
//     index data faults.
//   - gpu/frame_hash_map.h against std::unordered_map, over random finds,
//     emplaces and per-frame clears.
// Synthetic data only. --bench prints the time of both index forms.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "../gpu/frame_hash_map.h"
#include "../gpu/index_scan.h"
#include "../gpu/xenos.h"

namespace {

// --- Verbatim from gpu/vk_presenter.cpp. ----------------------------------
void CopySwapped(uint8_t* dst, const uint8_t* src, size_t bytes, uint32_t endian)
{
    switch (endian & 3u)
    {
        case 0:
            std::memcpy(dst, src, bytes);
            break;
        case 1:
        {
            size_t i = 0;
#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE2__)
            for (; i + 15 < bytes; i += 16)
            {
                const __m128i value = _mm_loadu_si128(
                    reinterpret_cast<const __m128i*>(src + i));
                const __m128i swapped = _mm_or_si128(
                    _mm_slli_epi16(value, 8), _mm_srli_epi16(value, 8));
                _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), swapped);
            }
#endif
            for (; i + 3 < bytes; i += 4)
            {
                uint32_t v;
                std::memcpy(&v, src + i, 4);
                v = ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
                std::memcpy(dst + i, &v, 4);
            }
            break;
        }
        case 2:
        {
            size_t i = 0;
#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE2__)
            for (; i + 15 < bytes; i += 16)
            {
                const __m128i value = _mm_loadu_si128(
                    reinterpret_cast<const __m128i*>(src + i));
                const __m128i byteSwapped16 = _mm_or_si128(
                    _mm_slli_epi16(value, 8), _mm_srli_epi16(value, 8));
                const __m128i swapped = _mm_or_si128(
                    _mm_slli_epi32(byteSwapped16, 16),
                    _mm_srli_epi32(byteSwapped16, 16));
                _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), swapped);
            }
#endif
            for (; i + 3 < bytes; i += 4)
            {
                uint32_t v;
                std::memcpy(&v, src + i, 4);
                v = __builtin_bswap32(v);
                std::memcpy(dst + i, &v, 4);
            }
            break;
        }
        case 3:
        {
            size_t i = 0;
#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE2__)
            for (; i + 15 < bytes; i += 16)
            {
                const __m128i value = _mm_loadu_si128(
                    reinterpret_cast<const __m128i*>(src + i));
                const __m128i swapped = _mm_or_si128(
                    _mm_slli_epi32(value, 16), _mm_srli_epi32(value, 16));
                _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), swapped);
            }
#endif
            for (; i + 3 < bytes; i += 4)
            {
                uint32_t v;
                std::memcpy(&v, src + i, 4);
                v = (v >> 16) | (v << 16);
                std::memcpy(dst + i, &v, 4);
            }
            break;
        }
    }
}

uint32_t ReadIndexValue(const uint8_t* src, uint32_t index, bool index32, uint32_t endian)
{
    if (index32)
    {
        uint32_t value = 0;
        std::memcpy(&value, src + uint64_t(index) * 4u, sizeof(value));
        uint8_t raw[4];
        std::memcpy(raw, &value, sizeof(raw));
        CopySwapped(reinterpret_cast<uint8_t*>(&value), raw, sizeof(raw), endian);
        return value;
    }

    if ((endian & 3u) == 2u)
    {
        uint32_t pair = 0;
        std::memcpy(&pair, src + uint64_t(index & ~1u) * 2u, sizeof(pair));
        pair = __builtin_bswap32(pair);
        return (index & 1u) ? (pair >> 16) : (pair & 0xFFFFu);
    }

    uint16_t value = 0;
    std::memcpy(&value, src + uint64_t(index) * 2u, sizeof(value));
    if ((endian & 3u) == 1u || (endian & 3u) == 3u)
        value = uint16_t((value >> 8) | (value << 8));
    return value;
}
// --- End of the verbatim copy. ----------------------------------------------

struct Case
{
    bool index32 = false;
    uint32_t endian = 0;
    uint32_t count = 0;
    bool restart = false;
    uint32_t restartIndex = 0;
    uint32_t indexOffset = 0;
    uint32_t minVertex = 0;
    uint32_t maxVertex = 0;
};

struct Bounds
{
    uint32_t maxIndex = 0;
    uint32_t maxEffective = 0;
    bool remap = false;
};

// The scan loop of PrepareVertexBindings for an indexed draw.
Bounds ReferenceBounds(const uint8_t* src, const Case& c)
{
    Bounds out;
    for (uint32_t i = 0; i < c.count; ++i)
    {
        const uint32_t raw = ReadIndexValue(src, i, c.index32, c.endian);
        if (c.restart && raw == c.restartIndex)
            continue;
        const uint32_t effective = xenos::RemapVertexIndex(raw, c.indexOffset,
                                                           c.minVertex, c.maxVertex);
        out.maxIndex = std::max(out.maxIndex, raw);
        out.maxEffective = std::max(out.maxEffective, effective);
        out.remap |= effective != raw;
    }
    return out;
}

// The host index buffer PrepareVertexBindings writes for an indexed draw.
std::vector<uint8_t> ReferenceIndices(const uint8_t* src, const Case& c, bool rewritten)
{
    std::vector<uint8_t> dst(size_t(c.count) * ((rewritten || c.index32) ? 4u : 2u));
    if (rewritten)
    {
        for (uint32_t i = 0; i < c.count; ++i)
        {
            const uint32_t raw = ReadIndexValue(src, i, c.index32, c.endian);
            const uint32_t value = c.restart && raw == c.restartIndex
                                       ? 0xFFFFFFFFu
                                       : xenos::RemapVertexIndex(raw, c.indexOffset,
                                                                 c.minVertex, c.maxVertex);
            std::memcpy(dst.data() + uint64_t(i) * 4u, &value, sizeof(value));
        }
    }
    else if (c.index32)
    {
        for (uint32_t i = 0; i < c.count; ++i)
        {
            const uint32_t value = ReadIndexValue(src, i, true, c.endian);
            std::memcpy(dst.data() + uint64_t(i) * 4u, &value, sizeof(value));
        }
    }
    else
    {
        for (uint32_t i = 0; i < c.count; ++i)
        {
            const uint16_t value = uint16_t(ReadIndexValue(src, i, false, c.endian));
            std::memcpy(dst.data() + uint64_t(i) * 2u, &value, sizeof(value));
        }
    }
    return dst;
}

// The fast forms, as PrepareVertexBindings now calls them.
std::vector<uint8_t> FastIndices(const uint8_t* src, const Case& c, bool rewritten)
{
    const size_t width = (rewritten || c.index32) ? 4u : 2u;
    // Guard bytes after the buffer catch a write past its end.
    std::vector<uint8_t> dst(size_t(c.count) * width + 64u, 0xA5);
    mojorecomp::gpu::DecodeGuestIndices(dst.data(), src, c.count, c.index32, c.endian,
                                        rewritten || c.index32);
    if (rewritten)
    {
        auto* values = reinterpret_cast<uint32_t*>(dst.data());
        for (uint32_t i = 0; i < c.count; ++i)
        {
            const uint32_t raw = values[i];
            values[i] = c.restart && raw == c.restartIndex
                            ? 0xFFFFFFFFu
                            : xenos::RemapVertexIndex(raw, c.indexOffset,
                                                      c.minVertex, c.maxVertex);
        }
    }
    for (size_t i = size_t(c.count) * width; i < dst.size(); ++i)
        if (dst[i] != 0xA5)
            return {};
    dst.resize(size_t(c.count) * width);
    return dst;
}

// Index data that ends exactly where an inaccessible page begins.
class GuardedBuffer
{
public:
    GuardedBuffer()
    {
#if defined(_WIN32)
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        page_ = info.dwPageSize;
        bytes_ = page_ * 64u;
        base_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, bytes_ + page_,
                                                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        DWORD old = 0;
        if (base_)
            VirtualProtect(base_ + bytes_, page_, PAGE_NOACCESS, &old);
#else
        page_ = static_cast<size_t>(sysconf(_SC_PAGESIZE));
        bytes_ = page_ * 64u;
        void* mapped = mmap(nullptr, bytes_ + page_, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        base_ = mapped == MAP_FAILED ? nullptr : static_cast<uint8_t*>(mapped);
        if (base_)
            mprotect(base_ + bytes_, page_, PROT_NONE);
#endif
    }

    bool Valid() const { return base_ != nullptr; }
    size_t Capacity() const { return bytes_; }

    // A buffer of `bytes` readable bytes followed by the guard page.
    uint8_t* Place(size_t bytes) { return base_ + bytes_ - bytes; }

private:
    uint8_t* base_ = nullptr;
    size_t page_ = 0;
    size_t bytes_ = 0;
};

int Fail(const char* message, const Case& c)
{
    std::fprintf(stderr,
                 "FAIL: %s (index32=%d endian=%u count=%u restart=%d/%08X offset=%u "
                 "clamp=%u..%u)\n",
                 message, c.index32 ? 1 : 0, c.endian, c.count, c.restart ? 1 : 0,
                 c.restartIndex, c.indexOffset, c.minVertex, c.maxVertex);
    return 1;
}

void FillIndices(uint8_t* src, size_t bytes, const Case& c, std::mt19937& random)
{
    // Mostly a mesh-like range, sometimes the whole width; 32-bit cases also
    // get values above the 24-bit vertex index mask.
    const uint32_t width = c.index32 ? 4u : 2u;
    const uint32_t mode = random() % 4u;
    const uint32_t span = mode == 0 ? 64u : mode == 1 ? 4096u : 0xFFFFFFFFu;
    const uint32_t start = random() % 3000u;
    for (size_t at = 0; at + width <= bytes; at += width)
    {
        uint32_t value = span == 0xFFFFFFFFu ? random() : start + random() % span;
        if (c.restart && random() % 8u == 0)
        {
            // Store the restart index so that it decodes to restartIndex.
            value = c.restartIndex;
            if (c.index32)
            {
                uint32_t encoded = value;
                uint8_t raw[4];
                std::memcpy(raw, &encoded, 4);
                CopySwapped(reinterpret_cast<uint8_t*>(&encoded), raw, 4, c.endian);
                std::memcpy(src + at, &encoded, 4);
                continue;
            }
        }
        if (c.index32)
            std::memcpy(src + at, &value, 4);
        else
        {
            const uint16_t narrow = static_cast<uint16_t>(value);
            std::memcpy(src + at, &narrow, 2);
        }
    }
    // 16-bit restart values: encode some slots so they decode to it. Mode 2
    // keeps index i in the other half of its dword, byte-swapped.
    if (!c.index32 && c.restart)
    {
        const uint16_t target = static_cast<uint16_t>(c.restartIndex);
        const uint16_t encoded = (c.endian & 3u) == 0u
            ? target
            : static_cast<uint16_t>((target >> 8) | (target << 8));
        for (uint32_t i = 0; i < c.count; ++i)
        {
            if (random() % 8u)
                continue;
            const size_t at = (c.endian & 3u) == 2u ? size_t(i ^ 1u) * 2u : size_t(i) * 2u;
            std::memcpy(src + at, &encoded, 2);
        }
    }
}

struct MapKey
{
    uint64_t token = 0;
    uint32_t bytes = 0;

    bool operator==(const MapKey& other) const noexcept
    {
        return token == other.token && bytes == other.bytes;
    }
};

// Deliberately weak, so probes collide and wrap around the table.
struct MapKeyHash
{
    size_t operator()(const MapKey& key) const noexcept
    {
        return static_cast<size_t>(key.token % 61u) * 7u + key.bytes % 3u;
    }
};

bool FrameHashMapMatchesUnorderedMap()
{
    std::mt19937 random(0xF1A7);
    mojorecomp::gpu::FrameHashMap<MapKey, uint64_t, MapKeyHash> map(16);
    std::unordered_map<MapKey, uint64_t, MapKeyHash> reference;
    for (uint32_t frame = 0; frame < 400; ++frame)
    {
        // Frames from empty to large enough to grow the table several times.
        const uint32_t operations = random() % (frame % 50u == 49u ? 5000u : 300u);
        for (uint32_t i = 0; i < operations; ++i)
        {
            const MapKey key{random() % 700u, static_cast<uint32_t>(random() % 4u)};
            if (random() % 2u)
            {
                const uint64_t value = random();
                map.Emplace(key, value);
                reference.emplace(key, value); // keeps an existing value, like Emplace
            }
            const uint64_t* found = map.Find(key);
            const auto expected = reference.find(key);
            if ((found != nullptr) != (expected != reference.end()) ||
                (found && *found != expected->second))
                return false;
        }
        if (map.Size() != reference.size())
            return false;
        map.Clear();
        reference.clear();
        for (uint32_t probe = 0; probe < 50; ++probe)
            if (map.Find(MapKey{random() % 700u, static_cast<uint32_t>(random() % 4u)}))
                return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    const bool bench = argc > 1 && std::string(argv[1]) == "--bench";
    if (!FrameHashMapMatchesUnorderedMap())
    {
        std::fprintf(stderr, "FAIL: FrameHashMap differs from std::unordered_map\n");
        return 1;
    }
    std::puts("PASS: FrameHashMap finds, keeps and clears entries like std::unordered_map");
    GuardedBuffer guarded;
    if (!guarded.Valid())
    {
        std::fprintf(stderr, "FAIL: could not map the guarded buffer\n");
        return 1;
    }

    std::mt19937 random(0x1DE5);
    const uint32_t counts[] = {0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 33, 63, 64, 65,
                               127, 255, 1000, 3001, 8192};
    uint64_t cases = 0;
    uint64_t fastBounds = 0;
    for (uint32_t round = 0; round < 40; ++round)
    for (bool index32 : {false, true})
    for (uint32_t endian = 0; endian < 4; ++endian)
    for (uint32_t count : counts)
    for (bool restart : {false, true})
    {
        Case c;
        c.index32 = index32;
        c.endian = endian | ((random() & 1u) << 2); // high bits must be ignored
        c.count = count;
        c.restart = restart;
        const uint32_t restartChoice = random() % 3u;
        c.restartIndex = restartChoice == 0 ? 0xFFFFFFFFu : restartChoice == 1 ? 0u : random();
        if (!index32)
            c.restartIndex &= 0xFFFFu; // as PrepareVertexBindings masks it
        c.indexOffset = random() % 4u == 0 ? random() & xenos::kVertexIndexMask : 0u;
        const uint32_t clamp = random() % 4u;
        c.minVertex = clamp == 0 ? 0u : random() % 4000u;
        c.maxVertex = clamp == 1 ? xenos::kVertexIndexMask
                    : clamp == 2 ? c.minVertex + random() % 5000u
                                 : random() % 4000u; // may be below the minimum
        c.minVertex &= xenos::kVertexIndexMask;
        c.maxVertex &= xenos::kVertexIndexMask;

        const uint64_t rawBytes = uint64_t(count) * (index32 ? 4u : 2u);
        const uint64_t readBytes = (!index32 && (c.endian & 3u) == 2u)
                                       ? ((rawBytes + 3u) & ~3ull)
                                       : rawBytes;
        if (readBytes > guarded.Capacity())
            return Fail("case does not fit the guarded buffer", c);
        uint8_t* src = guarded.Place(static_cast<size_t>(readBytes));
        FillIndices(src, static_cast<size_t>(readBytes), c, random);

        const Bounds expected = ReferenceBounds(src, c);
        mojorecomp::gpu::GuestIndexBounds fast{};
        if (mojorecomp::gpu::FastGuestIndexBounds(src, c.count, c.index32, c.endian, c.restart,
                                                  c.restartIndex, c.indexOffset, c.minVertex,
                                                  c.maxVertex, fast))
        {
            ++fastBounds;
            if (fast.maxRaw != expected.maxIndex || fast.maxEffective != expected.maxEffective ||
                fast.remap != expected.remap)
                return Fail("fast index bounds differ from the per-index loop", c);
        }
        else if (c.indexOffset == 0 && !c.index32)
            return Fail("the fast bounds declined a 16-bit draw without an index offset", c);

        for (bool rewritten : {false, true})
        {
            if (ReferenceIndices(src, c, rewritten) != FastIndices(src, c, rewritten))
                return Fail(rewritten ? "remapped host indices differ"
                                      : "host indices differ", c);
        }
        ++cases;
    }
    // The rest decline by design (an index offset, or a 32-bit index above the
    // vertex index mask), but enough must take the fast bounds to prove them.
    if (fastBounds * 3u < cases)
    {
        std::fprintf(stderr, "FAIL: only %llu of %llu cases used the fast bounds\n",
                     static_cast<unsigned long long>(fastBounds),
                     static_cast<unsigned long long>(cases));
        return 1;
    }
    std::printf("PASS: SSE2 index bounds and decoding match ReadIndexValue in %llu cases "
                "(%llu through the fast bounds)\n",
                static_cast<unsigned long long>(cases),
                static_cast<unsigned long long>(fastBounds));

    if (bench)
    {
        using Clock = std::chrono::steady_clock;
        for (bool index32 : {false, true})
        for (uint32_t endian : {0u, 1u, 2u})
        {
            Case c;
            c.index32 = index32;
            c.endian = endian;
            c.count = 3000;
            c.maxVertex = xenos::kVertexIndexMask;
            const size_t bytes = size_t(c.count) * (index32 ? 4u : 2u);
            uint8_t* src = guarded.Place(bytes);
            FillIndices(src, bytes, c, random);
            std::vector<uint8_t> dst(size_t(c.count) * 4u);
            constexpr int kIterations = 2000;
            uint64_t sink = 0;
            auto start = Clock::now();
            for (int i = 0; i < kIterations; ++i)
            {
                const Bounds b = ReferenceBounds(src, c);
                sink += b.maxEffective;
                for (uint32_t k = 0; k < c.count; ++k)
                {
                    const uint32_t v = ReadIndexValue(src, k, index32, endian);
                    std::memcpy(dst.data() + k * (index32 ? 4u : 2u), &v, index32 ? 4u : 2u);
                }
                sink += dst[i % dst.size()];
            }
            const double before = std::chrono::duration<double, std::nano>(Clock::now() - start)
                                      .count() / kIterations;
            start = Clock::now();
            for (int i = 0; i < kIterations; ++i)
            {
                mojorecomp::gpu::GuestIndexBounds b{};
                mojorecomp::gpu::FastGuestIndexBounds(src, c.count, index32, endian, false, 0,
                                                      0, 0, c.maxVertex, b);
                sink += b.maxEffective;
                mojorecomp::gpu::DecodeGuestIndices(dst.data(), src, c.count, index32, endian,
                                                    index32);
                sink += dst[i % dst.size()];
            }
            const double after = std::chrono::duration<double, std::nano>(Clock::now() - start)
                                     .count() / kIterations;
            std::printf("BENCH: %s endian %u, 3000 indices: scan+decode %.2f -> %.2f us "
                        "(sink %llu)\n", index32 ? "32-bit" : "16-bit", endian,
                        before / 1000.0, after / 1000.0,
                        static_cast<unsigned long long>(sink & 1u));
        }
    }
    return 0;
}
