#include "movie/replacement_movie.hpp"

#include <algorithm>
#include <thread>

namespace portablekit::movie {
namespace {

constexpr std::size_t kReadSize = 4u << 20u;

// Where the start code at or after `from` begins (three bytes 00 00 01, or
// four with a leading zero), or npos.
std::size_t find_start(const std::vector<std::uint8_t> &data, std::size_t from) {
    for (std::size_t i = from; i + 3u <= data.size(); ++i) {
        if (data[i] == 0u && data[i + 1u] == 0u && data[i + 2u] == 1u)
            return i > from && data[i - 1u] == 0u ? i - 1u : i;
    }
    return std::string::npos;
}

std::size_t header_at(const std::vector<std::uint8_t> &nal) {
    return nal.size() > 3u && nal[2] == 1u ? 3u : 4u;  // after the start code
}

} // namespace

bool ReplacementMovie::open(const std::filesystem::path &path) {
    file_.open(path, std::ios::binary);
    if (!file_.is_open()) return false;
    // Frame threads: a large picture decodes several times faster, and the
    // game's own movie decoding is not held up by it.
    const unsigned threads = std::clamp(std::thread::hardware_concurrency() / 2u, 2u, 8u);
    if (!decoder_.open(threads)) {
        file_.close();
        return false;
    }
    return refill();
}

bool ReplacementMovie::refill() {
    if (end_of_file_) return false;
    data_.erase(data_.begin(), data_.begin() + static_cast<std::ptrdiff_t>(at_));
    at_ = 0u;
    const std::size_t old_size = data_.size();
    data_.resize(old_size + kReadSize);
    file_.read(reinterpret_cast<char *>(data_.data() + old_size), static_cast<std::streamsize>(kReadSize));
    const auto got = static_cast<std::size_t>(file_.gcount());
    data_.resize(old_size + got);
    if (got < kReadSize) end_of_file_ = true;
    return got > 0u;
}

// The next NAL unit, start code included.
bool ReplacementMovie::next_nal(std::vector<std::uint8_t> &nal) {
    while (true) {
        const std::size_t start = find_start(data_, at_);
        if (start == std::string::npos) {
            if (!refill()) return false;
            continue;
        }
        const std::size_t next = find_start(data_, start + 4u);
        if (next == std::string::npos && !end_of_file_) {
            // The unit runs on past what is read: read more and look again.
            at_ = start;
            if (!refill() && !end_of_file_) return false;
            continue;
        }
        const std::size_t end = next == std::string::npos ? data_.size() : next;
        nal.assign(data_.begin() + static_cast<std::ptrdiff_t>(start), data_.begin() + static_cast<std::ptrdiff_t>(end));
        at_ = end;
        return true;
    }
}

// The next access unit: the NAL units of one picture, with the parameter
// sets and SEI that come before it. A picture's first slice (a slice whose
// first_mb_in_slice is 0), an access unit delimiter, or a parameter set or
// SEI after a slice starts the next unit.
bool ReplacementMovie::next_unit(std::vector<std::uint8_t> &unit) {
    unit.clear();
    bool slice = false;
    std::vector<std::uint8_t> nal;
    while (true) {
        if (!pending_.empty()) {
            nal.swap(pending_);
            pending_.clear();
        } else if (!next_nal(nal)) {
            return !unit.empty();
        }
        const std::size_t header = header_at(nal);
        if (nal.size() <= header) continue;
        const unsigned type = nal[header] & 0x1Fu;
        const bool vcl = type == 1u || type == 5u;
        const bool starts_picture = vcl && nal.size() > header + 1u && (nal[header + 1u] & 0x80u) != 0u;
        const bool starts_unit = starts_picture || type == 9u || type == 6u || type == 7u || type == 8u;
        if (slice && starts_unit) {
            pending_.swap(nal);
            return true;
        }
        unit.insert(unit.end(), nal.begin(), nal.end());
        slice = slice || vcl;
    }
}

bool ReplacementMovie::next_picture() {
    std::vector<std::uint8_t> unit;
    while (!finished_) {
        if (next_unit(unit)) {
            if (decoder_.decode(unit, current_)) {
                ++produced_;
                return true;
            }
            continue;
        }
        // The end of the file: the pictures the decoder still holds.
        if (decoder_.drain(current_)) {
            ++produced_;
            return true;
        }
        finished_ = true;
    }
    return false;
}

bool ReplacementMovie::picture(std::uint64_t index, Picture &out) {
    if (!is_open()) return false;
    while (produced_ <= index) {
        if (!next_picture()) return false;
    }
    out = current_;
    return true;
}

} // namespace portablekit::movie
