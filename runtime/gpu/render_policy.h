#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "texture_abi.h"

namespace mojorecomp::gpu {

enum class TextureSource {
    Dummy,
    ResolveSnapshot,
    GuestTexture,
};

enum class EdramDepthTransferMode {
    ShaderStencilExport,
    FixedFunctionBitPlanes,
};

inline EdramDepthTransferMode SelectEdramDepthTransferMode(
    bool shaderStencilExport)
{
    return shaderStencilExport
        ? EdramDepthTransferMode::ShaderStencilExport
        : EdramDepthTransferMode::FixedFunctionBitPlanes;
}

inline uint8_t ApplyStencilBitPlane(uint8_t current, uint8_t source,
                                    uint32_t bit)
{
    const uint8_t mask = static_cast<uint8_t>(1u << bit);
    // The portable Vulkan path clears stencil first. A fragment survives the
    // bit-plane shader only when this source bit is set; REPLACE uses reference
    // 0xFF and a one-bit write mask, which sets exactly that bit.
    return (source & mask) != 0 ? static_cast<uint8_t>(current | mask) : current;
}

inline bool ShouldEnableAnisotropy(
    const texture_abi::Fetch2D& fetch, TextureSource source,
    uint32_t filteringOverride, float effectiveAnisotropy)
{
    // Render targets, video planes, lookup textures and other one-level images
    // must retain the filtering requested by the guest. Forcing anisotropy on
    // them changes UI/video sampling and can expose undefined mip data.
    return source == TextureSource::GuestTexture &&
           fetch.mipMax > fetch.mipMin && fetch.mipFilter != 2u &&
           filteringOverride > 1u && effectiveAnisotropy > 1.0f;
}

// RB_SURFACE_INFO gives a surface's pitch but not its height. One surface
// pass renders the same rows into its color and its depth, and on hardware
// those two ranges cannot overlap on the EDRAM ring without the game
// corrupting its own frame. That bounds the rows: the color range must end
// before depth starts and the depth range before color starts again. Returns 0
// (no bound) when the bases are equal or a pitch is 0.
inline uint32_t EdramSurfacePairRowLimit(uint32_t colorBaseTiles, uint32_t colorPitchTiles,
                                         uint32_t depthBaseTiles, uint32_t depthPitchTiles,
                                         uint32_t edramTiles)
{
    if (!colorPitchTiles || !depthPitchTiles || !edramTiles ||
        (edramTiles & (edramTiles - 1u)) != 0)
        return 0;
    const uint32_t mask = edramTiles - 1u;
    const uint32_t colorToDepth = (depthBaseTiles - colorBaseTiles) & mask;
    const uint32_t depthToColor = (colorBaseTiles - depthBaseTiles) & mask;
    if (!colorToDepth)
        return 0;
    return std::min(colorToDepth / colorPitchTiles, depthToColor / depthPitchTiles);
}

// Xenos surfaces at the same EDRAM base with the same tile pitch and format
// but different MSAA modes alias the same samples. In a tile, a 4x pixel
// (x, y) holds the samples of 2x pixels (2x, y) and (2x + 1, y) and of 1x
// pixels (2x..2x+1, 2y..2y+1); a 2x pixel (x, y) holds those of 1x pixels
// (x, 2y..2y+1). Returns how many owner pixels one alias pixel spans on each
// axis, or {0, 0} when the alias is not a higher MSAA mode of the owner with
// the same tile pitch. Sample counts are 1, 2 or 4.
struct EdramAliasScale
{
    uint32_t x = 0;
    uint32_t y = 0;
};

inline EdramAliasScale EdramAliasPixelScale(uint32_t aliasSamples, uint32_t aliasPitchTiles,
                                            uint32_t ownerSamples, uint32_t ownerPitchTiles)
{
    if (!aliasPitchTiles || aliasPitchTiles != ownerPitchTiles)
        return {};
    if (aliasSamples == 4 && ownerSamples == 2)
        return {2, 1};
    if (aliasSamples == 4 && ownerSamples == 1)
        return {2, 2};
    if (aliasSamples == 2 && ownerSamples == 1)
        return {1, 2};
    return {};
}

inline bool HasRecentPhysicalTileContent(uint64_t currentFrame,
                                         uint64_t lastPhysicalTileFrame)
{
    return lastPhysicalTileFrame != 0 && currentFrame <= lastPhysicalTileFrame + 1;
}

inline bool ShouldTreatAsSafeAreaOverlay(float minX, float minY,
                                         float maxX, float maxY,
                                         uint32_t logicalWidth,
                                         uint32_t logicalHeight)
{
    if (!logicalWidth || !logicalHeight)
        return false;

    const float width = maxX - minX;
    const float height = maxY - minY;

    // Small normalized quads are also UI (button icons, glyph decorations).
    // Fullscreen normalized compositors cover roughly the unit square and must
    // remain neutral after the scene has already been composed.
    const float maxMagnitude = std::max({std::fabs(minX), std::fabs(minY),
                                         std::fabs(maxX), std::fabs(maxY)});
    if (maxMagnitude <= 2.0f)
        return width < 0.75f && height < 0.75f;

    const bool coversMostFrame =
        width >= static_cast<float>(logicalWidth) * 0.75f &&
        height >= static_cast<float>(logicalHeight) * 0.75f;
    return !coversMostFrame;
}

inline std::array<float, 2> SafeAreaViewportScale(float targetAspect)
{
    constexpr float nativeAspect = 16.0f / 9.0f;
    if (!std::isfinite(targetAspect) || targetAspect <= 0.0f ||
        std::fabs(targetAspect - nativeAspect) < 0.0001f)
        return {1.0f, 1.0f};

    // Safe-area UI must be fitted after native clip-space clipping. Wider
    // outputs shrink the viewport horizontally; narrower outputs shrink it
    // vertically. This keeps off-screen guest glyphs/icons clipped instead of
    // pulling them back into view by scaling each vertex before clipping.
    if (targetAspect >= nativeAspect)
        return {nativeAspect / targetAspect, 1.0f};
    return {1.0f, targetAspect / nativeAspect};
}

inline std::array<float, 2> SceneAspectScale(float targetAspect)
{
    constexpr float nativeAspect = 16.0f / 9.0f;
    if (!std::isfinite(targetAspect) || targetAspect <= 0.0f ||
        targetAspect <= nativeAspect + 0.0001f)
        return {1.0f, 1.0f};

    // Wider modes need extra horizontal field of view. Narrower modes are kept
    // in native clip space and center-cropped at presentation instead; scaling
    // final clip-space X for those modes separates tiled/multipass draws that
    // overlap correctly at the game's native 16:9 projection.
    return {nativeAspect / targetAspect, 1.0f};
}

struct PresentationSourceCrop
{
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

inline PresentationSourceCrop ScenePresentationSourceCrop(
    uint32_t sourceWidth, uint32_t sourceHeight, float targetAspect,
    bool preserveNative16x9)
{
    PresentationSourceCrop crop{0, 0, sourceWidth, sourceHeight};
    if (!sourceWidth || !sourceHeight || preserveNative16x9 ||
        !std::isfinite(targetAspect) || targetAspect <= 0.0f)
        return crop;

    const float sourceAspect = static_cast<float>(sourceWidth) /
                               static_cast<float>(sourceHeight);
    if (targetAspect >= sourceAspect - 0.0001f)
        return crop;

    const uint32_t croppedWidth = std::clamp<uint32_t>(
        static_cast<uint32_t>(std::lround(static_cast<double>(sourceHeight) *
                                          static_cast<double>(targetAspect))),
        1u, sourceWidth);
    crop.x = (sourceWidth - croppedWidth) / 2u;
    crop.width = croppedWidth;
    return crop;
}

inline bool ShouldApplyConfiguredAspect(bool physicalTileReplay,
                                        bool physicalTileContentRecent,
                                        bool logical3DSceneSeen,
                                        bool logical3DSceneDraw)
{
    // Physical EDRAM tiles contain portions of one native 16:9 frame. Applying
    // the host aspect transform independently to each portion duplicates or
    // drops content when the tiles are composed. Once a pure 2D frame has seen
    // physical tile content, its later logical composition passes must also stay
    // native so the completed frame is fitted exactly once at presentation.
    // A real indexed 3D scene owns the configured aspect transform directly.
    if (physicalTileReplay)
        return false;
    if (logical3DSceneSeen)
        return logical3DSceneDraw;
    return !physicalTileContentRecent;
}

inline bool ShouldPreserveNativePresentation(bool physicalTileContentSeen,
                                             bool logical3DSceneSeen)
{
    // Pure 2D and video frames are assembled from physical tiles and then fitted
    // once at presentation. A collapsed logical 3D scene already received the
    // configured aspect transform and must continue to fill the selected mode.
    return physicalTileContentSeen && !logical3DSceneSeen;
}

inline bool ShouldUseLogicalFirstTile(bool collapseEnabled, bool firstSceneTile,
                                      uint64_t currentFrame,
                                      uint64_t lastIndexedSceneFrame)
{
    // Only a real indexed 3D scene arms tile collapse. Predicated 2D boot and
    // frontend passes use the same 0/416/832 windows, but every window contains
    // a physical portion that must remain physical. Promoting tile 0 alone
    // produces one complete copy followed by fragments from tiles 1 and 2.
    return collapseEnabled && firstSceneTile && lastIndexedSceneFrame != 0 &&
           currentFrame <= lastIndexedSceneFrame + 1;
}

} // namespace mojorecomp::gpu
