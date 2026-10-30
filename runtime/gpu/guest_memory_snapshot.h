#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace mojorecomp::gpu {

inline constexpr std::array<uint32_t, 3> kGuestMemoryPhysicalViews{
    0xA0000000u, 0xC0000000u, 0xE0000000u};

struct GuestMemoryIdentity
{
    uint64_t token = 0;
    uint64_t offset = 0;

    explicit operator bool() const noexcept { return token != 0; }
};

class GuestMemorySnapshot
{
public:
    GuestMemorySnapshot() { ranges_.reserve(16); }

    bool Capture(const uint8_t* guestBase, uint32_t address, uint64_t bytes);
    void Overlay(uint32_t address, const void* data, uint64_t bytes);
    const uint8_t* Resolve(uint32_t address, uint64_t bytes) const noexcept;
    bool ResolveIdentity(uint32_t address, uint64_t bytes,
                         GuestMemoryIdentity& out) const noexcept;
    const uint8_t* ResolveZeroFill(uint64_t bytes) const noexcept;
    uint64_t ByteSize() const noexcept { return byteSize_; }

private:
    struct Range
    {
        uint32_t address = 0;
        uint64_t bytes = 0;
        uint64_t identity = 0;
        std::shared_ptr<const std::vector<uint8_t>> data;
    };

    std::vector<Range> ranges_;
    uint64_t byteSize_ = 0;
    mutable std::mutex zeroFillMutex_;
    mutable std::vector<std::shared_ptr<const std::vector<uint8_t>>> zeroFills_;
};

class ScopedGuestMemorySnapshot
{
public:
    explicit ScopedGuestMemorySnapshot(const GuestMemorySnapshot* snapshot) noexcept;
    ~ScopedGuestMemorySnapshot();

    ScopedGuestMemorySnapshot(const ScopedGuestMemorySnapshot&) = delete;
    ScopedGuestMemorySnapshot& operator=(const ScopedGuestMemorySnapshot&) = delete;

private:
    const GuestMemorySnapshot* previous_ = nullptr;
    bool previousReadFailed_ = false;
};

const uint8_t* GuestReadPtr(const uint8_t* guestBase, uint32_t address,
                            uint64_t bytes) noexcept;
bool GuestReadIdentity(uint32_t address, uint64_t bytes,
                       GuestMemoryIdentity& out) noexcept;
bool GuestSnapshotReadFailed() noexcept;
uint64_t GuestSnapshotCacheHitCount() noexcept;
uint64_t GuestSnapshotCacheMissCount() noexcept;
uint64_t GuestSnapshotMissingReadCount() noexcept;
// Guest writes that hit a write-protected tracked page, VirtualProtect calls
// that re-protect tracked pages, and bytes copied into snapshots.
uint64_t GuestSnapshotWriteFaultCount() noexcept;
uint64_t GuestSnapshotProtectCallCount() noexcept;
uint64_t GuestSnapshotCopiedBytes() noexcept;
// Captures served by a fresh copy because they touch a hot writable page
// (opt-in MOJORECOMP_SNAPSHOT_HOT_PAGES=1). The epoch advances once per swap.
uint64_t GuestSnapshotVolatileCaptureCount() noexcept;
// Marks guest memory the command processor executes as commands (the ring and
// indirect buffers). The title rewrites it on every lap of its command buffer,
// and draws read data from inside it: D3D BeginVertices, which Pure3D's
// immediate-mode primitives use, and the resolve and clear rectangles. Small
// captures that touch such pages are copied fresh instead of write-protected
// (MOJORECOMP_SNAPSHOT_COMMAND_MEMORY=0 restores protection).
void GuestSnapshotNoteCommandMemory(uint32_t address, uint64_t bytes) noexcept;
uint64_t GuestSnapshotCommandCaptureCount() noexcept;
void GuestSnapshotAdvanceEpoch() noexcept;
void GuestSnapshotCacheShutdown();

} // namespace mojorecomp::gpu
