#pragma once

// The header of a PSMF movie, as the scePsmf library reads it: the version,
// where the stream data starts and how long it is, the presentation times,
// and one 16-byte entry per elementary stream. Layout, all big-endian, as
// read off Patapon's (UCES00995) intro movie:
//
//   0x00  "PSMF"            0x04  version, four ASCII digits ("0014")
//   0x08  header size       0x0C  stream data size
//   0x54  start time (48-bit, 90 kHz)   0x5A  end time (48-bit)
//   0x80  number of streams (16-bit), then from 0x82 one entry each:
//         +0 stream id (0xE0.. video, 0xBD private: audio)
//         +1 private stream id (0x00.. ATRAC3plus, 0x10.. PCM)
//         +12, +13 video: width and height in 16-pixel units
//                  audio: channels, and a sample-rate code

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace portablekit::movie {

inline constexpr std::size_t kPsmfStreamTableOffset = 0x80u;
inline constexpr std::size_t kPsmfStreamEntrySize = 16u;

struct PsmfStream {
    std::uint8_t stream_id{};
    std::uint8_t private_id{};
    std::uint8_t info[2]{};  // bytes 12 and 13
    // The types scePsmf reports: 0 AVC video, 1 ATRAC3plus, 2 PCM, 3 other.
    [[nodiscard]] int type() const noexcept;
    // The channel the type's numbering uses: the low nibble of the id.
    [[nodiscard]] int channel() const noexcept;
};

struct PsmfHeader {
    std::uint32_t version{};  // the four ASCII digits, first byte lowest
    std::uint32_t header_size{};
    std::uint32_t stream_size{};
    std::uint64_t start_time{};
    std::uint64_t end_time{};
    std::vector<PsmfStream> streams;
};

// Empty if `bytes` does not start with a PSMF header or is too short for the
// stream table it announces.
[[nodiscard]] std::optional<PsmfHeader> parse_psmf_header(std::span<const std::uint8_t> bytes);

} // namespace portablekit::movie
