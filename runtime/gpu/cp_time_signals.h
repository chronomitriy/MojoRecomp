#pragma once

// Ordering rules for CP-time signals (docs/HFR-PHASE3.md section 3). They are
// kept apart from the renderer seams so tests/swap_handshake_test.cpp drives
// exactly the decisions the runtime makes.

#include <cstddef>
#include <cstdint>
#include <deque>

namespace mojorecomp::gpu {

// Decides, for each GPU store or command-processor interrupt the PM4 thread
// meets, whether it may take effect right there (CP time) or must be queued
// behind the Vulkan thread like every other command. A CP-time signal must
// never overtake one that was queued: a store could land out of order for its
// address, and an interrupt could run before the stores ahead of it. So the
// gate only opens once every queued signal has completed.
class CpTimeSignalGate
{
public:
    // policyAllows: CP-time signals are requested and the title runs an HFR
    // policy. completed: the executor's completed sequence.
    bool Allow(bool policyAllows, uint64_t completed) const noexcept
    {
        return policyAllows && completed >= lastQueued_;
    }

    void NoteQueued(uint64_t sequence) noexcept
    {
        if (sequence > lastQueued_)
            lastQueued_ = sequence;
    }

    uint64_t LastQueued() const noexcept { return lastQueued_; }

private:
    uint64_t lastQueued_ = 0;
};

// Presents the Vulkan thread has not completed yet, oldest first. Once swaps no
// longer wait for the Vulkan thread, the PM4 thread enqueues a present and then
// waits for the oldest ones until at most `limit` remain outstanding.
class PresentLagBound
{
public:
    // Forgets the presents that have completed; returns how many remain.
    size_t Retire(uint64_t completed)
    {
        while (!presents_.empty() && presents_.front() <= completed)
            presents_.pop_front();
        return presents_.size();
    }

    void Add(uint64_t sequence)
    {
        if (sequence)
            presents_.push_back(sequence);
    }

    // The present to wait for before the producer continues, or 0 when no more
    // than `limit` are outstanding.
    uint64_t MustWaitFor(size_t limit) const noexcept
    {
        return presents_.size() > limit ? presents_.front() : 0;
    }

    void PopOldest()
    {
        if (!presents_.empty())
            presents_.pop_front();
    }

    size_t Outstanding() const noexcept { return presents_.size(); }

private:
    std::deque<uint64_t> presents_;
};

} // namespace mojorecomp::gpu
