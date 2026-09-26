#pragma once

// The bookkeeping of a fixed-size memory pool (sceKernel*Fpl), separate from
// the HLE calls so it can be tested without a kernel: how the blocks are laid
// out in the partition memory the pool was given, which are in use, and the
// order threads that wait for one are served in.
//
// A pool holds `count` blocks of `block_size` bytes. Each block starts at a
// multiple of the pool's alignment (4 unless the creator asks for another),
// so blocks sit `stride` = block_size rounded up to the alignment apart, and
// the pool needs stride * count bytes. A free block is handed out lowest
// address first. Threads that wait are served first come, first served.

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace portablekit::kernel_pools {

struct FixedPoolLayout {
    std::uint32_t alignment{4u};
    std::uint32_t stride{};
    std::uint32_t bytes{};  // what to ask the partition for
};

// Empty when the request is not one a PSP accepts: no blocks, blocks of no
// size, an alignment that is not a power of two, or a pool larger than 4 GiB.
[[nodiscard]] std::optional<FixedPoolLayout> fixed_pool_layout(std::uint32_t block_size, std::uint32_t count,
                                                               std::uint32_t alignment);

class FixedPool {
public:
    FixedPool() = default;
    FixedPool(std::uint32_t address, std::uint32_t block_size, std::uint32_t count, const FixedPoolLayout &layout);

    [[nodiscard]] std::uint32_t address() const noexcept { return address_; }
    [[nodiscard]] std::uint32_t block_size() const noexcept { return block_size_; }
    [[nodiscard]] std::uint32_t count() const noexcept { return static_cast<std::uint32_t>(used_.size()); }
    [[nodiscard]] std::uint32_t free_count() const noexcept;

    // The lowest free block's address, now marked in use.
    [[nodiscard]] std::optional<std::uint32_t> allocate();
    // False when `block` is not the start of a block of this pool in use.
    bool free(std::uint32_t block);

    // Waiting threads, in the order they started to wait. A ticket is served
    // only when it is first in line and a block is free, so a thread that
    // asks later never overtakes one that waits.
    [[nodiscard]] std::uint64_t enqueue();
    void leave(std::uint64_t ticket);
    [[nodiscard]] bool first_in_line(std::uint64_t ticket) const noexcept;
    [[nodiscard]] std::uint32_t waiting() const noexcept { return static_cast<std::uint32_t>(queue_.size()); }
    // sceKernelCancelFpl: every waiting thread is released with an error;
    // returns how many there were.
    std::uint32_t cancel_waiters();
    [[nodiscard]] std::uint64_t cancel_generation() const noexcept { return cancel_generation_; }

private:
    std::uint32_t address_{};
    std::uint32_t block_size_{};
    std::uint32_t stride_{};
    std::vector<bool> used_;
    std::deque<std::uint64_t> queue_;
    std::uint64_t next_ticket_{1u};
    std::uint64_t cancel_generation_{};
};

} // namespace portablekit::kernel_pools
