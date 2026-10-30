#pragma once

// Guest index buffers on the Vulkan thread (docs/HFR-PHASE3.md section 7).
// Every indexed draw used to decode its indices one by one twice: once to find
// the largest vertex it reads, once more to write the host index buffer. These
// helpers do both with SSE2 and give exactly the values vk_presenter.cpp's
// ReadIndexValue decodes, which tests/vertex_path_test.cpp checks against a
// verbatim copy of it.

#include <algorithm>
#include <cstdint>
#include <cstring>

#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE2__)
#include <emmintrin.h>
#define MOJORECOMP_INDEX_SCAN_SSE2 1
#endif

#include "xenos.h"

namespace mojorecomp::gpu {

// One index, as ReadIndexValue decodes it. 32-bit indices use the Xenos endian
// swap of their dword. 16-bit indices: mode 2 swaps the dword that holds a
// pair of them, modes 1 and 3 swap the bytes of each index.
inline uint32_t DecodeGuestIndex(const uint8_t* src, uint32_t index, bool index32,
                                 uint32_t endian) noexcept
{
    if (index32)
    {
        uint32_t value = 0;
        std::memcpy(&value, src + uint64_t(index) * 4u, sizeof(value));
        switch (endian & 3u)
        {
            case 1: return ((value & 0x00FF00FFu) << 8) | ((value & 0xFF00FF00u) >> 8);
            case 2: return __builtin_bswap32(value);
            case 3: return (value >> 16) | (value << 16);
            default: return value;
        }
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
    if ((endian & 3u) != 0u)
        value = static_cast<uint16_t>((value >> 8) | (value << 8));
    return value;
}

#if defined(MOJORECOMP_INDEX_SCAN_SSE2)
namespace index_scan_detail {

// Eight 16-bit indices starting at an index that is a multiple of two.
inline __m128i Load16(const uint8_t* src, uint32_t first, uint32_t endian) noexcept
{
    const __m128i raw = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + uint64_t(first) * 2u));
    switch (endian & 3u)
    {
        case 0:
            return raw;
        case 2:
        {
            const __m128i bytes = _mm_or_si128(_mm_slli_epi16(raw, 8), _mm_srli_epi16(raw, 8));
            return _mm_or_si128(_mm_slli_epi32(bytes, 16), _mm_srli_epi32(bytes, 16));
        }
        default:
            return _mm_or_si128(_mm_slli_epi16(raw, 8), _mm_srli_epi16(raw, 8));
    }
}

// Four 32-bit indices.
inline __m128i Load32(const uint8_t* src, uint32_t first, uint32_t endian) noexcept
{
    const __m128i raw = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + uint64_t(first) * 4u));
    switch (endian & 3u)
    {
        case 1:
            return _mm_or_si128(_mm_slli_epi16(raw, 8), _mm_srli_epi16(raw, 8));
        case 2:
        {
            const __m128i bytes = _mm_or_si128(_mm_slli_epi16(raw, 8), _mm_srli_epi16(raw, 8));
            return _mm_or_si128(_mm_slli_epi32(bytes, 16), _mm_srli_epi32(bytes, 16));
        }
        case 3:
            return _mm_or_si128(_mm_slli_epi32(raw, 16), _mm_srli_epi32(raw, 16));
        default:
            return raw;
    }
}

inline int PopCount16(int mask) noexcept
{
    int count = 0;
    for (; mask; mask &= mask - 1)
        ++count;
    return count;
}

} // namespace index_scan_detail
#endif

struct GuestIndexRange
{
    uint32_t minRaw = 0xFFFFFFFFu;
    uint32_t maxRaw = 0;
    // Indices that are not the restart index.
    uint32_t counted = 0;
};

// The smallest and largest decoded index, skipping restartIndex when restart
// is set.
inline GuestIndexRange ScanGuestIndices(const uint8_t* src, uint32_t count, bool index32,
                                        uint32_t endian, bool restart,
                                        uint32_t restartIndex) noexcept
{
    GuestIndexRange range;
    uint32_t i = 0;
    uint32_t skipped = 0;
#if defined(MOJORECOMP_INDEX_SCAN_SSE2)
    using namespace index_scan_detail;
    if (!index32)
    {
        // Unsigned 16-bit min/max in SSE2: bias by 0x8000 and use the signed forms.
        const __m128i bias = _mm_set1_epi16(static_cast<short>(0x8000));
        const __m128i restartLanes = _mm_set1_epi16(static_cast<short>(restartIndex & 0xFFFFu));
        __m128i lo = _mm_set1_epi16(0x7FFF);
        __m128i hi = _mm_set1_epi16(static_cast<short>(0x8000));
        for (; i + 8u <= count; i += 8u)
        {
            const __m128i value = Load16(src, i, endian);
            __m128i forMin = value;
            __m128i forMax = value;
            if (restart)
            {
                const __m128i match = _mm_cmpeq_epi16(value, restartLanes);
                skipped += static_cast<uint32_t>(PopCount16(_mm_movemask_epi8(match)) / 2);
                forMin = _mm_or_si128(value, match);    // 0xFFFF never lowers the minimum
                forMax = _mm_andnot_si128(match, value); // 0 never raises the maximum
            }
            lo = _mm_min_epi16(lo, _mm_xor_si128(forMin, bias));
            hi = _mm_max_epi16(hi, _mm_xor_si128(forMax, bias));
        }
        if (i)
        {
            alignas(16) uint16_t los[8];
            alignas(16) uint16_t his[8];
            _mm_store_si128(reinterpret_cast<__m128i*>(los), _mm_xor_si128(lo, bias));
            _mm_store_si128(reinterpret_cast<__m128i*>(his), _mm_xor_si128(hi, bias));
            for (int lane = 0; lane < 8; ++lane)
            {
                range.minRaw = std::min<uint32_t>(range.minRaw, los[lane]);
                range.maxRaw = std::max<uint32_t>(range.maxRaw, his[lane]);
            }
        }
    }
    else
    {
        // Unsigned 32-bit min/max in SSE2: biased signed compares and a blend.
        const __m128i bias = _mm_set1_epi32(static_cast<int>(0x80000000u));
        const __m128i restartLanes = _mm_set1_epi32(static_cast<int>(restartIndex));
        __m128i lo = _mm_set1_epi32(0x7FFFFFFF);
        __m128i hi = _mm_set1_epi32(static_cast<int>(0x80000000u));
        for (; i + 4u <= count; i += 4u)
        {
            const __m128i value = Load32(src, i, endian);
            __m128i forMin = value;
            __m128i forMax = value;
            if (restart)
            {
                const __m128i match = _mm_cmpeq_epi32(value, restartLanes);
                skipped += static_cast<uint32_t>(PopCount16(_mm_movemask_epi8(match)) / 4);
                forMin = _mm_or_si128(value, match);
                forMax = _mm_andnot_si128(match, value);
            }
            const __m128i biasedMin = _mm_xor_si128(forMin, bias);
            const __m128i biasedMax = _mm_xor_si128(forMax, bias);
            const __m128i lower = _mm_cmpgt_epi32(lo, biasedMin);
            lo = _mm_or_si128(_mm_and_si128(lower, biasedMin), _mm_andnot_si128(lower, lo));
            const __m128i higher = _mm_cmpgt_epi32(biasedMax, hi);
            hi = _mm_or_si128(_mm_and_si128(higher, biasedMax), _mm_andnot_si128(higher, hi));
        }
        if (i)
        {
            alignas(16) uint32_t los[4];
            alignas(16) uint32_t his[4];
            _mm_store_si128(reinterpret_cast<__m128i*>(los), _mm_xor_si128(lo, bias));
            _mm_store_si128(reinterpret_cast<__m128i*>(his), _mm_xor_si128(hi, bias));
            for (int lane = 0; lane < 4; ++lane)
            {
                range.minRaw = std::min(range.minRaw, los[lane]);
                range.maxRaw = std::max(range.maxRaw, his[lane]);
            }
        }
    }
#endif
    // A lane holding the restart index contributed only the neutral values,
    // so the vector minimum and maximum stay exact. The remainder is scalar.
    const uint32_t restartValue = index32 ? restartIndex : (restartIndex & 0xFFFFu);
    for (; i < count; ++i)
    {
        const uint32_t raw = DecodeGuestIndex(src, i, index32, endian);
        if (restart && raw == restartValue)
        {
            ++skipped;
            continue;
        }
        range.minRaw = std::min(range.minRaw, raw);
        range.maxRaw = std::max(range.maxRaw, raw);
    }
    range.counted = count - skipped;
    if (!range.counted)
        range = {};
    return range;
}

// Writes `count` decoded indices as host uint16 (dstIndex32 false; 16-bit
// sources only) or uint32 values.
inline void DecodeGuestIndices(void* dst, const uint8_t* src, uint32_t count, bool index32,
                               uint32_t endian, bool dstIndex32) noexcept
{
    uint32_t i = 0;
    auto* dst8 = static_cast<uint8_t*>(dst);
#if defined(MOJORECOMP_INDEX_SCAN_SSE2)
    using namespace index_scan_detail;
    if (index32)
    {
        for (; i + 4u <= count; i += 4u)
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst8 + uint64_t(i) * 4u),
                             Load32(src, i, endian));
    }
    else if (!dstIndex32)
    {
        for (; i + 8u <= count; i += 8u)
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst8 + uint64_t(i) * 2u),
                             Load16(src, i, endian));
    }
    else
    {
        const __m128i zero = _mm_setzero_si128();
        for (; i + 8u <= count; i += 8u)
        {
            const __m128i value = Load16(src, i, endian);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst8 + uint64_t(i) * 4u),
                             _mm_unpacklo_epi16(value, zero));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst8 + uint64_t(i) * 4u + 16u),
                             _mm_unpackhi_epi16(value, zero));
        }
    }
#endif
    for (; i < count; ++i)
    {
        const uint32_t value = DecodeGuestIndex(src, i, index32, endian);
        if (dstIndex32 || index32)
            std::memcpy(dst8 + uint64_t(i) * 4u, &value, sizeof(value));
        else
        {
            const uint16_t narrow = static_cast<uint16_t>(value);
            std::memcpy(dst8 + uint64_t(i) * 2u, &narrow, sizeof(narrow));
        }
    }
}

struct GuestIndexBounds
{
    uint32_t maxRaw = 0;
    uint32_t maxEffective = 0;
    bool remap = false;
};

// What PrepareVertexBindings derives by passing every non-restart index
// through xenos::RemapVertexIndex: the largest raw and remapped index, and
// whether any index changes. With no index offset the remap is a clamp, which
// is monotonic, so the scan's minimum and maximum decide all three. Returns
// false where that does not hold (an index offset, or a 32-bit index above the
// vertex index mask); the caller then walks the indices itself.
inline bool FastGuestIndexBounds(const uint8_t* src, uint32_t count, bool index32,
                                 uint32_t endian, bool restart, uint32_t restartIndex,
                                 uint32_t indexOffset, uint32_t minVertex,
                                 uint32_t maxVertex, GuestIndexBounds& out) noexcept
{
    if (indexOffset != 0)
        return false;
    const GuestIndexRange range =
        ScanGuestIndices(src, count, index32, endian, restart, restartIndex);
    out = {};
    if (!range.counted)
        return true;
    if (range.maxRaw > xenos::kVertexIndexMask)
        return false;
    out.maxRaw = range.maxRaw;
    if (minVertex <= maxVertex)
    {
        out.maxEffective = std::clamp(range.maxRaw, minVertex, maxVertex);
        out.remap = range.minRaw < minVertex || range.maxRaw > maxVertex;
    }
    else
    {
        out.maxEffective = range.maxRaw;
    }
    return true;
}

} // namespace mojorecomp::gpu
