#include "psmf_header.hpp"

namespace portablekit::movie {
namespace {

std::uint32_t be32(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::uint32_t>(bytes[at]) << 24u | static_cast<std::uint32_t>(bytes[at + 1u]) << 16u |
           static_cast<std::uint32_t>(bytes[at + 2u]) << 8u | bytes[at + 3u];
}

std::uint64_t be48(std::span<const std::uint8_t> bytes, std::size_t at) {
    std::uint64_t value = 0u;
    for (std::size_t i = 0; i < 6u; ++i) value = value << 8u | bytes[at + i];
    return value;
}

} // namespace

int PsmfStream::type() const noexcept {
    if ((stream_id & 0xF0u) == 0xE0u) return 0;
    if (stream_id == 0xBDu) {
        if ((private_id & 0xF0u) == 0x00u) return 1;
        if ((private_id & 0xF0u) == 0x10u) return 2;
    }
    return 3;
}

int PsmfStream::channel() const noexcept { return (stream_id == 0xBDu ? private_id : stream_id) & 0x0Fu; }

std::optional<PsmfHeader> parse_psmf_header(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kPsmfStreamTableOffset + 2u) return std::nullopt;
    if (bytes[0] != 'P' || bytes[1] != 'S' || bytes[2] != 'M' || bytes[3] != 'F') return std::nullopt;
    PsmfHeader header;
    header.version = static_cast<std::uint32_t>(bytes[4]) | static_cast<std::uint32_t>(bytes[5]) << 8u |
                     static_cast<std::uint32_t>(bytes[6]) << 16u | static_cast<std::uint32_t>(bytes[7]) << 24u;
    header.header_size = be32(bytes, 0x08u);
    header.stream_size = be32(bytes, 0x0Cu);
    header.start_time = be48(bytes, 0x54u);
    header.end_time = be48(bytes, 0x5Au);
    const std::size_t count = static_cast<std::size_t>(bytes[0x80u]) << 8u | bytes[0x81u];
    const std::size_t table = kPsmfStreamTableOffset + 2u;
    if (bytes.size() < table + count * kPsmfStreamEntrySize) return std::nullopt;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t at = table + i * kPsmfStreamEntrySize;
        PsmfStream stream;
        stream.stream_id = bytes[at];
        stream.private_id = bytes[at + 1u];
        stream.info[0] = bytes[at + 12u];
        stream.info[1] = bytes[at + 13u];
        header.streams.push_back(stream);
    }
    return header;
}

} // namespace portablekit::movie
