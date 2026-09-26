// Fixed-size memory pools (sceKernel*Fpl): layout, allocation, freeing and the
// order waiting threads are served in. No kernel, no game.

#include "kernel/fixed_pool.hpp"

#include <cstdio>

namespace {

int g_failures = 0;

void check(bool condition, const char *what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) ++g_failures;
}

} // namespace

int main() {
    using namespace portablekit::kernel_pools;

    const auto layout = fixed_pool_layout(0x116C400u, 1u, 0u);
    check(layout && layout->alignment == 4u && layout->bytes == 0x116C400u, "one large block, default alignment");
    const auto odd = fixed_pool_layout(10u, 3u, 16u);
    check(odd && odd->stride == 16u && odd->bytes == 48u, "blocks are rounded up to the alignment");
    check(!fixed_pool_layout(0u, 3u, 4u) && !fixed_pool_layout(16u, 0u, 4u), "no blocks, or blocks of no size, are refused");
    check(!fixed_pool_layout(16u, 3u, 12u), "an alignment that is not a power of two is refused");
    check(!fixed_pool_layout(0x80000000u, 4u, 4u), "a pool past 4 GiB is refused");

    FixedPool pool(0x09000000u, 10u, 3u, *odd);
    check(pool.free_count() == 3u, "a new pool is all free");
    const auto a = pool.allocate();
    const auto b = pool.allocate();
    const auto c = pool.allocate();
    check(a == 0x09000000u && b == 0x09000010u && c == 0x09000020u, "blocks are handed out lowest first, aligned");
    check(!pool.allocate() && pool.free_count() == 0u, "a full pool has nothing to give");
    check(!pool.free(0x09000008u), "an address inside a block is not a block");
    check(!pool.free(0x09000030u), "an address past the pool is not a block");
    check(pool.free(*b) && pool.free_count() == 1u, "a block in use is freed");
    check(!pool.free(*b), "a free block cannot be freed twice");
    check(pool.allocate() == b, "the freed block is handed out again");

    const std::uint64_t first = pool.enqueue();
    const std::uint64_t second = pool.enqueue();
    check(pool.waiting() == 2u && pool.first_in_line(first) && !pool.first_in_line(second),
          "waiting threads are served in the order they came");
    pool.leave(first);
    check(pool.first_in_line(second), "when the first leaves (served, timed out), the next is first");
    const std::uint64_t generation = pool.cancel_generation();
    check(pool.cancel_waiters() == 1u && pool.waiting() == 0u && pool.cancel_generation() != generation,
          "cancelling releases every waiter and says how many");

    if (g_failures != 0) std::printf("%d failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
