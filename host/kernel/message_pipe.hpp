#pragma once

// The bookkeeping of a message pipe (sceKernel*MsgPipe), separate from the
// HLE calls so it can be tested without a kernel: the bytes in the pipe's
// buffer, and the senders and receivers waiting on it, served first come,
// first served.
//
// A pipe has a buffer of `capacity` bytes (it may be 0). A send puts bytes in
// and a receive takes them out, in order. Each waits in one of two modes:
// whole (the call ends once all of its bytes have gone or come) or partial
// (it ends once any have). A request moves as many bytes as it can whenever
// the pipe changes, so a send larger than the buffer goes through in pieces
// while a receiver takes them. With no buffer, bytes go only to a receiver
// that is waiting for them.

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <vector>

namespace portablekit::kernel_pipes {

class MessagePipe {
public:
    using Ticket = std::uint64_t;

    explicit MessagePipe(std::uint32_t capacity = 0u) : capacity_(capacity) {}

    [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::uint32_t available() const noexcept { return static_cast<std::uint32_t>(buffer_.size()); }
    [[nodiscard]] std::uint32_t free_space() const noexcept { return capacity_ - available(); }
    [[nodiscard]] std::uint32_t waiting_senders() const noexcept { return static_cast<std::uint32_t>(senders_.size()); }
    [[nodiscard]] std::uint32_t waiting_receivers() const noexcept {
        return static_cast<std::uint32_t>(receivers_.size());
    }

    // Queues a send or a receive, moves what can be moved, and returns its
    // ticket. Poll it with sent()/received() until it is done.
    Ticket send(std::vector<std::uint8_t> bytes, bool whole);
    Ticket receive(std::uint32_t size, bool whole);

    // Done: how many bytes went. Not done: nothing.
    [[nodiscard]] std::optional<std::uint32_t> sent(Ticket ticket);
    // Done: the bytes that came. Not done: nothing.
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> received(Ticket ticket);

    // A request that stops waiting (a timeout): it leaves the queue. Bytes a
    // whole send had already put in stay there; bytes a whole receive had
    // already taken go back to the front of the buffer. Returns that count.
    std::uint32_t withdraw(Ticket ticket);

    // The Try forms: done at once or not at all, and never ahead of a thread
    // already waiting. Empty when a PSP answers "full" or "empty".
    [[nodiscard]] std::optional<std::uint32_t> try_send(const std::vector<std::uint8_t> &bytes, bool whole);
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> try_receive(std::uint32_t size, bool whole);

    // Every waiting request is dropped; waits end with the cancel error.
    // Returns how many there were. The generation lets a waiter tell.
    std::uint32_t cancel();
    // Empties the buffer.
    void clear() { buffer_.clear(); }
    [[nodiscard]] std::uint64_t cancel_generation() const noexcept { return generation_; }

private:
    struct Send {
        std::vector<std::uint8_t> bytes;
        std::uint32_t moved{};
        bool whole{};
        bool done{};
    };
    struct Receive {
        std::uint32_t size{};
        std::vector<std::uint8_t> bytes;
        bool whole{};
        bool done{};
    };

    void pump();
    // What a sender may put in now: the free buffer, or with no buffer what
    // the first waiting receiver still wants beyond what is already there.
    [[nodiscard]] std::uint32_t room() const;

    std::uint32_t capacity_{};
    std::deque<std::uint8_t> buffer_;
    Ticket next_ticket_{1u};
    std::uint64_t generation_{};
    std::deque<Ticket> senders_;
    std::deque<Ticket> receivers_;
    std::map<Ticket, Send> sends_;
    std::map<Ticket, Receive> receives_;
};

} // namespace portablekit::kernel_pipes
