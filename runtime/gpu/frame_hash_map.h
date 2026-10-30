#pragma once

// A hash map that is emptied once per frame (docs/HFR-PHASE3.md section 7).
// The Vulkan thread's per-frame upload caches used std::unordered_map, which
// allocates a node for every vertex or index upload and frees them all again
// at the start of the next frame. This open-addressing table keeps its slots:
// Clear only advances a generation, and an insert never allocates unless the
// table has to grow. Find and Emplace behave like unordered_map's find and
// emplace (an existing key keeps its value); tests/vertex_path_test.cpp checks
// that against std::unordered_map.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mojorecomp::gpu {

template <typename Key, typename Value, typename Hash>
class FrameHashMap
{
public:
    explicit FrameHashMap(size_t initialCapacity = 4096)
    {
        size_t capacity = 16;
        while (capacity < initialCapacity)
            capacity <<= 1;
        slots_.resize(capacity);
    }

    const Value* Find(const Key& key) const noexcept
    {
        const size_t mask = slots_.size() - 1;
        for (size_t at = Hash{}(key) & mask;; at = (at + 1) & mask)
        {
            const Slot& slot = slots_[at];
            if (slot.generation != generation_)
                return nullptr;
            if (slot.key == key)
                return &slot.value;
        }
    }

    // Inserts the value unless the key is already present.
    void Emplace(const Key& key, const Value& value)
    {
        // Keep at least half of the slots free, so probes stay short and a
        // free slot always ends a lookup.
        if ((size_ + 1) * 2 > slots_.size())
            Grow();
        Insert(key, value);
    }

    void Clear() noexcept
    {
        size_ = 0;
        if (++generation_ == 0)
        {
            // After 2^32 frames: forget the old stamps so none can match again.
            for (Slot& slot : slots_)
                slot.generation = 0;
            generation_ = 1;
        }
    }

    size_t Size() const noexcept { return size_; }

private:
    struct Slot
    {
        uint32_t generation = 0;
        Key key{};
        Value value{};
    };

    void Insert(const Key& key, const Value& value)
    {
        const size_t mask = slots_.size() - 1;
        for (size_t at = Hash{}(key) & mask;; at = (at + 1) & mask)
        {
            Slot& slot = slots_[at];
            if (slot.generation != generation_)
            {
                slot.generation = generation_;
                slot.key = key;
                slot.value = value;
                ++size_;
                return;
            }
            if (slot.key == key)
                return;
        }
    }

    void Grow()
    {
        std::vector<Slot> previous(slots_.size() * 2);
        previous.swap(slots_);
        const uint32_t live = generation_;
        size_ = 0;
        for (Slot& slot : slots_)
            slot.generation = 0;
        generation_ = 1;
        for (const Slot& slot : previous)
            if (slot.generation == live)
                Insert(slot.key, slot.value);
    }

    std::vector<Slot> slots_;
    uint32_t generation_ = 1;
    size_t size_ = 0;
};

} // namespace mojorecomp::gpu
