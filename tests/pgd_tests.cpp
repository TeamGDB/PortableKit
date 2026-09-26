// PGD containers (crypto/pgd.hpp) with dummy keys made up here: files built
// by the implementation itself decrypt back, random access into them works,
// and a wrong MAC, a wrong version key and a missing key are refused. No
// console key and no game data.

#include "crypto/pgd.hpp"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char *what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) ++g_failures;
}

portablekit::Key16 dummy(std::uint8_t seed) {
    portablekit::Key16 key{};
    for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::uint8_t>(seed * 31u + i * 7u + 1u);
    return key;
}

} // namespace

int main() {
    using namespace portablekit::pgd;
    static std::map<std::uint8_t, portablekit::Key16> kirk{{0x38, dummy(1)}, {0x39, dummy(2)}, {0x3A, dummy(3)}, {0x63, dummy(4)}};
    static std::map<int, portablekit::Key16> fixed{{1, dummy(5)}, {2, dummy(6)}, {3, dummy(7)}};
    const KeySource keys{[](std::uint8_t slot) -> const portablekit::Key16 * {
                             const auto it = kirk.find(slot);
                             return it != kirk.end() ? &it->second : nullptr;
                         },
                         [](int index) -> const portablekit::Key16 * {
                             const auto it = fixed.find(index);
                             return it != fixed.end() ? &it->second : nullptr;
                         }};

    std::vector<std::uint8_t> plain(3000);
    for (std::size_t i = 0; i < plain.size(); ++i) plain[i] = static_cast<std::uint8_t>(i * 13u + (i >> 8u));
    const Block desc_key = dummy(20), data_key = dummy(21), vkey = dummy(22);

    for (const bool restart : {false, true}) {
        CipherScheme scheme = active_scheme();
        scheme.block_counters_restart = restart;
        const auto file = PgdFile::build(plain, desc_key, data_key, vkey, keys, scheme, 0x38);
        check(file.size() == 0x90u + 3u * 0x400u, "three 0x400-byte blocks after the 0x90-byte header");
        const auto raw_reader = [&file](std::uint64_t offset, std::span<std::uint8_t> out) -> std::size_t {
            if (offset >= file.size()) return 0u;
            const std::size_t n = std::min<std::size_t>(out.size(), file.size() - offset);
            std::copy_n(file.begin() + static_cast<std::ptrdiff_t>(offset), n, out.begin());
            return n;
        };
        std::string error;
        auto opened = PgdFile::open(std::span(file.data(), 0x90), vkey, file.size(), keys, scheme, 0x38, error);
        check(opened.has_value(), "a PGD built here opens with its version key and a matching MAC");
        if (!opened) continue;
        check(opened->size() == plain.size() && opened->desc().block_size == 0x400u && opened->desc().data_offset == 0x90u,
              "its description decrypts to the size, block size and data offset");
        std::vector<std::uint8_t> all(plain.size());
        check(opened->read(0, all, raw_reader) == plain.size() && all == plain, "the whole file decrypts back");
        std::vector<std::uint8_t> middle(1500);
        const std::size_t got = opened->read(1000, middle, raw_reader);
        check(got == 1500u && std::equal(middle.begin(), middle.end(), plain.begin() + 1000),
              "a read across a block boundary at a random offset decrypts back");
        std::vector<std::uint8_t> tail(100);
        check(opened->read(2950, tail, raw_reader) == 50u, "a read past the end stops at the data size");

        auto tampered = file;
        tampered[0x44] ^= 1u;
        check(!PgdFile::open(std::span(tampered.data(), 0x90), vkey, tampered.size(), keys, scheme, 0x38, error) &&
                  error.find("MAC") != std::string::npos,
              "a changed header fails the MAC check");
        check(!PgdFile::open(std::span(file.data(), 0x90), dummy(99), file.size(), keys, scheme, std::nullopt, error) &&
                  error.find("valid description") != std::string::npos,
              "another version key does not give a valid description");
    }

    std::map<std::uint8_t, portablekit::Key16> none;
    const KeySource missing{[](std::uint8_t) -> const portablekit::Key16 * { return nullptr; },
                            [](int) -> const portablekit::Key16 * { return nullptr; }};
    std::vector<std::uint8_t> header(0x90, 0u);
    header[1] = 'P';
    header[2] = 'G';
    header[3] = 'D';
    std::string error;
    check(!PgdFile::open(header, vkey, 0x10000, missing, active_scheme(), std::nullopt, error) &&
              error.find("kirk.aes.") != std::string::npos,
          "a missing key is named");
    header[1] = 'X';
    check(!PgdFile::open(header, vkey, 0x10000, keys, active_scheme(), std::nullopt, error) && error == "not a PGD file",
          "a file without the magic is not PGD");
    check(candidate_schemes().size() == 1024u, "pgd-check tries 1024 schemes");

    if (g_failures != 0) std::printf("%d failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
