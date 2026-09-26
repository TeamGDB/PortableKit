// Message pipes (sceKernel*MsgPipe): the buffer, whole and partial sends and
// receives, the order waiting threads are served in, and pipes with no
// buffer. No kernel, no game.

#include "kernel/message_pipe.hpp"

#include <cstdio>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char *what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) ++g_failures;
}

std::vector<std::uint8_t> bytes(std::uint32_t count, std::uint8_t first = 1u) {
    std::vector<std::uint8_t> result(count);
    for (std::uint32_t i = 0; i < count; ++i) result[i] = static_cast<std::uint8_t>(first + i);
    return result;
}

} // namespace

int main() {
    using portablekit::kernel_pipes::MessagePipe;

    {
        MessagePipe pipe(0x400u);
        check(pipe.try_send(bytes(0x54u), true) == 0x54u && pipe.available() == 0x54u,
              "a message that fits goes into the buffer at once");
        const auto got = pipe.try_receive(8u, true);
        check(got && got->size() == 8u && (*got)[0] == 1u && (*got)[7] == 8u, "a receive takes the first bytes");
        check(!pipe.try_receive(0x100u, true) && pipe.available() == 0x4Cu,
              "a whole receive of more than there is fails and takes nothing");
        const auto rest = pipe.try_receive(0x100u, false);
        check(rest && rest->size() == 0x4Cu && pipe.available() == 0u, "a partial receive takes what there is");
        check(!pipe.try_receive(4u, false), "an empty pipe has nothing for a partial receive either");
    }
    {
        MessagePipe pipe(16u);
        check(!pipe.try_send(bytes(20u), true) && pipe.available() == 0u,
              "a whole send larger than the free space fails and puts nothing in");
        check(pipe.try_send(bytes(20u), false) == 16u, "a partial send puts in what fits");
        const auto sender = pipe.send(bytes(8u, 100u), true);
        check(!pipe.sent(sender) && pipe.waiting_senders() == 1u, "a send with no room waits");
        check(!pipe.try_send(bytes(1u), false), "a try does not go ahead of a waiting sender");
        const auto first = pipe.try_receive(16u, true);
        check(first && (*first)[15] == 16u, "the receiver gets the bytes in the order they were sent");
        check(pipe.sent(sender) == 8u && pipe.available() == 8u, "the waiting send completes once there is room");
    }
    {
        MessagePipe pipe(4u);
        const auto receiver = pipe.receive(10u, true);
        const auto sender = pipe.send(bytes(10u), true);
        const auto got = pipe.received(receiver);
        check(got && got->size() == 10u && (*got)[9] == 10u,
              "a whole send larger than the buffer goes through in pieces to a waiting receiver");
        check(pipe.sent(sender) == 10u, "and the send completes with every byte");
    }
    {
        MessagePipe pipe(0u);
        check(!pipe.try_send(bytes(4u), true), "with no buffer and no receiver a send cannot go");
        const auto sender = pipe.send(bytes(4u), true);
        check(!pipe.sent(sender), "so it waits");
        const auto got = pipe.try_receive(4u, true);
        check(got && got->size() == 4u && pipe.sent(sender) == 4u, "a receiver takes it straight from the sender");
        const auto waiting = pipe.receive(3u, true);
        check(pipe.try_send(bytes(3u), true) == 3u && pipe.received(waiting)->size() == 3u,
              "a try send hands bytes to a waiting receiver");
    }
    {
        MessagePipe pipe(8u);
        const auto a = pipe.receive(4u, true);
        const auto b = pipe.receive(4u, true);
        check(pipe.waiting_receivers() == 2u, "receivers wait in order");
        const auto sent = pipe.try_send(bytes(6u), true);
        check(sent == 6u && pipe.received(a) && !pipe.received(b), "the first receiver is served first");
        check(pipe.withdraw(b) == 2u && pipe.waiting_receivers() == 0u && pipe.available() == 2u,
              "a receiver that times out leaves the queue, and what it had taken goes back");
        const auto c = pipe.receive(4u, true);
        const std::uint64_t generation = pipe.cancel_generation();
        check(pipe.cancel() == 1u && pipe.cancel_generation() != generation && !pipe.received(c),
              "cancelling drops every waiter and says how many");
    }

    if (g_failures != 0) std::printf("%d failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
