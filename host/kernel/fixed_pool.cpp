#include "kernel/fixed_pool.hpp"

#include <algorithm>

namespace portablekit::kernel_pools {

std::optional<FixedPoolLayout> fixed_pool_layout(std::uint32_t block_size, std::uint32_t count,
                                                 std::uint32_t alignment) {
    if (block_size == 0u || count == 0u) return std::nullopt;
    if (alignment == 0u) alignment = 4u;
    if ((alignment & (alignment - 1u)) != 0u) return std::nullopt;
    FixedPoolLayout layout;
    layout.alignment = alignment;
    const std::uint64_t stride = (static_cast<std::uint64_t>(block_size) + alignment - 1u) & ~std::uint64_t{alignment - 1u};
    const std::uint64_t bytes = stride * count;
    if (bytes > 0xFFFFFFFFull) return std::nullopt;
    layout.stride = static_cast<std::uint32_t>(stride);
    layout.bytes = static_cast<std::uint32_t>(bytes);
    return layout;
}

FixedPool::FixedPool(std::uint32_t address, std::uint32_t block_size, std::uint32_t count,
                     const FixedPoolLayout &layout)
    : address_(address), block_size_(block_size), stride_(layout.stride), used_(count, false) {}

std::uint32_t FixedPool::free_count() const noexcept {
    return static_cast<std::uint32_t>(std::count(used_.begin(), used_.end(), false));
}

std::optional<std::uint32_t> FixedPool::allocate() {
    for (std::size_t i = 0; i < used_.size(); ++i) {
        if (used_[i]) continue;
        used_[i] = true;
        return address_ + static_cast<std::uint32_t>(i) * stride_;
    }
    return std::nullopt;
}

bool FixedPool::free(std::uint32_t block) {
    if (block < address_ || stride_ == 0u) return false;
    const std::uint32_t offset = block - address_;
    if (offset % stride_ != 0u) return false;
    const std::size_t index = offset / stride_;
    if (index >= used_.size() || !used_[index]) return false;
    used_[index] = false;
    return true;
}

std::uint64_t FixedPool::enqueue() {
    queue_.push_back(next_ticket_);
    return next_ticket_++;
}

void FixedPool::leave(std::uint64_t ticket) {
    const auto found = std::find(queue_.begin(), queue_.end(), ticket);
    if (found != queue_.end()) queue_.erase(found);
}

bool FixedPool::first_in_line(std::uint64_t ticket) const noexcept {
    return !queue_.empty() && queue_.front() == ticket;
}

std::uint32_t FixedPool::cancel_waiters() {
    const auto count = static_cast<std::uint32_t>(queue_.size());
    queue_.clear();
    ++cancel_generation_;
    return count;
}

} // namespace portablekit::kernel_pools
