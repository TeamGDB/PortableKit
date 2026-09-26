// The PSMF header as scePsmf reads it, from a header built here in the layout
// traced from a game's movie. No game data.

#include "movie/psmf_header.hpp"

#include <cstdio>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char *what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) ++g_failures;
}

void put_be(std::vector<std::uint8_t> &bytes, std::size_t at, std::uint64_t value, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) bytes[at + i] = static_cast<std::uint8_t>(value >> (8u * (width - 1u - i)));
}

std::vector<std::uint8_t> header() {
    std::vector<std::uint8_t> bytes(2048u, 0u);
    const char magic[] = "PSMF0014";
    for (std::size_t i = 0; i < 8u; ++i) bytes[i] = static_cast<std::uint8_t>(magic[i]);
    put_be(bytes, 0x08u, 0x800u, 4u);
    put_be(bytes, 0x0Cu, 0x870800u, 4u);
    put_be(bytes, 0x54u, 90000u, 6u);
    put_be(bytes, 0x5Au, 0x542C85u, 6u);
    put_be(bytes, 0x80u, 3u, 2u);
    // Video 0xE0, 480x272; ATRAC3plus on private stream 0; PCM on 0x11.
    bytes[0x82u] = 0xE0u;
    bytes[0x82u + 12u] = 30u;
    bytes[0x82u + 13u] = 17u;
    bytes[0x92u] = 0xBDu;
    bytes[0x92u + 12u] = 2u;
    bytes[0x92u + 13u] = 2u;
    bytes[0xA2u] = 0xBDu;
    bytes[0xA3u] = 0x11u;
    return bytes;
}

} // namespace

int main() {
    using namespace portablekit::movie;
    const auto bytes = header();
    const auto parsed = parse_psmf_header(bytes);
    check(parsed.has_value(), "a PSMF header parses");
    if (parsed) {
        check(parsed->header_size == 0x800u && parsed->stream_size == 0x870800u, "header and stream sizes");
        check(parsed->start_time == 90000u && parsed->end_time == 0x542C85u, "presentation times, 48-bit");
        check(parsed->version == 0x34313030u, "the version's digits, first byte lowest");
        check(parsed->streams.size() == 3u, "three streams");
        check(parsed->streams[0].type() == 0 && parsed->streams[0].info[0] * 16 == 480 &&
                  parsed->streams[0].info[1] * 16 == 272,
              "the video stream and its size");
        check(parsed->streams[1].type() == 1 && parsed->streams[1].channel() == 0 && parsed->streams[1].info[0] == 2u,
              "ATRAC3plus on private stream 0, two channels");
        check(parsed->streams[2].type() == 2 && parsed->streams[2].channel() == 1, "PCM on private stream 0x11");
    }
    std::vector<std::uint8_t> wrong = bytes;
    wrong[0] = 'X';
    check(!parse_psmf_header(wrong), "no magic, no header");
    std::vector<std::uint8_t> short_table(bytes.begin(), bytes.begin() + 0xA0);
    check(!parse_psmf_header(short_table), "a stream table cut short is refused");
    if (g_failures != 0) std::printf("%d failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
