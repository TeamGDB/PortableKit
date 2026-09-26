#include "message_pipe.hpp"

#include <algorithm>

namespace portablekit::kernel_pipes {

std::uint32_t MessagePipe::room() const {
    if (capacity_ != 0u) return free_space();
    if (receivers_.empty()) return 0u;
    const Receive &first = receives_.at(receivers_.front());
    const std::uint32_t wanted = first.size - static_cast<std::uint32_t>(first.bytes.size());
    return wanted > available() ? wanted - available() : 0u;
}

void MessagePipe::pump() {
    for (bool moved = true; moved;) {
        moved = false;
        // The first receiver takes what is there.
        if (!receivers_.empty()) {
            Receive &first = receives_.at(receivers_.front());
            const std::uint32_t wanted = first.size - static_cast<std::uint32_t>(first.bytes.size());
            const std::uint32_t take = std::min(wanted, available());
            if (take != 0u) {
                first.bytes.insert(first.bytes.end(), buffer_.begin(), buffer_.begin() + take);
                buffer_.erase(buffer_.begin(), buffer_.begin() + take);
                moved = true;
            }
            if (first.bytes.size() == first.size || (!first.whole && !first.bytes.empty())) {
                first.done = true;
                receivers_.pop_front();
                moved = true;
            }
        }
        // The first sender puts in what fits.
        if (!senders_.empty()) {
            Send &first = sends_.at(senders_.front());
            const std::uint32_t left = static_cast<std::uint32_t>(first.bytes.size()) - first.moved;
            const std::uint32_t put = std::min(left, room());
            if (put != 0u) {
                buffer_.insert(buffer_.end(), first.bytes.begin() + first.moved, first.bytes.begin() + first.moved + put);
                first.moved += put;
                moved = true;
            }
            if (first.moved == first.bytes.size() || (!first.whole && first.moved != 0u)) {
                first.done = true;
                senders_.pop_front();
                moved = true;
            }
        }
    }
}

MessagePipe::Ticket MessagePipe::send(std::vector<std::uint8_t> bytes, bool whole) {
    const Ticket ticket = next_ticket_++;
    sends_[ticket] = Send{std::move(bytes), 0u, whole, false};
    senders_.push_back(ticket);
    pump();
    return ticket;
}

MessagePipe::Ticket MessagePipe::receive(std::uint32_t size, bool whole) {
    const Ticket ticket = next_ticket_++;
    Receive request;
    request.size = size;
    request.whole = whole;
    receives_[ticket] = std::move(request);
    receivers_.push_back(ticket);
    pump();
    return ticket;
}

std::optional<std::uint32_t> MessagePipe::sent(Ticket ticket) {
    pump();
    const auto found = sends_.find(ticket);
    if (found == sends_.end() || !found->second.done) return std::nullopt;
    const std::uint32_t moved = found->second.moved;
    sends_.erase(found);
    return moved;
}

std::optional<std::vector<std::uint8_t>> MessagePipe::received(Ticket ticket) {
    pump();
    const auto found = receives_.find(ticket);
    if (found == receives_.end() || !found->second.done) return std::nullopt;
    std::vector<std::uint8_t> bytes = std::move(found->second.bytes);
    receives_.erase(found);
    return bytes;
}

std::uint32_t MessagePipe::withdraw(Ticket ticket) {
    std::uint32_t count = 0u;
    if (const auto send = sends_.find(ticket); send != sends_.end()) {
        count = send->second.moved;
        sends_.erase(send);
        senders_.erase(std::remove(senders_.begin(), senders_.end(), ticket), senders_.end());
    }
    if (const auto receive = receives_.find(ticket); receive != receives_.end()) {
        // What it had taken goes back in front, so nothing is lost.
        const std::vector<std::uint8_t> &taken = receive->second.bytes;
        count = static_cast<std::uint32_t>(taken.size());
        if (!receive->second.done) buffer_.insert(buffer_.begin(), taken.begin(), taken.end());
        receives_.erase(receive);
        receivers_.erase(std::remove(receivers_.begin(), receivers_.end(), ticket), receivers_.end());
    }
    pump();
    return count;
}

std::optional<std::uint32_t> MessagePipe::try_send(const std::vector<std::uint8_t> &bytes, bool whole) {
    if (!senders_.empty()) return std::nullopt;
    const auto size = static_cast<std::uint32_t>(bytes.size());
    // With no buffer the bytes can only go to receivers already waiting.
    std::uint32_t space = room();
    if (capacity_ == 0u) {
        space = 0u;
        for (const Ticket ticket : receivers_) {
            const Receive &waiting = receives_.at(ticket);
            space += waiting.size - static_cast<std::uint32_t>(waiting.bytes.size());
        }
        space = space > available() ? space - available() : 0u;
    }
    const std::uint32_t put = std::min(size, space);
    if (put == 0u && size != 0u) return std::nullopt;
    if (whole && put < size) return std::nullopt;
    std::vector<std::uint8_t> part(bytes.begin(), bytes.begin() + put);
    const Ticket ticket = send(std::move(part), true);
    const auto moved = sent(ticket);
    return moved.value_or(withdraw(ticket));
}

std::optional<std::vector<std::uint8_t>> MessagePipe::try_receive(std::uint32_t size, bool whole) {
    if (!receivers_.empty()) return std::nullopt;
    pump();
    // What waiting senders still hold counts too: taking from the buffer
    // makes room for it.
    std::uint32_t there = available();
    for (const Ticket ticket : senders_) {
        const Send &waiting = sends_.at(ticket);
        there += static_cast<std::uint32_t>(waiting.bytes.size()) - waiting.moved;
    }
    const std::uint32_t take = std::min(size, there);
    if (take == 0u && size != 0u) return std::nullopt;
    if (whole && take < size) return std::nullopt;
    const Ticket ticket = receive(take, true);
    if (auto bytes = received(ticket)) return bytes;
    (void)withdraw(ticket);
    return std::nullopt;
}

std::uint32_t MessagePipe::cancel() {
    const auto count = static_cast<std::uint32_t>(senders_.size() + receivers_.size());
    for (const Ticket ticket : senders_) sends_.erase(ticket);
    for (const Ticket ticket : receivers_) receives_.erase(ticket);
    senders_.clear();
    receivers_.clear();
    ++generation_;
    return count;
}

} // namespace portablekit::kernel_pipes
