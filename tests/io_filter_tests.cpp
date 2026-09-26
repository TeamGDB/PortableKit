// A read filter on an open file (hle_extension::set_file_filter): PortableKit's
// own sceIoRead, sceIoLseek, sceIoLseek32 and their asynchronous forms work on
// the filtered bytes, the raw bytes stay readable, and closing the file drops
// the filter. Runs the real IoFileMgrForUser handlers on a memory stick folder
// in the temporary directory, with no game and no running kernel.

#include "hle/hle_common.hpp"
#include "portablekit/hle_extension.hpp"

#include "psprecomp/runtime.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace ext = portablekit::hle_extension;

int g_failures = 0;

void check(bool condition, const char *what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) ++g_failures;
}

constexpr std::uint32_t kPath = 0x08800000u;
constexpr std::uint32_t kBuffer = 0x08810000u;
constexpr std::uint32_t kResult = 0x08820000u;
constexpr std::size_t kRawSize = 1000u;

std::uint8_t raw_byte(std::size_t i) { return static_cast<std::uint8_t>(i * 7u + 3u); }

// The file's first half, each byte XORed with 0x5A.
class XorFilter : public ext::FileFilter {
public:
    explicit XorFilter(std::uint32_t fd) : fd_(fd) {}
    std::uint64_t size() const override { return kRawSize / 2u; }
    std::size_t read(std::uint64_t offset, std::uint8_t *destination, std::size_t length) override {
        if (offset >= size()) return 0u;
        length = static_cast<std::size_t>(std::min<std::uint64_t>(length, size() - offset));
        const std::size_t count = ext::read_open_file(fd_, offset, std::span<std::uint8_t>(destination, length));
        for (std::size_t i = 0; i < count; ++i) destination[i] ^= 0x5Au;
        return count;
    }

private:
    std::uint32_t fd_;
};

struct Io {
    psprecomp::Runtime runtime;
    std::uint32_t v1{};

    std::uint32_t nid(const char *name) {
        for (const auto &symbol : runtime.nids().all())
            if (symbol.library == "IoFileMgrForUser" && symbol.name == name) return symbol.nid;
        std::printf("no NID for %s\n", name);
        return 0u;
    }
    // Calls IoFileMgrForUser::name with a0, a1, ... (t0 onwards after a3).
    std::uint32_t call(const char *name, std::initializer_list<std::uint32_t> args) {
        psprecomp::AllegrexContext ctx{};
        unsigned i = 0;
        for (const std::uint32_t value : args) {
            ctx.set_gpr(i < 4u ? 4u + i : 8u + (i - 4u), value);
            ++i;
        }
        runtime.invoke_import("IoFileMgrForUser", nid(name), ctx);
        v1 = ctx.gpr[3];
        return ctx.gpr[2];
    }
    std::uint64_t result64() const {
        return static_cast<std::uint64_t>(runtime.memory().load32(kResult)) |
               (static_cast<std::uint64_t>(runtime.memory().load32(kResult + 4u)) << 32u);
    }
    bool guest_bytes_are(std::uint64_t filtered_offset, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i)
            if (runtime.memory().load8(kBuffer + static_cast<std::uint32_t>(i)) !=
                (raw_byte(static_cast<std::size_t>(filtered_offset) + i) ^ 0x5Au))
                return false;
        return true;
    }
};

} // namespace

int main(int, char **argv) {
    (void)argv;
    const auto stick = std::filesystem::temp_directory_path() / "portablekit_io_filter_test";
    std::filesystem::remove_all(stick);
    std::filesystem::create_directories(stick / "DATA");
    {
        std::ofstream out(stick / "DATA" / "FILE.BIN", std::ios::binary);
        for (std::size_t i = 0; i < kRawSize; ++i) out.put(static_cast<char>(raw_byte(i)));
    }

    Io io;
    io.runtime.nids().load_csv(PORTABLEKIT_NIDS_CSV);
    portablekit::HleRegistrar hle(io.runtime);
    portablekit::register_io(hle, {}, stick);
    const std::string path = "ms0:/DATA/FILE.BIN";
    for (std::size_t i = 0; i <= path.size(); ++i)
        io.runtime.memory().store8(kPath + static_cast<std::uint32_t>(i), i < path.size() ? static_cast<std::uint8_t>(path[i]) : 0u);

    const std::uint32_t fd = io.call("sceIoOpen", {kPath, 1u, 0u});
    check(static_cast<std::int32_t>(fd) > 0, "the file opens");
    io.call("sceIoLseek32", {fd, 4u, 0u});
    auto filter = std::make_shared<XorFilter>(fd);
    std::weak_ptr<XorFilter> watch = filter;
    check(ext::set_file_filter(fd, filter), "a filter is set on an open file");
    filter.reset();

    // Synchronous seeks and reads.
    check(io.call("sceIoLseek32", {fd, 0u, 1u}) == 4u, "the position is left where it was");
    check(io.call("sceIoLseek", {fd, 0u, 0u, 0u, 2u}) == kRawSize / 2u && io.v1 == 0u,
          "sceIoLseek from the end counts the filtered size");
    check(io.call("sceIoLseek32", {fd, 10u, 0u}) == 10u, "sceIoLseek32 moves in filtered bytes");
    check(io.call("sceIoRead", {fd, kBuffer, 20u}) == 20u && io.guest_bytes_are(10u, 20u),
          "sceIoRead gives the filtered bytes");
    check(io.call("sceIoLseek32", {fd, 0u, 1u}) == 30u, "and moves the position by as many");
    io.call("sceIoLseek32", {fd, static_cast<std::uint32_t>(-5), 2u});
    check(io.call("sceIoRead", {fd, kBuffer, 20u}) == 5u && io.guest_bytes_are(kRawSize / 2u - 5u, 5u),
          "a read at the end stops at the filtered size");
    check(io.call("sceIoRead", {fd, kBuffer, 20u}) == 0u, "and a read past it gives nothing");

    // The raw bytes stay readable.
    std::vector<std::uint8_t> raw(8u);
    bool raw_ok = ext::read_open_file(fd, 600u, raw) == raw.size();
    for (std::size_t i = 0; i < raw.size(); ++i) raw_ok = raw_ok && raw[i] == raw_byte(600u + i);
    check(raw_ok, "read_open_file gives the file's own bytes, past the filtered size too");

    // Asynchronous forms: the result comes with sceIoWaitAsync as usual.
    check(io.call("sceIoLseekAsync", {fd, 0u, 3u, 0u, 0u}) == 0u && io.call("sceIoWaitAsync", {fd, kResult}) == 0u &&
              io.result64() == 3u,
          "sceIoLseekAsync seeks in filtered bytes");
    check(io.call("sceIoReadAsync", {fd, kBuffer, 4u}) == 0u && io.call("sceIoWaitAsync", {fd, kResult}) == 0u &&
              io.result64() == 4u && io.guest_bytes_are(3u, 4u),
          "sceIoReadAsync reads filtered bytes");
    check(io.call("sceIoLseek32Async", {fd, 0u, 2u}) == 0u && io.call("sceIoPollAsync", {fd, kResult}) == 0u &&
              io.result64() == kRawSize / 2u,
          "sceIoLseek32Async from the end counts the filtered size");

    // Removing the filter restores the file's own size.
    check(ext::set_file_filter(fd, nullptr) && watch.expired(),
          "a null filter removes it and lets it go");
    check(io.call("sceIoLseek", {fd, 0u, 0u, 0u, 2u}) == kRawSize, "and the file's own size is back");

    // Closing drops the filter.
    filter = std::make_shared<XorFilter>(fd);
    watch = filter;
    ext::set_file_filter(fd, filter);
    filter.reset();
    check(!watch.expired(), "the file keeps its filter");
    check(io.call("sceIoClose", {fd}) == 0u && watch.expired(), "sceIoClose drops it");
    check(!ext::set_file_filter(fd, std::make_shared<XorFilter>(fd)), "a closed descriptor takes no filter");

    const std::uint32_t second = io.call("sceIoOpen", {kPath, 1u, 0u});
    filter = std::make_shared<XorFilter>(second);
    watch = filter;
    ext::set_file_filter(second, filter);
    filter.reset();
    check(io.call("sceIoCloseAsync", {second}) == 0u && watch.expired(), "sceIoCloseAsync drops it at once");
    io.call("sceIoWaitAsync", {second, kResult});

    for (std::size_t i = 0; i < 5u; ++i) io.runtime.memory().store8(kPath + static_cast<std::uint32_t>(i), "ms0:"[i]);
    io.runtime.memory().store8(kPath + 4u, 0u);
    const std::uint32_t directory = io.call("sceIoDopen", {kPath});
    check(static_cast<std::int32_t>(directory) > 0 && !ext::set_file_filter(directory, std::make_shared<XorFilter>(directory)),
          "a directory takes no filter");

    std::filesystem::remove_all(stick);
    if (g_failures != 0) std::printf("%d failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
