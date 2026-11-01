// White-box GPU integration test: compile the actual presenter into this test
// executable so descriptor allocation, shared uploads, transitions and draws are
// exercised without adding test-only public entry points to the runtime.
#include "../gpu/vk_presenter.cpp"
#include "../gpu/shader_translator.h"
#include "../host/window.h"
#include <fstream>
#include <filesystem>
#include <stdexcept>

// The white-box presenter probe does not exercise host Debug Mode. HostWindow and
// the presenter only need these two read/poll seams, so keep deterministic stubs
// here rather than linking the complete debug clock/audio/input stack into a GPU
// regression executable.
namespace mojorecomp::debug {
void PollHotkeys() {}
DebugOverlaySnapshot GetOverlaySnapshot() noexcept { return {}; }
}

namespace mojorecomp::subtitles {
SubtitleSnapshot GetSnapshot() { return {}; }
}

static void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

static void ClearLive(const VkClearColorValue& value)
{
    EndColorRendering();
    ColorBacking* backing = ActiveColorBacking();
    Require(backing != nullptr, "active color backing for clear");
    TransitionColorBacking(*backing, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p_vkCmdClearColorImage(g_commandBuffer, backing->image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
    TransitionColorBacking(*backing, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    ResumeColorRendering();
}

struct ProbeImage
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

static ProbeImage CreateProbeImage(VkExtent2D extent, VkFormat format,
                                   VkImageUsageFlags usage)
{
    ProbeImage result{};
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {extent.width, extent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = usage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    Require(p_vkCreateImage(g_device, &imageInfo, nullptr, &result.image) == VK_SUCCESS,
            "create probe image");

    VkMemoryRequirements requirements{};
    p_vkGetImageMemoryRequirements(g_device, result.image, &requirements);
    const uint32_t memoryType = FindMemoryType(
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Require(memoryType != UINT32_MAX, "find probe image memory");
    VkMemoryAllocateInfo memoryInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    memoryInfo.allocationSize = requirements.size;
    memoryInfo.memoryTypeIndex = memoryType;
    Require(p_vkAllocateMemory(g_device, &memoryInfo, nullptr, &result.memory) == VK_SUCCESS,
            "allocate probe image memory");
    Require(p_vkBindImageMemory(g_device, result.image, result.memory, 0) == VK_SUCCESS,
            "bind probe image memory");

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = result.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    Require(p_vkCreateImageView(g_device, &viewInfo, nullptr, &result.view) == VK_SUCCESS,
            "create probe image view");
    return result;
}

static void DestroyProbeImage(ProbeImage& image)
{
    if (image.view) p_vkDestroyImageView(g_device, image.view, nullptr);
    if (image.image) p_vkDestroyImage(g_device, image.image, nullptr);
    if (image.memory) p_vkFreeMemory(g_device, image.memory, nullptr);
    image = {};
}

static void WriteProbeBmp(const std::filesystem::path& path, const uint8_t* pixels,
                          VkExtent2D extent, VkFormat format)
{
    if (path.empty()) return;
    std::filesystem::create_directories(path.parent_path());
    const size_t pixelBytes = size_t(extent.width) * extent.height * 4u;
    std::vector<uint8_t> bgra(pixelBytes);
    for (size_t i = 0; i < pixelBytes; i += 4)
    {
        if (format == VK_FORMAT_B8G8R8A8_UNORM)
            std::memcpy(bgra.data() + i, pixels + i, 4);
        else
        {
            bgra[i + 0] = pixels[i + 2];
            bgra[i + 1] = pixels[i + 1];
            bgra[i + 2] = pixels[i + 0];
            bgra[i + 3] = pixels[i + 3];
        }
    }
    BITMAPFILEHEADER file{};
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
    file.bfSize = file.bfOffBits + static_cast<DWORD>(bgra.size());
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = static_cast<LONG>(extent.width);
    info.biHeight = -static_cast<LONG>(extent.height);
    info.biPlanes = 1;
    info.biBitCount = 32;
    std::ofstream output(path, std::ios::binary);
    Require(bool(output), "open FXAA probe BMP");
    output.write(reinterpret_cast<const char*>(&file), sizeof(file));
    output.write(reinterpret_cast<const char*>(&info), sizeof(info));
    output.write(reinterpret_cast<const char*>(bgra.data()),
                 static_cast<std::streamsize>(bgra.size()));
    Require(bool(output), "write FXAA probe BMP");
}

enum class PresentationProbe
{
    Fxaa,
    // FXAA at the internal size, EASU to a larger output, then RCAS.
    FsrEasu,
    // Output not larger than the internal image: scaled crop, then RCAS only.
    FsrRcasOnly,
};

static void CheckFxaaOrientation(const std::filesystem::path& capturePath,
                                 PresentationProbe probe = PresentationProbe::Fxaa)
{
    const bool fsr = probe != PresentationProbe::Fxaa;
    mojorecomp::config::RuntimeConfig config{};
    config.antiAliasing = mojorecomp::config::AntiAliasing::Fxaa;
    if (fsr)
        config.upscaler = mojorecomp::config::Upscaler::Fsr1;
    mojorecomp::config::Set(config);
    // The logical guest frame is 320x180; EASU needs a larger window.
    const uint32_t windowScale = probe == PresentationProbe::FsrEasu ? 2u : 1u;
    Require(HostWindow_Init(320 * windowScale, 180 * windowScale, true), "hidden FXAA window");
    Require(VkPresenter_Init(HostWindow_NativeHandle(), 320, 180), "FXAA presenter init");
    Require(BeginFrame(), "begin FXAA orientation frame");
    Require(SwitchActiveColorSurface(0, 0), "select FXAA source surface");

    ClearLive(VkClearColorValue{{0.0f, 0.0f, 1.0f, 1.0f}});
    VkClearAttachment top{};
    top.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    top.colorAttachment = 0;
    top.clearValue.color = {{1.0f, 0.0f, 0.0f, 1.0f}};
    const VkClearRect topHalf{{{0, 0}, {g_internalExtent.width,
                                       g_internalExtent.height / 2u}}, 0, 1};
    p_vkCmdClearAttachments(g_commandBuffer, 1, &top, 1, &topHalf);
    EndColorRendering();

    ColorBacking* source = ActiveColorBacking();
    Require(source != nullptr, "FXAA source backing");
    TransitionColorBacking(*source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    ProbeImage target = CreateProbeImage(
        g_outputExtent, g_swapFormat,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

    const auto savedImages = g_images;
    const auto savedViews = g_swapViews;
    const auto savedInitialized = g_imageInitialized;
    g_images = {target.image};
    g_swapViews = {target.view};
    g_imageInitialized = {false};
    if (fsr)
    {
        Require(DrawFsrPresentation(0, source->image, g_extent.width, g_extent.height, false),
                "draw FSR orientation frame");
        const bool easuUsed = g_fsrEasu.image != VK_NULL_HANDLE;
        Require(easuUsed == (probe == PresentationProbe::FsrEasu),
                "FSR must use EASU exactly when the output is larger than the crop");
        Require(!easuUsed || (g_fsrEasu.extent.width == g_outputExtent.width &&
                              g_fsrEasu.extent.height == g_outputExtent.height),
                "EASU target must be the output size");
        Require(g_fsrAa.image != VK_NULL_HANDLE, "FSR path must run FXAA first");
    }
    else
        Require(DrawFxaa(0, source->image, g_extent.width, g_extent.height),
                "draw FXAA orientation frame");

    VkImageMemoryBarrier toReadback{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toReadback.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toReadback.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toReadback.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toReadback.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toReadback.srcQueueFamilyIndex = toReadback.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toReadback.image = target.image;
    toReadback.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p_vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                           0, nullptr, 0, nullptr, 1, &toReadback);

    // The presenter's readback area is sized for the internal image; an output
    // larger than that (the EASU probe) is read back from the end of the
    // upload arena instead, which this probe does not otherwise use.
    const VkDeviceSize outputBytes =
        VkDeviceSize(g_outputExtent.width) * g_outputExtent.height * 4u;
    const VkDeviceSize readbackOffset = outputBytes <= g_readbackBytes
        ? g_readbackOffset : (kUploadBytes - outputBytes) & ~VkDeviceSize(255);
    VkBufferImageCopy copy{};
    copy.bufferOffset = readbackOffset;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {g_outputExtent.width, g_outputExtent.height, 1};
    p_vkCmdCopyImageToBuffer(g_commandBuffer, target.image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_uploadBuffer, 1, &copy);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    p_vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT, 0,
                           1, &host, 0, nullptr, 0, nullptr);
    Require(p_vkEndCommandBuffer(g_commandBuffer) == VK_SUCCESS,
            "end FXAA orientation commands");
    Require(p_vkResetFences(g_device, 1, &g_fence) == VK_SUCCESS,
            "reset FXAA orientation fence");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &g_commandBuffer;
    Require(p_vkQueueSubmit(g_queue, 1, &submit, g_fence) == VK_SUCCESS,
            "submit FXAA orientation probe");
    Require(p_vkWaitForFences(g_device, 1, &g_fence, VK_TRUE, 10000000000ull) == VK_SUCCESS,
            "wait FXAA orientation probe");
    g_frameOpen = false;

    const uint8_t* pixels = g_uploadMapped + readbackOffset;
    WriteProbeBmp(capturePath, pixels, g_outputExtent, g_swapFormat);
    const auto isRed = [&](uint32_t x, uint32_t y) {
        const uint8_t* pixel = pixels + (size_t(y) * g_outputExtent.width + x) * 4u;
        const uint8_t r = g_swapFormat == VK_FORMAT_B8G8R8A8_UNORM ? pixel[2] : pixel[0];
        const uint8_t b = g_swapFormat == VK_FORMAT_B8G8R8A8_UNORM ? pixel[0] : pixel[2];
        return r > 220 && b < 32;
    };
    const auto isBlue = [&](uint32_t x, uint32_t y) {
        const uint8_t* pixel = pixels + (size_t(y) * g_outputExtent.width + x) * 4u;
        const uint8_t r = g_swapFormat == VK_FORMAT_B8G8R8A8_UNORM ? pixel[2] : pixel[0];
        const uint8_t b = g_swapFormat == VK_FORMAT_B8G8R8A8_UNORM ? pixel[0] : pixel[2];
        return b > 220 && r < 32;
    };
    const uint32_t x = g_outputExtent.width / 2u;
    Require(isRed(x, g_outputExtent.height / 4u),
            "FXAA must preserve the red top half");
    Require(isBlue(x, g_outputExtent.height * 3u / 4u),
            "FXAA must preserve the blue bottom half");

    g_images = savedImages;
    g_swapViews = savedViews;
    g_imageInitialized = savedInitialized;
    DestroyProbeImage(target);
    VkPresenter_Shutdown();
    std::puts(probe == PresentationProbe::FsrEasu
                  ? "PASS: FXAA + FSR 1 EASU + RCAS preserve presentation orientation and colour"
              : probe == PresentationProbe::FsrRcasOnly
                  ? "PASS: FXAA + RCAS-only presentation preserves orientation and colour"
                  : "PASS: FXAA preserves top-to-bottom presentation orientation");
}

// Regression for the blocky strip at the top of the frame: the EDRAM layout of
// a traced COT gameplay frame, as register values only. The 1280x720 frame
// has color at tile 0 and depth at 0x2D0; its 640-wide 4x MSAA alias uses the
// same bases. Sized from the 720-line frame, the alias's depth claimed 1440
// tiles and wrapped past tile 2047 into the frame's first 112 color tiles.
static void CheckEdramSurfacePairBound()
{
    mojorecomp::config::Set(mojorecomp::config::RuntimeConfig{});
    Require(HostWindow_Init(1280, 720, true), "hidden EDRAM window");
    Require(VkPresenter_Init(HostWindow_NativeHandle(), 1280, 720), "EDRAM presenter init");
    Require(BeginFrame(), "begin EDRAM frame");

    Require(SwitchActiveColorSurfaces(0x14000500u, 0x00000000u, 0, false, false) &&
                SwitchActiveDepthSurface(0x14000500u, 0x000102D0u, true, false),
            "select the 1280x720 frame");
    NoteActiveEdramSurfacePair();
    const EdramOwner frameColor{EdramOwnerKind::Color, g_activeColorSurfaceKey};
    EdramBackingMeta meta{};
    Require(GetEdramBackingMeta(frameColor, meta) && EdramBackingTileCount(meta) == 720,
            "the 1280x720 frame color must stay 720 tiles");
    Require(ClaimEdramOwnership(frameColor), "claim the frame color");

    Require(SwitchActiveColorSurfaces(0x0A020280u, 0x00000000u, 0, false, false) &&
                SwitchActiveDepthSurface(0x0A020280u, 0x000102D0u, true, false),
            "select the 4x MSAA alias");
    NoteActiveEdramSurfacePair();
    const EdramOwner aliasDepth{EdramOwnerKind::Depth, g_activeDepthSurfaceKey};
    Require(GetEdramBackingMeta(aliasDepth, meta) && EdramBackingTileCount(meta) == 720,
            "the 4x alias depth must be 720 tiles (45 rows of 16)");
    Require(ClaimEdramOwnership(aliasDepth), "claim the alias depth");
    for (uint32_t tile = 0; tile < 720; ++tile)
        Require(EdramOwnerEqual(g_edramOwners[tile], frameColor),
                "the alias depth must not take the frame color's tiles");
    for (uint32_t tile = 720; tile < 1440; ++tile)
        Require(EdramOwnerEqual(g_edramOwners[tile], aliasDepth),
                "the alias depth must own its own 720 tiles");

    EndColorRendering();
    Require(p_vkEndCommandBuffer(g_commandBuffer) == VK_SUCCESS, "end EDRAM commands");
    Require(p_vkResetFences(g_device, 1, &g_fence) == VK_SUCCESS, "reset EDRAM fence");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &g_commandBuffer;
    Require(p_vkQueueSubmit(g_queue, 1, &submit, g_fence) == VK_SUCCESS, "submit EDRAM probe");
    Require(p_vkWaitForFences(g_device, 1, &g_fence, VK_TRUE, 10000000000ull) == VK_SUCCESS,
            "wait EDRAM probe");
    g_frameOpen = false;
    VkPresenter_Shutdown();
    std::puts("PASS: a 4x MSAA alias stays within its color/depth EDRAM layout");
}

struct ProbeBuffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* mapped = nullptr;
};

static ProbeBuffer CreateProbeBuffer(VkDeviceSize size)
{
    ProbeBuffer out;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    Require(p_vkCreateBuffer(g_device, &bi, nullptr, &out.buffer) == VK_SUCCESS, "probe buffer");
    VkMemoryRequirements req{};
    p_vkGetBufferMemoryRequirements(g_device, out.buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    Require(ai.memoryTypeIndex != UINT32_MAX &&
                p_vkAllocateMemory(g_device, &ai, nullptr, &out.memory) == VK_SUCCESS &&
                p_vkBindBufferMemory(g_device, out.buffer, out.memory, 0) == VK_SUCCESS &&
                p_vkMapMemory(g_device, out.memory, 0, VK_WHOLE_SIZE, 0,
                              reinterpret_cast<void**>(&out.mapped)) == VK_SUCCESS,
            "probe buffer memory");
    return out;
}

static void DestroyProbeBuffer(ProbeBuffer& buffer)
{
    if (buffer.buffer)
        p_vkDestroyBuffer(g_device, buffer.buffer, nullptr);
    if (buffer.memory)
        p_vkFreeMemory(g_device, buffer.memory, nullptr);
    buffer = {};
}

// Submits the open frame's commands and waits, making transfer writes visible
// to the host. BeginFrame() opens the next one.
static void SubmitProbeFrame(const char* what)
{
    EndColorRendering();
    VkMemoryBarrier toHost{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    p_vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &toHost, 0, nullptr, 0, nullptr);
    Require(p_vkEndCommandBuffer(g_commandBuffer) == VK_SUCCESS, what);
    Require(p_vkResetFences(g_device, 1, &g_fence) == VK_SUCCESS, what);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &g_commandBuffer;
    Require(p_vkQueueSubmit(g_queue, 1, &submit, g_fence) == VK_SUCCESS, what);
    Require(p_vkWaitForFences(g_device, 1, &g_fence, VK_TRUE, 10000000000ull) == VK_SUCCESS,
            what);
    g_frameOpen = false;
}

static void CopyDepthAspect(uint64_t key, VkImageAspectFlags aspect, const ProbeBuffer& buffer,
                            VkDeviceSize offset, bool toImage)
{
    DepthBacking* backing = FindDepthBacking(key);
    Require(backing != nullptr, "probe depth backing");
    TransitionDepthBacking(*backing, toImage ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                                             : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy region{};
    region.bufferOffset = offset;
    region.imageSubresource = {aspect, 0, 0, 1};
    region.imageExtent = {g_internalExtent.width, g_internalExtent.height, 1};
    if (toImage)
        p_vkCmdCopyBufferToImage(g_commandBuffer, buffer.buffer, backing->image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    else
        p_vkCmdCopyImageToBuffer(g_commandBuffer, backing->image,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer.buffer, 1, &region);
}

struct StencilCopyCase
{
    const char* name;
    EdramOwnerKind sourceKind;
    uint32_t sourceSurface;
    uint32_t sourceInfo;
    uint32_t destSurface;
    uint32_t destDepthInfo;
    uint32_t destLocal;
    uint32_t tiles;
};

// Runs one depth-target transfer through the eight bit-plane draws and then
// through the compute copy, into the same starting destination, and requires
// identical stencil and depth everywhere.
static void CheckStencilCopyCase(const StencilCopyCase& c, ProbeBuffer& buffer)
{
    const uint32_t width = g_internalExtent.width;
    const uint32_t height = g_internalExtent.height;
    const VkDeviceSize pixels = VkDeviceSize(width) * height;
    // [source stencil][source depth or color][dest stencil][dest depth]
    // [bit-plane stencil][bit-plane depth][compute stencil][compute depth]
    const VkDeviceSize srcStencil = 0, srcWords = pixels, dstStencil = 5 * pixels,
                       dstDepth = 6 * pixels, planesStencil = 10 * pixels,
                       planesDepth = 11 * pixels, copyStencil = 15 * pixels,
                       copyDepth = 16 * pixels;

    const uint64_t sourceKey = (uint64_t(c.sourceSurface) << 32) | c.sourceInfo;
    const uint64_t destKey = (uint64_t(c.destSurface) << 32) | c.destDepthInfo;
    DepthBacking* destBacking = nullptr;
    Require(CreateDepthBacking(destKey, destBacking), c.name);
    const bool destD24 = destBacking->format == VK_FORMAT_D24_UNORM_S8_UINT;
    bool sourceD24 = false;
    if (c.sourceKind == EdramOwnerKind::Depth)
    {
        DepthBacking* sourceBacking = nullptr;
        Require(CreateDepthBacking(sourceKey, sourceBacking), c.name);
        sourceD24 = sourceBacking->format == VK_FORMAT_D24_UNORM_S8_UINT;
    }
    else
    {
        ColorBacking* sourceBacking = nullptr;
        Require(CreateColorBacking(c.sourceSurface, c.sourceInfo, sourceBacking), c.name);
    }

    // Per-pixel patterns that exercise every stencil bit.
    uint8_t* bytes = buffer.mapped;
    auto* words = reinterpret_cast<uint32_t*>(bytes);
    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            const VkDeviceSize i = VkDeviceSize(y) * width + x;
            const uint32_t hash = (x * 0x9E3779B1u) ^ (y * 0x85EBCA77u) ^ (x >> 3) ^ (y << 5);
            bytes[srcStencil + i] = uint8_t(hash >> 11);
            if (c.sourceKind == EdramOwnerKind::Depth)
            {
                const float depth = float((hash >> 8) % 1000u) / 1000.0f;
                uint32_t value = 0;
                if (sourceD24)
                    value = uint32_t(depth * 16777215.0f);
                else
                    std::memcpy(&value, &depth, sizeof(value));
                words[srcWords / 4 + i] = value;
            }
            else
            {
                words[srcWords / 4 + i] = hash * 0x2545F491u;
            }
            bytes[dstStencil + i] = uint8_t((x ^ y ^ 0x5Au) & 255u);
            const float garbageDepth = 0.25f;
            uint32_t value = 0x00400000u;
            if (!destD24)
                std::memcpy(&value, &garbageDepth, sizeof(value));
            words[dstDepth / 4 + i] = value;
        }
    }

    const EdramOwner sourceOwner{c.sourceKind, sourceKey};
    const EdramOwner destOwner{EdramOwnerKind::Depth, destKey};
    Require(BeginFrame(), c.name);
    if (c.sourceKind == EdramOwnerKind::Depth)
    {
        CopyDepthAspect(sourceKey, VK_IMAGE_ASPECT_STENCIL_BIT, buffer, srcStencil, true);
        CopyDepthAspect(sourceKey, VK_IMAGE_ASPECT_DEPTH_BIT, buffer, srcWords, true);
    }
    else
    {
        ColorBacking* source = FindColorBacking(sourceKey);
        Require(source != nullptr, c.name);
        TransitionColorBacking(*source, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy region{};
        region.bufferOffset = srcWords;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {width, height, 1};
        p_vkCmdCopyBufferToImage(g_commandBuffer, buffer.buffer, source->image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }

    const auto runPath = [&](bool compute, VkDeviceSize stencilOut, VkDeviceSize depthOut) {
        CopyDepthAspect(destKey, VK_IMAGE_ASPECT_STENCIL_BIT, buffer, dstStencil, true);
        CopyDepthAspect(destKey, VK_IMAGE_ASPECT_DEPTH_BIT, buffer, dstDepth, true);
        g_edramStencilCopyState = compute ? 1 : -1;
        Require(TransferEdramOwnershipSpan(sourceOwner, destOwner, c.destLocal, c.tiles),
                c.name);
        CopyDepthAspect(destKey, VK_IMAGE_ASPECT_STENCIL_BIT, buffer, stencilOut, false);
        CopyDepthAspect(destKey, VK_IMAGE_ASPECT_DEPTH_BIT, buffer, depthOut, false);
    };
    runPath(false, planesStencil, planesDepth);
    runPath(true, copyStencil, copyDepth);
    SubmitProbeFrame(c.name);

    // The rectangles the transfer may write, in host pixels.
    EdramBackingMeta dest{};
    Require(GetEdramBackingMeta(destOwner, dest), c.name);
    std::vector<uint8_t> inside(size_t(pixels), 0);
    for (const VkRect2D& rect : EdramTransferRects(c.destLocal, c.tiles, dest.pitchTiles,
                                                   EdramTileWidthPixels(dest.guestSamples),
                                                   EdramTileHeightPixels(dest.guestSamples)))
    {
        const VkRect2D host = EdramStencilCopyHostRect(rect);
        for (uint32_t y = 0; y < host.extent.height; ++y)
            for (uint32_t x = 0; x < host.extent.width; ++x)
                inside[size_t(host.offset.y + y) * width + host.offset.x + x] = 1;
    }

    const uint32_t depthMask = destD24 ? 0x00FFFFFFu : 0xFFFFFFFFu;
    uint64_t insideCount = 0, changed = 0;
    for (VkDeviceSize i = 0; i < pixels; ++i)
    {
        const uint8_t planes = bytes[planesStencil + i];
        const uint8_t copy = bytes[copyStencil + i];
        if (planes != copy)
        {
            std::fprintf(stderr, "%s: pixel %llu,%llu stencil bit planes=%02X compute=%02X\n",
                         c.name, static_cast<unsigned long long>(i % width),
                         static_cast<unsigned long long>(i / width), planes, copy);
            Require(false, "the compute copy must write the bit-plane stencil");
        }
        if ((words[planesDepth / 4 + i] & depthMask) != (words[copyDepth / 4 + i] & depthMask))
            Require(false, "the stencil copy must leave the transferred depth unchanged");
        if (inside[size_t(i)])
        {
            ++insideCount;
            changed += copy != bytes[dstStencil + i];
        }
        else if (copy != bytes[dstStencil + i])
        {
            Require(false, "the stencil outside the transferred tiles must stay unchanged");
        }
    }
    Require(insideCount != 0 && changed * 2 > insideCount,
            "the transfer must replace most of the destination stencil");
    std::printf("  ok: %s (scale %u, %llu pixels, %s -> %s)\n", c.name, g_resolutionScale,
                static_cast<unsigned long long>(insideCount),
                c.sourceKind == EdramOwnerKind::Color ? "color" : (sourceD24 ? "D24S8" : "D32S8"),
                destD24 ? "D24S8" : "D32S8");
}

static void CheckEdramStencilCopy(uint32_t scale)
{
    static constexpr StencilCopyCase kCases[] = {
        {"2x depth -> 4x alias depth, all tiles", EdramOwnerKind::Depth,
         0x078101E0u, 0x0001021Cu, 0x03C200F0u, 0x0001021Cu, 0, 540},
        {"4x alias depth -> 2x depth, partial rows", EdramOwnerKind::Depth,
         0x03C200F0u, 0x0001021Cu, 0x078101E0u, 0x0001021Cu, 7, 200},
        {"2x color -> 4x alias unorm depth", EdramOwnerKind::Color,
         0x078101E0u, 0x00000438u, 0x03C200F0u, 0x00000438u, 0, 540},
        {"2x unorm depth -> 4x alias float depth", EdramOwnerKind::Depth,
         0x078101E0u, 0x0000021Cu, 0x03C200F0u, 0x0001021Cu, 13, 401},
        {"1x depth -> 4x alias depth, partial rows", EdramOwnerKind::Depth,
         0x14000500u, 0x000102D0u, 0x0A020280u, 0x000102D0u, 3, 100},
        {"4x alias depth -> 1x depth, partial rows", EdramOwnerKind::Depth,
         0x0A020280u, 0x000102D0u, 0x14000500u, 0x000102D0u, 100, 333},
    };
    {
        mojorecomp::config::RuntimeConfig config{};
        config.resolutionScale = scale;
        mojorecomp::config::Set(config);
        Require(HostWindow_Init(1280, 720, true), "hidden stencil copy window");
        Require(VkPresenter_Init(HostWindow_NativeHandle(), 1280, 720), "stencil copy presenter");
        Require(g_resolutionScale == scale, "resolution scale");
        // Both stencil paths replace stencil export; force them on any device.
        g_shaderStencilExport = false;
        Require(EnsureEdramStencilCopyInfrastructure(), "stencil copy infrastructure");
        ProbeBuffer buffer = CreateProbeBuffer(VkDeviceSize(20) * g_internalExtent.width *
                                               g_internalExtent.height);
        for (const StencilCopyCase& c : kCases)
            CheckStencilCopyCase(c, buffer);
        bool sawCompute = false, sawPlanes = false;
        for (const EdramTransferPipeline& rec : g_edramTransferPipelines)
        {
            sawCompute |= rec.pass == EdramTransferPass::StencilCopyCompute && rec.pipeline;
            sawPlanes |= rec.pass == EdramTransferPass::StencilBitPlanes && rec.pipeline;
        }
        Require(sawCompute && sawPlanes, "both stencil paths must have run");
        DestroyProbeBuffer(buffer);
        VkPresenter_Shutdown();
    }
    std::printf("PASS: the compute stencil copy matches the stencil bit planes exactly "
                "(scale %u)\n", scale);
}

// Which draws FindEdramAliasRedirect sends to the surface owning the tiles.
static void CheckEdramAliasRedirectDecision()
{
    constexpr uint32_t scene2x = 0x078101E0u, alias4x = 0x03C200F0u;
    constexpr uint32_t frame1x = 0x14000500u, frameAlias4x = 0x0A020280u;
    const auto ownerKey = [](uint32_t surface, uint32_t info) {
        return (uint64_t(surface) << 32) | info;
    };
    for (auto& owner : g_edramOwners)
        owner = {};
    g_edramOwners[0x000] = {EdramOwnerKind::Color, ownerKey(scene2x, 0x00000000u)};
    g_edramOwners[0x21C] = {EdramOwnerKind::Depth, ownerKey(scene2x, 0x0001021Cu)};
    g_edramOwners[0x438] = {EdramOwnerKind::Color, ownerKey(scene2x, 0x00000438u)};

    std::vector<uint32_t> regs(0x5000);
    regs[xenos::kRbSurfaceInfo] = alias4x;
    regs[xenos::kRbColorInfo] = 0x00000000u;
    regs[xenos::kRbDepthInfo] = 0x0001021Cu;
    Pm4Draw draw{};
    draw.primType = xenos::kRectangleList;
    ShaderModuleRec ps{};
    EdramAliasRedirect redirect{};
    const auto decide = [&](bool executePs, bool color0, bool depth) {
        redirect = {};
        return FindEdramAliasRedirect(regs.data(), draw, ps, executePs, color0, false, depth,
                                      redirect) == EdramAliasRefusal::None;
    };

    // The scene-tile clear: a color and depth rectangle through the 4x view.
    Require(decide(true, true, true) && redirect.surfaceInfo == scene2x &&
                redirect.scaleX == 2 && redirect.scaleY == 1,
            "a clear through the 4x view of the 2x scene must be redirected 2x1");
    Require(decide(false, false, true), "a depth-only (stencil) clear must be redirected");
    Require(!decide(true, false, false), "a draw touching no attachment must not be redirected");

    ps.usesTextures = true;
    Require(!decide(true, true, true), "a textured pixel shader must not be redirected");
    Require(decide(false, false, true), "an unexecuted textured pixel shader does not matter");
    ps.usesTextures = false;
    ps.writesDepth = true;
    Require(decide(true, true, true),
            "the host float24 depth conversion alone must still be redirected");
    ps.exportsDepth = true;
    Require(!decide(true, true, true), "a guest depth export must not be redirected");
    ps.writesDepth = ps.exportsDepth = false;
    regs[xenos::kSqProgramCntl] = 1u << 18;
    Require(!decide(true, true, true), "the screen-position input must not be redirected");
    regs[xenos::kSqProgramCntl] = 0;
    regs[xenos::kRbColorControl] = 0x10u;
    Require(!decide(true, true, true), "alpha-to-coverage must not be redirected");
    regs[xenos::kRbColorControl] = 0;
    draw.primType = xenos::kTriangleList;
    Require(!decide(true, true, true), "only rectangle lists are redirected");
    draw.primType = xenos::kRectangleList;

    // RT1 cleared through the alias's depth: its tiles hold color, so the
    // depth writes need a real format change.
    regs[xenos::kRbDepthInfo] = 0x00000438u;
    Require(!decide(false, false, true), "depth over a color owner must not be redirected");
    regs[xenos::kRbDepthInfo] = 0x0001021Cu;
    g_edramOwners[0x000] = {EdramOwnerKind::Color, ownerKey(alias4x, 0x00000000u)};
    Require(!decide(true, true, true), "color and depth in different surfaces must not be redirected");
    Require(!decide(true, true, false), "a surface that already owns its tiles is not an alias");
    g_edramOwners[0x000] = {EdramOwnerKind::Color, ownerKey(scene2x, 0x00000000u)};
    regs[xenos::kRbSurfaceInfo] = 0x000001E0u;  // 1x view of the same tiles
    Require(!decide(true, true, true), "a lower MSAA mode is not an alias");
    regs[xenos::kRbSurfaceInfo] = alias4x;

    // The frame's 4x view, as in the 1280x720 post-processing clears.
    g_edramOwners[0x2D0] = {EdramOwnerKind::Depth, ownerKey(frame1x, 0x000102D0u)};
    regs[xenos::kRbSurfaceInfo] = frameAlias4x;
    regs[xenos::kRbDepthInfo] = 0x000102D0u;
    Require(decide(false, false, true) && redirect.surfaceInfo == frame1x &&
                redirect.scaleX == 2 && redirect.scaleY == 2,
            "a clear through the 4x view of the 1x frame must be redirected 2x2");
    for (auto& owner : g_edramOwners)
        owner = {};
    std::puts("PASS: MSAA alias redirect decisions");
}

// The redirect's pixel scale checked against the transfer shaders' EDRAM
// mapping: a constant written into an alias rectangle and transferred back
// must land exactly on the owner rectangle the redirect draws.
static void CheckEdramAliasMapping(uint32_t scale)
{
    mojorecomp::config::RuntimeConfig config{};
    config.resolutionScale = scale;
    mojorecomp::config::Set(config);
    Require(HostWindow_Init(1280, 720, true), "hidden alias window");
    Require(VkPresenter_Init(HostWindow_NativeHandle(), 1280, 720), "alias presenter");
    const uint32_t width = g_internalExtent.width;
    const uint32_t height = g_internalExtent.height;
    const VkDeviceSize pixels = VkDeviceSize(width) * height;
    ProbeBuffer buffer = CreateProbeBuffer(16 * pixels);
    auto* words = reinterpret_cast<uint32_t*>(buffer.mapped);
    constexpr uint32_t kConstant = 0xA5C3E1F0u;
    for (VkDeviceSize i = 0; i < pixels; ++i)
    {
        words[i] = (uint32_t(i) * 0x9E3779B1u) | 1u;  // never kConstant
        words[pixels + i] = kConstant;
    }

    struct AliasCase
    {
        const char* name;
        uint32_t owner;
        uint32_t alias;
        uint32_t tiles;
        VkRect2D aliasRect;
    };
    static constexpr AliasCase kCases[] = {
        {"4x view of the 2x scene", 0x078101E0u, 0x03C200F0u, 540, {{40, 16}, {120, 64}}},
        {"4x view of the 1x frame", 0x14000500u, 0x0A020280u, 720, {{80, 8}, {160, 40}}},
    };
    for (const AliasCase& c : kCases)
    {
        const auto pixelScale = mojorecomp::gpu::EdramAliasPixelScale(
            1u << uint32_t(xenos::SurfaceMsaaSamples(c.alias)),
            xenos::SurfacePitchTiles(c.alias, false),
            1u << uint32_t(xenos::SurfaceMsaaSamples(c.owner)),
            xenos::SurfacePitchTiles(c.owner, false));
        Require(pixelScale.x != 0, c.name);
        ColorBacking* backing = nullptr;
        Require(CreateColorBacking(c.owner, 0, backing) && CreateColorBacking(c.alias, 0, backing),
                c.name);
        const uint64_t ownerKey = ColorSurfaceKey(c.owner, 0);
        const uint64_t aliasKey = ColorSurfaceKey(c.alias, 0);
        const auto copyWords = [&](uint64_t key, VkDeviceSize offset, VkRect2D logical,
                                   bool toImage) {
            ColorBacking* image = FindColorBacking(key);
            Require(image != nullptr, c.name);
            TransitionColorBacking(*image, toImage ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                                                   : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            const VkRect2D host = ScaleRectToInternal(logical);
            VkBufferImageCopy region{};
            region.bufferOffset = offset + (VkDeviceSize(host.offset.y) * width +
                                            VkDeviceSize(host.offset.x)) * 4u;
            region.bufferRowLength = width;
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageOffset = {host.offset.x, host.offset.y, 0};
            region.imageExtent = {host.extent.width, host.extent.height, 1};
            if (toImage)
                p_vkCmdCopyBufferToImage(g_commandBuffer, buffer.buffer, image->image,
                                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            else
                p_vkCmdCopyImageToBuffer(g_commandBuffer, image->image,
                                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer.buffer, 1,
                                         &region);
        };
        const VkRect2D full{{0, 0}, {1280, 720}};
        const VkDeviceSize viaTransfer = 8 * pixels, viaRedirect = 12 * pixels;
        const VkRect2D ownerRect{{c.aliasRect.offset.x * int32_t(pixelScale.x),
                                  c.aliasRect.offset.y * int32_t(pixelScale.y)},
                                 {c.aliasRect.extent.width * pixelScale.x,
                                  c.aliasRect.extent.height * pixelScale.y}};

        Require(BeginFrame(), c.name);
        // Through the alias: owner -> alias, constant rectangle, alias -> owner.
        copyWords(ownerKey, 0, full, true);
        Require(TransferEdramOwnershipSpan({EdramOwnerKind::Color, ownerKey},
                                           {EdramOwnerKind::Color, aliasKey}, 0, c.tiles),
                c.name);
        copyWords(aliasKey, 4 * pixels, c.aliasRect, true);
        Require(TransferEdramOwnershipSpan({EdramOwnerKind::Color, aliasKey},
                                           {EdramOwnerKind::Color, ownerKey}, 0, c.tiles),
                c.name);
        copyWords(ownerKey, viaTransfer, full, false);
        // Redirected: the constant rectangle written straight into the owner.
        copyWords(ownerKey, 0, full, true);
        copyWords(ownerKey, 4 * pixels, ownerRect, true);
        copyWords(ownerKey, viaRedirect, full, false);
        SubmitProbeFrame(c.name);

        uint64_t constantPixels = 0;
        for (VkDeviceSize i = 0; i < pixels; ++i)
        {
            const bool transferred = words[viaTransfer / 4 + i] == kConstant;
            const bool redirected = words[viaRedirect / 4 + i] == kConstant;
            if (transferred != redirected)
            {
                std::fprintf(stderr, "%s: pixel %llu,%llu transfer=%d redirect=%d\n", c.name,
                             static_cast<unsigned long long>(i % width),
                             static_cast<unsigned long long>(i / width), int(transferred),
                             int(redirected));
                Require(false, "the redirect must write the samples the alias writes");
            }
            constantPixels += redirected;
        }
        Require(constantPixels == VkDeviceSize(ownerRect.extent.width) * ownerRect.extent.height *
                                      scale * scale,
                "the redirected rectangle must cover exactly the scaled alias rectangle");
        std::printf("  ok: %s (scale %u, alias %ux%u px -> owner %ux%u px)\n", c.name, scale,
                    c.aliasRect.extent.width, c.aliasRect.extent.height,
                    ownerRect.extent.width, ownerRect.extent.height);
    }
    DestroyProbeBuffer(buffer);
    VkPresenter_Shutdown();
    std::printf("PASS: MSAA alias pixels map to the scaled owner rectangle (scale %u)\n", scale);
}

// The transfer shaders' Float32To20e4, for the probe's expected values.
static uint32_t Float32To20e4ForProbe(float value)
{
    if (!(value > 0.0f))
        return 0u;
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    if (bits >= 0x3FFFFFF8u)
        return 0xFFFFFFu;
    if (bits < 0x38800000u)
    {
        const uint32_t shift = std::min(113u - (bits >> 23), 24u);
        bits = (0x800000u | (bits & 0x7FFFFFu)) >> shift;
    }
    else
    {
        bits += 0xC8000000u;
    }
    bits += 3u + ((bits >> 3) & 1u);
    return (bits >> 3) & 0xFFFFFFu;
}

// Which depth-only rectangles FindEdramDepthAsColorClear draws as color.
static void CheckEdramDepthAsColorDecision()
{
    for (auto& owner : g_edramOwners)
        owner = {};
    g_edramOwners[0x438] = {EdramOwnerKind::Color, (uint64_t(0x078101E0u) << 32) | 0x438u};
    std::vector<uint32_t> regs(0x5000);
    regs[xenos::kRbSurfaceInfo] = 0x03C200F0u;
    regs[xenos::kRbDepthInfo] = 0x00000438u;
    regs[xenos::kRbStencilRefMask] = 0x00FF005Au;
    Pm4Draw draw{};
    draw.primType = xenos::kRectangleList;
    EdramDepthAsColorClear clear{};
    const auto decide = [&](uint32_t mode, uint32_t depthControl) {
        clear = {};
        return FindEdramDepthAsColorClear(regs.data(), draw, mode, depthControl, clear);
    };
    // COT's RT1 clear: depth and stencil REPLACE, both ALWAYS.
    Require(decide(5, 0x8777u) && clear.surfaceInfo == 0x078101E0u && clear.colorInfo == 0x438u &&
                clear.scaleX == 2 && clear.scaleY == 1 && clear.colorMask == 0xFu &&
                clear.stencil == 0x5Au && !clear.float24 && clear.tileWidthPixels == 80,
            "the RT1 depth clear through the 4x view must be drawn as color");
    Require(decide(5, 0x0076u) && clear.colorMask == 0xEu, "a depth-only write keeps R");
    Require(decide(5, 0x8701u) && clear.colorMask == 0x1u, "a stencil-only write keeps GBA");
    Require(decide(5, 0x4701u) && clear.stencil == 0, "stencil ZERO writes 0");
    Require(!decide(4, 0x8777u), "a draw that also writes color is not a depth clear");
    Require(!decide(5, 0x8767u), "a depth test other than ALWAYS reads the old depth");
    Require(!decide(5, 0xC777u), "stencil INCR depends on the old stencil");
    Require(!decide(5, 0x8677u), "a stencil test other than ALWAYS reads the old stencil");
    Require(!decide(5, 0x0002u), "a draw writing nothing is left alone");
    regs[xenos::kRbStencilRefMask] = 0x000F005Au;
    Require(!decide(5, 0x8701u), "a partial stencil write mask keeps old bits");
    regs[xenos::kRbStencilRefMask] = 0x00FF005Au;
    regs[xenos::kRbDepthInfo] = 0x00010438u;
    Require(decide(5, 0x8777u) && clear.float24, "a float24 depth clear is flagged");
    regs[xenos::kRbDepthInfo] = 0x00000438u;
    draw.primType = xenos::kTriangleList;
    Require(!decide(5, 0x8777u), "only rectangle lists are drawn as color");
    draw.primType = xenos::kRectangleList;
    g_edramOwners[0x438] = {EdramOwnerKind::Depth, (uint64_t(0x078101E0u) << 32) | 0x438u};
    Require(!decide(5, 0x8777u), "a depth owner is the alias redirect's case");
    for (auto& owner : g_edramOwners)
        owner = {};
    std::puts("PASS: depth-as-color clear decisions");
}

// A depth/stencil clear through the 4x view over a 2x color owner, the old
// way (color -> depth view by transfer, clear the rectangle, transfer back)
// and drawn as color in two passes, must leave identical color words.
static void CheckEdramDepthAsColor(uint32_t scale)
{
    mojorecomp::config::RuntimeConfig config{};
    config.resolutionScale = scale;
    mojorecomp::config::Set(config);
    Require(HostWindow_Init(1280, 720, true), "hidden depth-as-color window");
    Require(VkPresenter_Init(HostWindow_NativeHandle(), 1280, 720), "depth-as-color presenter");
    g_shaderStencilExport = false;
    const uint32_t width = g_internalExtent.width;
    const uint32_t height = g_internalExtent.height;
    const VkDeviceSize pixels = VkDeviceSize(width) * height;
    ProbeBuffer buffer = CreateProbeBuffer(12 * pixels);
    auto* words = reinterpret_cast<uint32_t*>(buffer.mapped);
    for (VkDeviceSize i = 0; i < pixels; ++i)
        words[i] = ((uint32_t(i) * 0x9E3779B1u) & 0x3F3F3F3Fu) | 0x40404040u;
    const VkDeviceSize oldWay = 4 * pixels, asColor = 8 * pixels;

    constexpr uint32_t ownerSurface = 0x078101E0u, aliasSurface = 0x03C200F0u;
    ColorBacking* colorBacking = nullptr;
    Require(CreateColorBacking(ownerSurface, 0x438u, colorBacking), "owner color");
    const uint64_t ownerKey = ColorSurfaceKey(ownerSurface, 0x438u);
    const auto copyOwner = [&](VkDeviceSize offset, bool toImage) {
        ColorBacking* image = FindColorBacking(ownerKey);
        TransitionColorBacking(*image, toImage ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                                               : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy region{};
        region.bufferOffset = offset;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {width, height, 1};
        if (toImage)
            p_vkCmdCopyBufferToImage(g_commandBuffer, buffer.buffer, image->image,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        else
            p_vkCmdCopyImageToBuffer(g_commandBuffer, image->image,
                                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer.buffer, 1,
                                     &region);
    };

    // A full-screen triangle at a constant depth for the color passes.
    static constexpr float kDepth = 0.625f;  // exact in float32 and unambiguous in D24
    std::vector<uint8_t> vsSpv;
    std::string err;
    Require(ShaderTranslator::CompileHostHlsl(R"(
float4 main(uint id : SV_VertexID) : SV_Position
{
    float2 p = id == 0 ? float2(-1.0, -1.0) : id == 1 ? float2(-1.0, 3.0) : float2(3.0, -1.0);
    return float4(p, 0.625, 1.0);
}
)", true, vsSpv, err), "depth-as-color probe VS");
    VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = vsSpv.size();
    moduleInfo.pCode = reinterpret_cast<const uint32_t*>(vsSpv.data());
    VkShaderModule vsModule = VK_NULL_HANDLE;
    Require(p_vkCreateShaderModule(g_device, &moduleInfo, nullptr, &vsModule) == VK_SUCCESS,
            "probe VS module");
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    Require(p_vkCreatePipelineLayout(g_device, &layoutInfo, nullptr, &layout) == VK_SUCCESS,
            "probe layout");
    std::vector<VkPipeline> pipelines;
    const auto makePipeline = [&](VkShaderModule ps, uint32_t colorMask) {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                     VK_SHADER_STAGE_VERTEX_BIT, vsModule, "main", nullptr};
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                     VK_SHADER_STAGE_FRAGMENT_BIT, ps, "main", nullptr};
        VkPipelineVertexInputStateCreateInfo vertex{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = colorMask;
        VkPipelineColorBlendStateCreateInfo blendState{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blendState.attachmentCount = 1;
        blendState.pAttachments = &blend;
        const VkDynamicState dynamics[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamics;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &g_swapFormat;
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        ci.pNext = &rendering;
        ci.stageCount = 2;
        ci.pStages = stages;
        ci.pVertexInputState = &vertex;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &raster;
        ci.pMultisampleState = &ms;
        ci.pColorBlendState = &blendState;
        ci.pDynamicState = &dynamic;
        ci.layout = layout;
        VkPipeline pipeline = VK_NULL_HANDLE;
        Require(p_vkCreateGraphicsPipelines(g_device, VK_NULL_HANDLE, 1, &ci, nullptr,
                                            &pipeline) == VK_SUCCESS,
                "probe depth-as-color pipeline");
        pipelines.push_back(pipeline);
        return pipeline;
    };

    struct DepthAsColorCase
    {
        const char* name;
        uint32_t depthInfo;
        VkRect2D aliasRect;  // 4x view pixels
        bool writesDepth;
        bool writesStencil;
        uint32_t stencil;
    };
    static constexpr DepthAsColorCase kCases[] = {
        {"COT RT1 clear (tile-aligned, depth + stencil)", 0x00000438u, {{0, 0}, {200, 720}},
         true, true, 0xA5u},
        {"partial tiles, stencil only", 0x00000438u, {{10, 8}, {120, 64}}, false, true, 0xC3u},
        {"partial tiles, float24 depth only", 0x00010438u, {{25, 16}, {70, 24}}, true, false, 0u},
    };
    for (const DepthAsColorCase& c : kCases)
    {
        DepthBacking* depthBacking = nullptr;
        const uint64_t aliasKey = (uint64_t(aliasSurface) << 32) | c.depthInfo;
        Require(CreateDepthBacking(aliasKey, depthBacking), c.name);

        Require(BeginFrame(), c.name);
        // The old way.
        copyOwner(0, true);
        Require(TransferEdramOwnershipSpan({EdramOwnerKind::Color, ownerKey},
                                           {EdramOwnerKind::Depth, aliasKey}, 0, 540),
                c.name);
        DepthBacking* depth = FindDepthBacking(aliasKey);
        TransitionDepthBacking(*depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo depthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depthAttachment.imageView = depth->view;
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, g_internalExtent};
        rendering.layerCount = 1;
        rendering.pDepthAttachment = &depthAttachment;
        rendering.pStencilAttachment = &depthAttachment;
        p_vkCmdBeginRendering(g_commandBuffer, &rendering);
        VkClearAttachment clearAttachment{};
        clearAttachment.aspectMask = (c.writesDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0u) |
                                     (c.writesStencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u);
        clearAttachment.clearValue.depthStencil = {kDepth, c.stencil};
        const VkClearRect clearRect{ScaleRectToInternal(c.aliasRect), 0, 1};
        p_vkCmdClearAttachments(g_commandBuffer, 1, &clearAttachment, 1, &clearRect);
        p_vkCmdEndRendering(g_commandBuffer);
        Require(TransferEdramOwnershipSpan({EdramOwnerKind::Depth, aliasKey},
                                           {EdramOwnerKind::Color, ownerKey}, 0, 540),
                c.name);
        copyOwner(oldWay, false);

        // Drawn as color: the passes the draw path uses.
        copyOwner(0, true);
        EdramDepthAsColorClear clear{};
        clear.scaleX = 2;
        clear.scaleY = 1;
        clear.colorMask = (c.writesStencil ? 0x1u : 0u) | (c.writesDepth ? 0xEu : 0u);
        clear.stencil = c.stencil;
        clear.float24 = ((c.depthInfo >> 16) & 1u) != 0;
        clear.tileWidthPixels = xenos::EdramTileWidthPixels(ownerSurface, false);
        ColorBacking* owner = FindColorBacking(ownerKey);
        TransitionColorBacking(*owner, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo colorAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        colorAttachment.imageView = owner->view;
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo colorRendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        colorRendering.renderArea = {{0, 0}, g_internalExtent};
        colorRendering.layerCount = 1;
        colorRendering.colorAttachmentCount = 1;
        colorRendering.pColorAttachments = &colorAttachment;
        p_vkCmdBeginRendering(g_commandBuffer, &colorRendering);
        const VkRect2D ownerRect{{c.aliasRect.offset.x * 2, c.aliasRect.offset.y},
                                 {c.aliasRect.extent.width * 2, c.aliasRect.extent.height}};
        for (const EdramDepthAsColorPass& pass : EdramDepthAsColorPasses(clear))
        {
            ShaderModuleRec* shader = GetEdramDepthAsColorShader(clear, pass.secondHalf);
            Require(shader != nullptr, "depth-as-color shader");
            uint32_t writeMask = 0;
            if (clear.colorMask & 1u) writeMask |= VK_COLOR_COMPONENT_R_BIT;
            if (clear.colorMask & 2u) writeMask |= VK_COLOR_COMPONENT_G_BIT;
            if (clear.colorMask & 4u) writeMask |= VK_COLOR_COMPONENT_B_BIT;
            if (clear.colorMask & 8u) writeMask |= VK_COLOR_COMPONENT_A_BIT;
            p_vkCmdBindPipeline(g_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                makePipeline(shader->module, writeMask));
            VkViewport viewport{float(pass.shiftPixels) * float(scale), 0.0f, float(width),
                                float(height), 0.0f, 1.0f};
            p_vkCmdSetViewport(g_commandBuffer, 0, 1, &viewport);
            const int32_t x0 = std::clamp(ownerRect.offset.x + pass.shiftPixels, 0, 1280);
            const int32_t x1 = std::clamp(ownerRect.offset.x + int32_t(ownerRect.extent.width) +
                                              pass.shiftPixels, 0, 1280);
            if (x1 <= x0)
                continue;
            const VkRect2D scissor = ScaleRectToInternal(
                {{x0, ownerRect.offset.y}, {uint32_t(x1 - x0), ownerRect.extent.height}});
            p_vkCmdSetScissor(g_commandBuffer, 0, 1, &scissor);
            p_vkCmdDraw(g_commandBuffer, 3, 1, 0, 0);
        }
        p_vkCmdEndRendering(g_commandBuffer);
        copyOwner(asColor, false);
        SubmitProbeFrame(c.name);

        // The word the clear writes: stencil in byte 0, the 24-bit depth
        // above it. Background bytes are 0x40..0x7F and no written byte is,
        // so a pixel is inside the clear exactly when its written bytes match.
        // Outside it the old way is not a reference: its round trip through
        // the 4x view keeps one sample per 4x pixel.
        const uint32_t d24 = clear.float24 ? Float32To20e4ForProbe(kDepth * 2.0f)
                                           : uint32_t(std::lround(kDepth * 16777215.0));
        // Channels R, G, B, A of the word; in memory BGRA images hold R in byte 2.
        const uint32_t channels = c.stencil | (d24 << 8);
        const bool bgra = g_swapFormat == VK_FORMAT_B8G8R8A8_UNORM;
        uint32_t word = 0, byteMask = 0;
        for (uint32_t channel = 0; channel < 4; ++channel)
        {
            const uint32_t byte = bgra && channel != 3 ? 2u - channel : channel;
            const uint32_t value = (channels >> (channel * 8)) & 0xFFu;
            word |= value << (byte * 8);
            if (clear.colorMask & (1u << channel))
            {
                byteMask |= 0xFFu << (byte * 8);
                Require(value < 0x40u || value > 0x7Fu, "probe clear bytes must differ from the background");
            }
        }
        uint64_t inside = 0;
        for (VkDeviceSize i = 0; i < pixels; ++i)
        {
            const uint32_t expected = words[oldWay / 4 + i];
            const uint32_t actual = words[asColor / 4 + i];
            const bool clearedOldWay = (expected & byteMask) == (word & byteMask);
            const bool clearedAsColor = (actual & byteMask) == (word & byteMask);
            const bool untouched = actual == words[i];
            if (clearedOldWay != clearedAsColor ||
                (clearedAsColor && (actual & ~byteMask) != (words[i] & ~byteMask)) ||
                (!clearedAsColor && !untouched))
            {
                std::fprintf(stderr, "%s: pixel %llu,%llu old way %08X, as color %08X, before %08X\n",
                             c.name, static_cast<unsigned long long>(i % width),
                             static_cast<unsigned long long>(i / width), expected, actual,
                             words[i]);
                Require(false, "the color passes must write what the depth clear writes");
            }
            inside += clearedAsColor;
        }
        Require(inside == VkDeviceSize(c.aliasRect.extent.width) * 2u * c.aliasRect.extent.height *
                              scale * scale,
                "the clear must cover the scaled rectangle's area");
        const uint64_t changed = inside;
        std::printf("  ok: %s (scale %u, %llu pixels changed)\n", c.name, scale,
                    static_cast<unsigned long long>(changed));
    }
    for (VkPipeline pipeline : pipelines)
        p_vkDestroyPipeline(g_device, pipeline, nullptr);
    p_vkDestroyPipelineLayout(g_device, layout, nullptr);
    p_vkDestroyShaderModule(g_device, vsModule, nullptr);
    DestroyProbeBuffer(buffer);
    VkPresenter_Shutdown();
    std::printf("PASS: depth clears drawn as color match the transfer path (scale %u)\n", scale);
}

// Informational (not a CTest): GPU time of a 540-tile depth transfer at scale 2,
// with the stencil bit planes and with the compute stencil copy.
static void BenchEdramStencilCopy()
{
    mojorecomp::config::RuntimeConfig config{};
    config.resolutionScale = 2;
    mojorecomp::config::Set(config);
    Require(HostWindow_Init(1280, 720, true), "bench window");
    Require(VkPresenter_Init(HostWindow_NativeHandle(), 1280, 720), "bench presenter");
    g_shaderStencilExport = false;
    Require(EnsureEdramStencilCopyInfrastructure(), "bench infrastructure");
    const uint64_t key2x = (uint64_t(0x078101E0u) << 32) | 0x0001021Cu;
    const uint64_t key4x = (uint64_t(0x03C200F0u) << 32) | 0x0001021Cu;
    DepthBacking* backing = nullptr;
    Require(CreateDepthBacking(key2x, backing) && CreateDepthBacking(key4x, backing), "bench backings");
    const EdramOwner owner2x{EdramOwnerKind::Depth, key2x};
    const EdramOwner owner4x{EdramOwnerKind::Depth, key4x};

    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = 4;
    VkQueryPool pool = VK_NULL_HANDLE;
    Require(p_vkCreateQueryPool(g_device, &qi, nullptr, &pool) == VK_SUCCESS, "bench queries");
    constexpr uint32_t kRounds = 16;
    const char* names[2] = {"stencil bit planes", "compute stencil copy"};
    for (uint32_t pass = 0; pass < 2; ++pass)  // pass 0 warms pipelines and caches
    {
        Require(BeginFrame(), "bench frame");
        p_vkCmdResetQueryPool(g_commandBuffer, pool, 0, 4);
        for (uint32_t mode = 0; mode < 2; ++mode)
        {
            g_edramStencilCopyState = mode == 0 ? -1 : 1;
            p_vkCmdWriteTimestamp(g_commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool,
                                  mode * 2);
            for (uint32_t i = 0; i < kRounds; ++i)
            {
                Require(TransferEdramOwnershipSpan(owner2x, owner4x, 0, 540), "bench 2x->4x");
                Require(TransferEdramOwnershipSpan(owner4x, owner2x, 0, 540), "bench 4x->2x");
            }
            p_vkCmdWriteTimestamp(g_commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool,
                                  mode * 2 + 1);
        }
        SubmitProbeFrame("bench submit");
        uint64_t ticks[4]{};
        Require(p_vkGetQueryPoolResults(g_device, pool, 0, 4, sizeof(ticks), ticks,
                                        sizeof(uint64_t),
                                        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) ==
                    VK_SUCCESS,
                "bench results");
        if (pass == 0)
            continue;
        for (uint32_t mode = 0; mode < 2; ++mode)
            std::printf("  %-24s %7.1f us per depth transfer\n", names[mode],
                        double(ticks[mode * 2 + 1] - ticks[mode * 2]) * g_gpuTimestampPeriodNs /
                            1000.0 / (2.0 * kRounds));
    }
    p_vkDestroyQueryPool(g_device, pool, nullptr);
    VkPresenter_Shutdown();
}

static void CheckPixels(bool dummy, bool rectangle = false, bool tiledResolve = false)
{
    EndColorRendering();
    ColorBacking* backing = ActiveColorBacking();
    Require(backing != nullptr, "active color backing for readback");
    TransitionColorBacking(*backing, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.bufferOffset = g_readbackOffset;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {g_extent.width, g_extent.height, 1};
    p_vkCmdCopyImageToBuffer(g_commandBuffer, backing->image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_uploadBuffer, 1, &copy);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    p_vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
    Require(p_vkEndCommandBuffer(g_commandBuffer) == VK_SUCCESS, "end probe commands");
    Require(p_vkResetFences(g_device, 1, &g_fence) == VK_SUCCESS, "reset probe fence");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &g_commandBuffer;
    Require(p_vkQueueSubmit(g_queue, 1, &submit, g_fence) == VK_SUCCESS, "submit probe");
    Require(p_vkWaitForFences(g_device, 1, &g_fence, VK_TRUE, 10000000000ull) == VK_SUCCESS, "wait probe");
    g_frameOpen = false;
    size_t checked = 0;
    for (uint32_t y = 16; y < g_extent.height - 16; y += 32)
        for (uint32_t x = 16; x < g_extent.width - 16; x += 32)
        {
            const uint8_t* p = g_uploadMapped + g_readbackOffset + (size_t(y) * g_extent.width + x) * 4;
            const bool red = g_swapFormat == VK_FORMAT_B8G8R8A8_UNORM ?
                p[0] == 0 && p[1] == 0 && p[2] == 255 :
                p[0] == 255 && p[1] == 0 && p[2] == 0;
            const bool zero = p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 0;
            const bool blue = g_swapFormat == VK_FORMAT_B8G8R8A8_UNORM ?
                p[0] == 255 && p[1] == 0 && p[2] == 0 :
                p[0] == 0 && p[1] == 0 && p[2] == 255;
            const bool green = p[0] == 0 && p[1] == 255 && p[2] == 0;
            const bool resolved = tiledResolve ? (x < 416 ? red : x < 832 ? green : blue) : red;
            const bool outside = rectangle && (x < 128 || x >= 1152 || y < 96 || y >= 624);
            if (!(outside ? blue && p[3] == 255 : dummy ? zero : resolved && p[3] == 255))
            {
                std::fprintf(stderr, "pixel %u,%u = %u,%u,%u,%u dummy=%u\n",
                    x, y, p[0], p[1], p[2], p[3], dummy);
                throw std::runtime_error(rectangle ? "rectangle coverage readback mismatch" :
                                                    "translated texture readback mismatch");
            }
            ++checked;
        }
    std::printf("PASS: %zu sampled pixels match %s\n", checked, dummy ? "dummy negative control" :
        tiledResolve ? "three distinct resolved tiles" : "resolved red snapshot");
}

// Reserve the guest address space, committing only this synthetic vertex page.
// No game files or runtime CPU state are needed by the raster regression.
struct SyntheticGuest
{
    uint8_t* base = nullptr;
    ~SyntheticGuest() { if (base) VirtualFree(base, 0, MEM_RELEASE); }
    void Commit(uint32_t va, size_t bytes, const char* message)
    {
        Require(VirtualAlloc(base + va, bytes, MEM_COMMIT, PAGE_READWRITE) != nullptr,
                message);
    }
    void Init()
    {
        base = static_cast<uint8_t*>(VirtualAlloc(nullptr, 1ull << 32, MEM_RESERVE, PAGE_NOACCESS));
        Require(base != nullptr, "reserve synthetic guest");
        Commit(0xA0001000u, 4096, "commit synthetic vertex page");
    }
};

static void CheckAuthoredMipUpload(SyntheticGuest& guest)
{
    using namespace mojorecomp::texture_abi;
    constexpr uint32_t basePhysical = 0x01000000u;
    constexpr uint32_t mipPhysical = 0x01200000u;
    constexpr uint32_t baseVa = 0xA1000000u;
    constexpr uint32_t mipVa = 0xA1200000u;
    constexpr uint32_t mipLevels = 7u;
    guest.Commit(baseVa, 0x100000u, "commit synthetic mip base");
    guest.Commit(mipVa, 0x60000u, "commit synthetic mip backing");
    std::memset(guest.base + baseVa, 0xCD, 0x100000u);
    std::memset(guest.base + mipVa, 0xCD, 0x60000u);

    Fetch2D fetch{};
    fetch.key = basePhysical;
    fetch.mipKey = mipPhysical;
    fetch.width = 512;
    fetch.height = 512;
    fetch.pitch = 512;
    fetch.format = 6;
    fetch.dimension = 1;
    fetch.type = 2;
    fetch.swizzle = 0x60A;
    fetch.endian = 2;
    fetch.mipMax = mipLevels - 1u;
    fetch.packedMips = true;

    std::array<LinearRgba8MipLayout, 16> layout{};
    uint32_t count = 0;
    bool authored = false;
    Require(BuildLinearRgba8MipLayout(fetch, mipLevels, layout, count, authored) &&
            authored && count == mipLevels, "build synthetic authored mip layout");

    // Use a unique RGBA value per level. Guest endian=8-in-32 means the raw
    // bytes are written reversed so the Vulkan image should contain R,G,B,A.
    for (uint32_t level = 0; level < count; ++level)
    {
        const auto& mip = layout[level];
        const uint8_t r = uint8_t(0x10u + level);
        const uint8_t g = uint8_t(0x40u + level);
        const uint8_t b = uint8_t(0x70u + level);
        const uint8_t a = uint8_t(0xA0u + level);
        const uint32_t backing = mip.mipBacking ? mipVa : baseVa;
        for (uint32_t y = 0; y < mip.height; ++y)
            for (uint32_t x = 0; x < mip.width; ++x)
            {
                uint8_t* pixel = guest.base + backing + mip.byteOffset +
                    (uint64_t(mip.offsetY + y) * mip.pitchPixels + mip.offsetX + x) * 4u;
                pixel[0] = a;
                pixel[1] = b;
                pixel[2] = g;
                pixel[3] = r;
            }
    }

    Require(BeginFrame(), "begin authored mip frame");
    GuestTexture* texture = CreateGuestRGBA8Texture(guest.base, fetch);
    Require(texture != nullptr && texture->image != VK_NULL_HANDLE,
            "create authored mip guest texture");
    if (g_rendering)
        EndColorRendering();

    VkImageMemoryBarrier toReadback{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toReadback.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toReadback.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toReadback.oldLayout = texture->layout;
    toReadback.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toReadback.srcQueueFamilyIndex = toReadback.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toReadback.image = texture->image;
    toReadback.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, 1};
    p_vkCmdPipelineBarrier(g_commandBuffer,
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toReadback);

    std::array<VkBufferImageCopy, mipLevels> copies{};
    std::array<size_t, mipLevels> offsets{};
    size_t readbackBytes = 0;
    for (uint32_t level = 0; level < mipLevels; ++level)
    {
        offsets[level] = readbackBytes;
        copies[level].bufferOffset = g_readbackOffset + readbackBytes;
        copies[level].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        copies[level].imageExtent = {layout[level].width, layout[level].height, 1};
        readbackBytes += size_t(layout[level].width) * layout[level].height * 4u;
    }
    Require(readbackBytes <= g_readbackBytes, "authored mip readback fits staging buffer");
    p_vkCmdCopyImageToBuffer(g_commandBuffer, texture->image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_uploadBuffer,
        static_cast<uint32_t>(copies.size()), copies.data());

    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    p_vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
    Require(p_vkEndCommandBuffer(g_commandBuffer) == VK_SUCCESS,
            "end authored mip commands");
    Require(p_vkResetFences(g_device, 1, &g_fence) == VK_SUCCESS,
            "reset authored mip fence");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &g_commandBuffer;
    Require(p_vkQueueSubmit(g_queue, 1, &submit, g_fence) == VK_SUCCESS,
            "submit authored mip probe");
    Require(p_vkWaitForFences(g_device, 1, &g_fence, VK_TRUE, 10000000000ull) == VK_SUCCESS,
            "wait authored mip probe");
    g_frameOpen = false;

    for (uint32_t level = 0; level < mipLevels; ++level)
    {
        const uint8_t expected[4] = {
            uint8_t(0x10u + level), uint8_t(0x40u + level),
            uint8_t(0x70u + level), uint8_t(0xA0u + level)};
        const size_t pixels = size_t(layout[level].width) * layout[level].height;
        const uint8_t* data = g_uploadMapped + g_readbackOffset + offsets[level];
        for (size_t i = 0; i < pixels; ++i)
        {
            const uint8_t* pixel = data + i * 4u;
            if (std::memcmp(pixel, expected, sizeof(expected)) != 0)
            {
                std::fprintf(stderr,
                    "authored mip %u pixel %zu = %u,%u,%u,%u expected=%u,%u,%u,%u\n",
                    level, i, pixel[0], pixel[1], pixel[2], pixel[3],
                    expected[0], expected[1], expected[2], expected[3]);
                throw std::runtime_error("authored mip GPU readback mismatch");
            }
        }
    }
    std::puts("PASS: authored RGBA8 mip chain including packed tail matches GPU readback");
}

static void CheckResolveRegions()
{
    std::vector<uint32_t> regs(0x5000);
    regs[xenos::kPaSuScModeCntl] = 1u << 16;
    regs[xenos::kPaScWindowOffset] = (uint32_t(-64) & 0x7FFFu) |
                                   ((uint32_t(-32) & 0x7FFFu) << 16);
    VkImageCopy copy{};
    Require(ResolveCopyRegion(regs.data(), 64, 32, 320, 160, copy) &&
        copy.srcOffset.x == 0 && copy.srcOffset.y == 0 &&
        copy.dstOffset.x == 64 && copy.dstOffset.y == 32 &&
        copy.extent.width == 256 && copy.extent.height == 128,
        "arbitrary horizontal/vertical resolve offset");
    Require(ResolveCopyRegion(regs.data(), 0, 0, 128, 64, copy) &&
        copy.srcOffset.x == 0 && copy.srcOffset.y == 0 &&
        copy.dstOffset.x == 64 && copy.dstOffset.y == 32 &&
        copy.extent.width == 64 && copy.extent.height == 32,
        "clip source and destination together");
    Require(!ResolveCopyRegion(regs.data(), 0, 0, 32, 16, copy), "empty local source");
    regs[xenos::kPaSuScModeCntl] = 0;
    Require(ResolveCopyRegion(regs.data(), 64, 32, 320, 160, copy) &&
        copy.srcOffset.x == 64 && copy.srcOffset.y == 32,
        "disabled vertex window offset");
    regs[xenos::kPaSuScModeCntl] = 1u << 16;
    regs[xenos::kPaScWindowOffset] = 16 | (8 << 16);
    Require(ResolveCopyRegion(regs.data(), 1200, 680, 1280, 720, copy) &&
        copy.srcOffset.x == 1216 && copy.srcOffset.y == 688 &&
        copy.extent.width == 64 && copy.extent.height == 32,
        "positive offset clips at physical source edge");
}

static void CheckResolveVertexCoordinates()
{
    SyntheticGuest guest;
    guest.Init();
    g_extent = {1280, 720};
    std::vector<uint32_t> regs(0x5000);
    regs[xenos::kFetchConstantBase] = 0x1000u | 3u;
    regs[xenos::kFetchConstantBase + 1] = (6u << 2) | 2u;
    regs[xenos::kPaSuScModeCntl] = 1u << 16;
    regs[xenos::kPaSuVtxCntl] = 1u;
    regs[xenos::kPaScScreenScissorBr] = 1280u | (720u << 16);

    // Recorded resolve shapes, with synthetic vertices and no title assets.
    // Expected physical clipping follows Xenia draw_util::GetResolveInfo:
    // vertex window offset -> scissor -> eight-pixel alignment. The presenter
    // then exposes logical coordinates to its existing snapshot-copy API.
    for (uint32_t source : {0u, 1u, 4u})
    {
        for (uint32_t tile = 0; tile < 3; ++tile)
        {
            const uint32_t left = tile * 416;
            const float vx0 = source == 0 ? float(left) : 0.0f;
            const float vx1 = source == 0 ? float(left + 448)
                                        : source == 1 ? 1280.0f : 448.0f;
            const float vertices[] = {vx0, 0, vx1, 0, vx1, 720};
            CopySwapped(guest.base + 0xA0001000u,
                        reinterpret_cast<const uint8_t*>(vertices), sizeof(vertices), 2);
            regs[xenos::kRbCopyControl] = source;
            regs[xenos::kPaScWindowScissorTl] = left;
            regs[xenos::kPaScWindowScissorBr] = (left + 448) | (720u << 16);
            regs[xenos::kPaScWindowOffset] = (0u - left) & 0x7FFFu;
            uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            const auto result = DecodeResolveRectFromVertices(
                guest.base, regs.data(), 1280, 720, x0, y0, x1, y1);
            if (source == 4 && tile == 2)
            {
                Require(result == ResolveRectDecode::Empty, "third depth rectangle is empty");
                std::printf("PASS: resolve source=%u tile=%u empty\n", source, tile);
                continue;
            }
            const uint32_t expectedRight = source == 4 ? 448 : left + 448;
            Require(result == ResolveRectDecode::Valid && x0 == left && x1 == expectedRight &&
                    y0 == 0 && y1 == 720, "recorded resolve logical rectangle");
            VkImageCopy copy{};
            Require(ResolveCopyRegion(regs.data(), x0, y0, x1, y1, copy) &&
                    copy.srcOffset.x == 0 && copy.dstOffset.x == int32_t(left) &&
                    copy.extent.width == expectedRight - left,
                    "recorded resolve physical source rectangle");
            std::printf("PASS: resolve source=%u tile=%u logical=%u..%u physical=0..%u\n",
                        source, tile, x0, x1, copy.extent.width);
        }
    }

    // A physical screen scissor is intentionally not in logical tile space.
    const float vertices[] = {416, 0, 864, 0, 864, 720};
    CopySwapped(guest.base + 0xA0001000u,
                reinterpret_cast<const uint8_t*>(vertices), sizeof(vertices), 2);
    regs[xenos::kRbCopyControl] = 0;
    regs[xenos::kPaScWindowScissorTl] = 416;
    regs[xenos::kPaScWindowScissorBr] = 864 | (720u << 16);
    regs[xenos::kPaScWindowOffset] = uint32_t(-416) & 0x7FFFu;
    regs[xenos::kPaScScreenScissorTl] = 64;
    regs[xenos::kPaScScreenScissorBr] = 128 | (720u << 16);
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    Require(DecodeResolveRectFromVertices(guest.base, regs.data(), 1280, 720,
                x0, y0, x1, y1) == ResolveRectDecode::Valid && x0 == 480 && x1 == 544,
            "physical screen scissor maps to logical destination");
    std::puts("PASS: physical screen scissor 64..128 maps to logical 480..544");

    // Crash's depth sweep uses a local 0..448 resolve rectangle for every
    // physical EDRAM replay, while RB_COPY_DEST_BASE advances by 32x32 tiled
    // macrotiles. Keep the generic Xenia rectangle decode above untouched, but
    // verify that the host-EDRAM reconstruction layer can join tile 1/2 onto a
    // depth snapshot that tile 0 already established in the same frame.
    g_snapshots.clear();
    constexpr uint32_t depthBase = 0x0A2A5000u;
    const uint64_t resolveFrame = g_frames.load(std::memory_order_relaxed) + 1;
    ResolveSnapshot seed{};
    seed.key = depthBase;
    seed.width = 1280;
    seed.height = 720;
    seed.depth = true;
    seed.depthFloat24 = false;
    seed.format = kDepthUnormFormat;
    seed.frameSeen = resolveFrame;
    seed.copies = 1;
    g_snapshots.push_back(seed);

    regs[xenos::kPaScScreenScissorTl] = 0;
    regs[xenos::kPaScScreenScissorBr] = 1280u | (720u << 16);
    regs[xenos::kRbCopyControl] = 4;
    const float localDepthVertices[] = {0, 0, 448, 0, 448, 720};
    CopySwapped(guest.base + 0xA0001000u,
                reinterpret_cast<const uint8_t*>(localDepthVertices),
                sizeof(localDepthVertices), 2);
    for (uint32_t tile = 1; tile < 3; ++tile)
    {
        const uint32_t left = tile * 416u;
        regs[xenos::kPaScWindowScissorTl] = left;
        regs[xenos::kPaScWindowScissorBr] = (left + 448u) | (720u << 16);
        regs[xenos::kPaScWindowOffset] = (0u - left) & 0x7FFFu;
        regs[xenos::kRbCopyDestBase] = depthBase + MacroTileOffset(left, 0, 1280);

        ResolveVertexBounds raw{};
        uint32_t dx0 = 0, dy0 = 0, dx1 = 0, dy1 = 0;
        const auto decoded = DecodeResolveRectFromVertices(
            guest.base, regs.data(), 1280, 720, dx0, dy0, dx1, dy1, &raw);
        if (tile == 1)
            Require(decoded == ResolveRectDecode::Valid && dx0 == 416 && dx1 == 448,
                    "generic Xenia depth tile 1 remains narrowly clipped");
        else
            Require(decoded == ResolveRectDecode::Empty,
                    "generic Xenia depth tile 2 remains empty");

        uint32_t recoveredKey = 0;
        Require(RecoverPhasedDepthResolve(
                    regs.data(), 1280, 720, raw, kDepthUnormFormat, false,
                    resolveFrame, regs[xenos::kRbCopyDestBase],
                    dx0, dy0, dx1, dy1, recoveredKey),
                "recover same-frame phased depth resolve");
        Require(recoveredKey == depthBase && dx0 == left && dy0 == 0 &&
                    dx1 == std::min(left + 448u, 1280u) && dy1 == 720,
                "phased depth resolve logical destination");

        VkImageCopy recoveredCopy{};
        Require(ResolveCopyRegion(regs.data(), dx0, dy0, dx1, dy1, recoveredCopy) &&
                    recoveredCopy.srcOffset.x == 0 && recoveredCopy.srcOffset.y == 0 &&
                    recoveredCopy.dstOffset.x == int32_t(left) &&
                    recoveredCopy.dstOffset.y == 0 &&
                    recoveredCopy.extent.width == std::min(448u, 1280u - left) &&
                    recoveredCopy.extent.height == 720,
                "phased depth resolve physical-to-logical copy");
        std::printf("PASS: phased depth tile=%u physical=0..%u logical=%u..%u key=%08X\n",
                    tile, recoveredCopy.extent.width, dx0, dx1, recoveredKey);
    }
    g_snapshots.clear();
}

int main(int argc, char** argv)
{
    try
    {
        if (argc >= 2 && std::string_view(argv[1]) == "--fxaa-orientation")
        {
            CheckFxaaOrientation(argc >= 3 ? std::filesystem::path(argv[2])
                                            : std::filesystem::path{});
            return 0;
        }
        if (argc >= 2 && (std::string_view(argv[1]) == "--fsr-easu" ||
                          std::string_view(argv[1]) == "--fsr-rcas"))
        {
            CheckFxaaOrientation(argc >= 3 ? std::filesystem::path(argv[2])
                                            : std::filesystem::path{},
                                 std::string_view(argv[1]) == "--fsr-easu"
                                     ? PresentationProbe::FsrEasu
                                     : PresentationProbe::FsrRcasOnly);
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--edram-pair-bound")
        {
            CheckEdramSurfacePairBound();
            return 0;
        }
        if ((argc == 2 || argc == 3) && std::string_view(argv[1]) == "--edram-alias-redirect")
        {
            CheckEdramAliasRedirectDecision();
            CheckEdramAliasMapping(argc == 3 ? uint32_t(std::stoul(argv[2])) : 1u);
            return 0;
        }
        if ((argc == 2 || argc == 3) && std::string_view(argv[1]) == "--edram-depth-as-color")
        {
            CheckEdramDepthAsColorDecision();
            CheckEdramDepthAsColor(argc == 3 ? uint32_t(std::stoul(argv[2])) : 1u);
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--edram-stencil-copy-bench")
        {
            BenchEdramStencilCopy();
            return 0;
        }
        if ((argc == 2 || argc == 3) && std::string_view(argv[1]) == "--edram-stencil-copy")
        {
            CheckEdramStencilCopy(argc == 3 ? uint32_t(std::stoul(argv[2])) : 1u);
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--resolve-rect-cpu")
        {
            CheckResolveVertexCoordinates();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--authored-mips")
        {
            SyntheticGuest guest;
            guest.Init();
            Require(HostWindow_Init(1280, 720, true), "hidden window");
            Require(VkPresenter_Init(HostWindow_NativeHandle(), 1280, 720), "presenter init");
            CheckAuthoredMipUpload(guest);
            VkPresenter_Shutdown();
            return 0;
        }
        const bool rectangle = argc == 3 && std::string_view(argv[2]) == "--rectangle";
        const bool resolveSwapIdentity = argc == 3 && std::string_view(argv[2]) == "--resolve-swap-rgba";
        const bool resolveSwap = resolveSwapIdentity ||
            (argc == 3 && std::string_view(argv[2]) == "--resolve-swap");
        const bool resolveSource = resolveSwap ||
            (argc == 3 && std::string_view(argv[2]) == "--resolve-source");
        const bool tiledResolve = argc == 3 && std::string_view(argv[2]) == "--tiled-resolve";
        const bool inverseExec = argc == 3 && std::string_view(argv[2]) == "--conditional-exec-not";
        const bool conditionalExec = inverseExec ||
            (argc == 3 && std::string_view(argv[2]) == "--conditional-exec");
        Require(argc == 2 || rectangle || resolveSource || tiledResolve || conditionalExec,
                "expected vertex SPIR-V path [--rectangle|--resolve-source|--tiled-resolve|--conditional-exec|--conditional-exec-not]");
        SyntheticGuest guest;
        guest.Init();
        // Original synthetic Xenos microcode, not game code: exec-end containing
        // tfetch2D r0.xy from s3, followed by an identity max export to oC0.
        std::vector<uint32_t> code = {0x00012001, 0x00002800, 0,
            1u | (3u << 20) | (4u << 26),
            0x688u | (3u << 12) | (3u << 14) | (3u << 16) | (7u << 18) | (1u << 28),
            1u << 14,
            (1u << 15) | (15u << 16) | (50u << 26), 0, (2u << 24) | (3u << 30)};
        if (conditionalExec)
        {
            // Sample red, conditionally overwrite r0 with zero, then export.
            // Boolean b37 is deliberately outside the first packed DWORD.
            auto exec = [](uint64_t op, uint64_t address, uint64_t sequence) {
                return (op << 44) | (1ull << 43) | (sequence << 16) |
                       (1ull << 12) | address;
            };
            const uint64_t fetch = exec(1, 2, 1);
            const uint64_t conditional = exec(3, 3, 0) | (37ull << 34) |
                                         (uint64_t(!inverseExec) << 42);
            const uint64_t end = exec(2, 4, 0);
            code = {uint32_t(fetch), uint32_t((fetch >> 32) | (conditional << 16)),
                    uint32_t(conditional >> 16), uint32_t(end), uint32_t(end >> 32), 0,
                    code[3], code[4], code[5],
                    (15u << 16) | (50u << 26), 0, (2u << 24),
                    code[6], code[7], code[8]};
        }
        for (auto& word : code) word = __builtin_bswap32(word);
        ShaderTranslator::Result translated;
        std::string error;
        const bool translatedOk = ShaderTranslator::Translate("ps_0000000000000001", reinterpret_cast<uint8_t*>(code.data()),
            code.size() * sizeof(uint32_t), translated, error);
        Require(translatedOk, error.c_str());
        std::printf("Synthetic translated PS metadata: %s\n", translated.metaJson.c_str());
        Require(HostWindow_Init(1280, 720, true), "hidden window");
        Require(VkPresenter_Init(HostWindow_NativeHandle(), 1280, 720), "presenter init");
        if (tiledResolve) CheckResolveRegions();
        const auto clearAttachments = reinterpret_cast<PFN_vkCmdClearAttachments>(
            p_vkGetDeviceProcAddr(g_device, "vkCmdClearAttachments"));
        Require(clearAttachments != nullptr, "load synthetic tile clear");
        std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
        Require(bool(file), "open vertex shader");
        const size_t size = size_t(file.tellg());
        Require(size && size % 4 == 0, "vertex shader size");
        std::vector<uint32_t> vertex(size / 4);
        file.seekg(0);
        file.read(reinterpret_cast<char*>(vertex.data()), size);
        ShaderModuleRec vs{}, ps{};
        vs.hash = 2;
        if (rectangle) vs.attributes.push_back({0, 0, 57, 1, 1, 7, 0, 0});
        ps.hash = 1;
        ps.type = 1;
        Require(PopulateShaderModuleMetadata(
                    MojoTranslatedShader{1, 1, translated.spirv, translated.metaJson,
                                         translated.hlsl},
                    ps),
                "translated shader metadata");
        Require(ps.usesTextures && ps.textureSlots == std::vector<uint32_t>{3}, "translated sampler metadata");
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = size;
        ci.pCode = vertex.data();
        Require(p_vkCreateShaderModule(g_device, &ci, nullptr, &vs.module) == VK_SUCCESS, "vertex module");
        ci.codeSize = translated.spirv.size();
        ci.pCode = reinterpret_cast<const uint32_t*>(translated.spirv.data());
        Require(p_vkCreateShaderModule(g_device, &ci, nullptr, &ps.module) == VK_SUCCESS, "pixel module");
        g_modules.push_back(vs);
        g_modules.push_back(ps);
        std::vector<uint32_t> regs(0x5000);
        // The synthetic stream is intentionally unclamped. A zero-initialized
        // VGT_MAX_VTX_INDX means "only vertex 0" on Xenos and now that the
        // presenter honors min/max index semantics it would collapse RECTLIST
        // corners to one point.
        regs[xenos::kVgtMinVtxIndx] = 0;
        regs[xenos::kVgtMaxVtxIndx] = xenos::kVertexIndexMask;
        constexpr uint32_t address = 0x01000000;
        regs[xenos::kRbCopyDestBase] = address;
        regs[xenos::kRbCopyDestPitch] = 1280 | (720 << 16);
        regs[xenos::kPaScWindowScissorBr] = 1280 | (720 << 16);
        // A zero screen scissor is empty coverage, not an unset register.
        regs[xenos::kPaScScreenScissorBr] = 1280 | (720 << 16);
        uint32_t* fetch = regs.data() + xenos::kFetchConstantBase + 3 * 6;
        fetch[0] = 2 | (2 << 10) | (2 << 13);
        fetch[1] = address | 6;
        fetch[2] = 1279 | (719 << 13);
        fetch[3] = (0x688 << 1) | (1 << 19) | (1 << 21);
        fetch[5] = 1 << 9;
        if (resolveSwap)
        {
            // Resolve swaps R/B, then the BGRA texture fetch swaps them back.
            // This must still sample the original red from both RT0 and RT1.
            regs[xenos::kRbCopyDestInfo] = (1u << 24) | (6u << 7) | 2u;
            fetch[3] = ((resolveSwapIdentity ? 0x688u : 0x60Au) << 1) | (1 << 19) | (1 << 21);
        }
        for (uint32_t pass = 0; pass < 3; ++pass)
        {
            auto setResolveVertices = [&](float left, float right) {
                const float positions[] = {left, 0, right, 0, right, 720};
                CopySwapped(guest.base + 0xA0001000u,
                            reinterpret_cast<const uint8_t*>(positions), sizeof(positions), 2);
                regs[xenos::kFetchConstantBase] = 0x1000u | 3u;
                regs[xenos::kFetchConstantBase + 1] = (6u << 2) | 2u;
                regs[xenos::kPaSuVtxCntl] = 1u;
            };
            setResolveVertices(0, 1280);
            const bool dummy = pass == 1;
            if (conditionalExec)
            {
                // b5 must not alias b37 through the legacy OR-folded DWORD.
                regs[xenos::kBoolConstantBase] = 1u << 5;
                regs[xenos::kBoolConstantBase + 1] = (dummy != inverseExec) ? (1u << 5) : 0;
            }
            Require(BeginFrame(), "begin frame");
            const bool rt1 = (resolveSource || tiledResolve) && pass == 2;
            regs[xenos::kRbCopyControl] = rt1 ? 1 : 0;
            regs[xenos::kRbColor1Info] = 0x438;
            Require(SwitchActiveColorSurface(0, rt1 ? 0x438 : 0), "select red EDRAM surface");
            ClearLive(VkClearColorValue{{resolveSwapIdentity ? 0.0f : 1.0f, 0,
                                         resolveSwapIdentity ? 1.0f : 0.0f, 1}});
            if (resolveSource)
            {
                // The title changes RB_COLOR_INFO for its resolve without an
                // intervening draw. The last drawn surface is NOT the source.
                Require(SwitchActiveColorSurface(0, rt1 ? 0 : 0x438), "select unrelated EDRAM surface");
                ClearLive(VkClearColorValue{{0, 0, 1, 1}});
            }
            if (tiledResolve)
            {
                // Three overlapping logical tiles reuse one physical EDRAM
                // window. Different colors catch both stale reads and repeats.
                for (uint32_t tile = 0; tile < 3; ++tile)
                {
                    Require(SwitchActiveColorSurface(0, rt1 ? 0x438 : 0), "select local tile");
                    ClearLive(VkClearColorValue{{0, 0, 0, 0}});
                    VkClearAttachment attachment{};
                    attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    attachment.clearValue.color.float32[tile] = 1.0f;
                    attachment.clearValue.color.float32[3] = 1.0f;
                    const VkClearRect local{{{0, 0}, {448, 720}}, 0, 1};
                    clearAttachments(g_commandBuffer, 1, &attachment, 1, &local);
                    const uint32_t x = tile * 416;
                    regs[xenos::kRbCopyDestBase] = address + MacroTileOffset(x, 0, 1280);
                    regs[xenos::kPaScWindowScissorTl] = x;
                    regs[xenos::kPaScWindowScissorBr] = (x + 448) | (720 << 16);
                    regs[xenos::kPaScWindowOffset] = (0u - x) & 0x7FFFu;
                    regs[xenos::kPaSuScModeCntl] = 1u << 16;
                    setResolveVertices(float(x), float(x + 448));
                    Require(ResolveColorSurface(guest.base, regs.data()), "resolve local tile");
                }
                regs[xenos::kPaScWindowOffset] = 0;
            }
            else Require(ResolveColorSurface(guest.base, regs.data()), "resolve known source");
            ClearLive(VkClearColorValue{{0, 0, 1, 1}});
            std::array<float, kTextureSlots> sampleScales{};
            const TextureBundle* bundle = PrepareTextures(nullptr, regs.data(), vs, ps, sampleScales, true);
            Require(bundle, "snapshot descriptors");
            // Xenos blending disabled is ONE/ZERO/ADD for color and alpha.
            const uint32_t primitive = rectangle ? xenos::kRectangleList : xenos::kTriangleList;
            const VkPipeline pipeline = GetPipeline(vs, ps, primitive, 15,
                                                     0x00010001u, 0x00010001u, 0, 0, 0, 4,
                                                     false, VK_SAMPLE_COUNT_1_BIT,
                                                     kDepthUnormFormat);
            Require(pipeline != VK_NULL_HANDLE, "texture pipeline");
            const VkDeviceSize shared = UploadShared(regs.data(), sampleScales);
            Require(shared != VK_WHOLE_SIZE, "shared upload");
            if (dummy && !conditionalExec)
            {
                uint32_t index = 15;
                std::memcpy(g_uploadMapped + shared + mojorecomp::texture_abi::IndexOffset(0, 3), &index, 4);
            }
            uint64_t push[3] = {g_uploadAddress + shared, g_uploadAddress + shared, g_uploadAddress + shared};
            p_vkCmdBindPipeline(g_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            p_vkCmdBindDescriptorSets(g_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                g_pipelineLayout, 0, 4, bundle->sets, 0, nullptr);
            const VkViewport viewport = DecodeViewport(regs.data());
            VkRect2D scissor{{0, 0}, {1280, 720}};
            const float blend[4]{};
            p_vkCmdSetViewport(g_commandBuffer, 0, 1, &viewport);
            p_vkCmdSetScissor(g_commandBuffer, 0, 1, &scissor);
            p_vkCmdSetBlendConstants(g_commandBuffer, blend);
            p_vkCmdSetStencilReference(g_commandBuffer, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
            p_vkCmdSetStencilCompareMask(g_commandBuffer, VK_STENCIL_FACE_FRONT_AND_BACK, 255);
            p_vkCmdSetStencilWriteMask(g_commandBuffer, VK_STENCIL_FACE_FRONT_AND_BACK, 255);
            p_vkCmdPushConstants(g_commandBuffer, g_pipelineLayout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
            if (rectangle)
            {
                // Consecutive perimeter corners, matching rectangle-list streams.
                // The negative control also reverses winding to cover both orders.
                float vertices[3][7] = {
                    {128, 96, 0, 0, 0, 0, 0},
                    {dummy ? 128.0f : 1152.0f, dummy ? 624.0f : 96.0f, 0, 0, 0, 0, 0},
                    {1152, 624, 0, 0, 0, 0, 0}};
                // The third pass checks the indexed guest path and a nonzero
                // base vertex while the host still consumes the expanded strip.
                // Also swap corners 1 and 2 so the rectangle diagonal is 0-1;
                // a fixed 0+2-1 reconstruction leaves the rectangle malformed.
                if (pass == 2)
                    std::swap(vertices[1], vertices[2]);
                const uint32_t baseVertex = pass == 2 ? 1u : 0u;
                CopySwapped(guest.base + 0xA0001000u + baseVertex * sizeof(vertices[0]),
                            reinterpret_cast<const uint8_t*>(vertices),
                            sizeof(vertices), 2);
                regs[xenos::kFetchConstantBase] = 0x1000u | 3u;
                regs[xenos::kFetchConstantBase + 1] = ((21u + baseVertex * 7u) << 2) | 2u;
                regs[xenos::kVgtIndxOffset] = baseVertex;
                Pm4Draw draw{};
                draw.primType = primitive;
                draw.indexCount = 3;
                if (pass == 2)
                {
                    const uint32_t indices[] = {0, 1, 2};
                    draw.indexed = true;
                    draw.index32 = true;
                    draw.indexEndian = 2;
                    draw.indexVa = 0xA0001800u;
                    CopySwapped(guest.base + draw.indexVa, reinterpret_cast<const uint8_t*>(indices),
                                sizeof(indices), draw.indexEndian);
                }
                PreparedDraw prepared{};
                Require(PrepareVertexBindings(guest.base, draw, regs.data(), vs, prepared),
                        "prepare real rectangle upload");
                Require(!prepared.indexed, "rectangle expanded to non-indexed stream");
                p_vkCmdDraw(g_commandBuffer, prepared.count, 1, 0, 0);
            }
            else p_vkCmdDraw(g_commandBuffer, 3, 1, 0, 0);
            CheckPixels(dummy, rectangle, tiledResolve);
        }
        VkPresenter_Shutdown();
        return 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        VkPresenter_Shutdown();
        return 1;
    }
}
