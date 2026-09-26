// IoFileMgrForUser and sceUmdUser: UMD access straight from the disc image
// (including raw "sce_lbn" sector files) and a host directory for ms0:.
#include "../profile.hpp"
#include "hle_common.hpp"
#include "install/user_data.hpp"
#include "kernel/fast_loading.hpp"
#include "kernel/load_trace.hpp"
#include "kernel/iso_image.hpp"

#include "psprecomp/common.hpp"
#include "portablekit/hle_extension.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <vector>

namespace portablekit {
namespace {

namespace io_error {
constexpr std::uint32_t kFileNotFound = 0x80010002u;
constexpr std::uint32_t kBadFileDescriptor = 0x80010009u;
constexpr std::uint32_t kDeviceNotFound = 0x80010013u;
constexpr std::uint32_t kInvalidArgument = 0x80010016u;
constexpr std::uint32_t kReadOnly = 0x8001001Eu;
} // namespace io_error

// The two UMD device commands that start and finish a read-ahead. What they
// mean was taken from the caller in one game's executable, which is the only
// description of them this project has:
//
//   start: input is four words, {0, first sector, 0, sector count}; the
//          four-byte output is left for the driver to fill. The game keeps
//          that word and passes it, and nothing else, to the wait below, so
//          it is the identity of the request. The game treats a negative
//          result as failure and retries; it never inspects the word itself.
//   wait:  input is that word alone, and there is no output. The game tests
//          the result with "greater than zero" and, while it is positive,
//          asks again the next frame; at zero or below it considers the
//          request finished and stops. So a positive result means "still
//          outstanding", and zero means "nothing left to do". Here the
//          image is a host file and the range was there before it was
//          asked for, so the answer is always zero.
//
// Neither command names a destination for the data, and no read of the
// sectors follows in the game that issues them, which is what a read-ahead
// into the drive's own cache looks like rather than a transfer. **This is
// not what blocks that game**: the framework answered zero to everything
// before these were implemented, which the game already read as "finished",
// and it goes to exactly the same place either way.
constexpr std::uint32_t kDiscReadAheadStart = 0x01F300A5u;
constexpr std::uint32_t kDiscReadAheadWait = 0x01F300A7u;

constexpr std::uint32_t kOpenRead = 0x0001u;
constexpr std::uint32_t kOpenWrite = 0x0002u;
constexpr std::uint32_t kOpenAppend = 0x0100u;
constexpr std::uint32_t kOpenCreate = 0x0200u;
constexpr std::uint32_t kOpenTruncate = 0x0400u;

enum class Device { Disc, MemoryStick, Unknown };

struct OpenFile {
    enum class Kind { Disc, Host, Directory, Failed } kind{};
    std::string path;
    std::uint64_t disc_offset{};  // absolute image offset of byte 0
    std::uint64_t size{};
    std::uint64_t position{};
    // The raw disc device counts 2048-byte sectors, not bytes: a seek moves
    // to a sector, a read asks for that many sectors and answers with how
    // many it got. Everything else here counts bytes, so size and position
    // are sectors for this one kind of handle and the conversion happens
    // where the image is touched.
    bool sector_units{};
    std::unique_ptr<std::fstream> host;
    // What the game reads instead of the file's bytes, if anything
    // (hle_extension::set_file_filter): size and position then count its
    // bytes, and the file's own size is kept here.
    std::shared_ptr<hle_extension::FileFilter> filter;
    std::uint64_t unfiltered_size{};
    // Asynchronous calls (sceIo*Async): the result of the one operation in
    // flight, kept until sceIoWaitAsync or sceIoPollAsync collects it. The
    // operation itself has already run: the files are the host's, and a
    // read from them is done by the time a game could look. A handle whose
    // asynchronous open failed, or that sceIoCloseAsync closed, lives until
    // its result is collected, and no longer.
    // A directory's entries, read at sceIoDopen and handed out one at a time
    // by sceIoDread.
    struct DirectoryEntry {
        std::string name;
        bool directory{};
        std::uint64_t size{};
        std::uint32_t lba{};
    };
    std::vector<DirectoryEntry> entries;
    std::size_t next_entry{};
    std::optional<std::int64_t> async_result;
    bool release_after_async{};
    SceUID async_callback{};
    std::uint32_t async_callback_argument{};
};

// A read-ahead the game asked the drive for: a range of sectors it wants in
// the drive's cache before it reads them. Nothing is copied anywhere — the
// request names no destination — so all that is kept is what was asked for,
// and the identity the two halves of the request are joined by.
struct DiscReadAhead {
    std::uint32_t lba{};
    std::uint32_t sectors{};
};

struct IoState {
    std::unique_ptr<IsoImage> disc;
    std::filesystem::path memory_stick;
    std::map<std::uint32_t, OpenFile> files;
    std::uint32_t next_fd{3u};
    std::map<std::uint32_t, DiscReadAhead> read_ahead;
    std::uint32_t next_read_ahead{1u};
};

IoState &io() {
    static IoState state;
    return state;
}

// The drive: one image, present from the start, never ejected.
std::uint32_t drive_status() {
    constexpr std::uint32_t kNotPresent = 0x01u;
    constexpr std::uint32_t kPresent = 0x02u;
    constexpr std::uint32_t kReady = 0x10u;
    constexpr std::uint32_t kReadable = 0x20u;
    return io().disc ? (kPresent | kReady | kReadable) : kNotPresent;
}

// The one callback a game may register to hear about the drive; 0 for none.
SceUID &umd_callback() {
    static SceUID callback = 0;
    return callback;
}

struct SplitPath {
    Device device{Device::Unknown};
    std::string path;  // without device, '/' separated, no leading '/'
};

SplitPath split_path(const std::string &full) {
    SplitPath result;
    const auto colon = full.find(':');
    std::string device = colon == std::string::npos ? "disc0" : full.substr(0, colon);
    std::transform(device.begin(), device.end(), device.begin(), [](unsigned char c) { return std::tolower(c); });
    if (device == "disc0" || device == "umd0" || device == "umd1" || device == "isofs0") result.device = Device::Disc;
    else if (device == "ms0" || device == "fatms0") result.device = Device::MemoryStick;

    std::string rest = colon == std::string::npos ? full : full.substr(colon + 1u);
    std::replace(rest.begin(), rest.end(), '\\', '/');
    std::vector<std::string> parts;
    std::size_t start = 0u;
    while (start <= rest.size()) {
        const auto end = rest.find('/', start);
        std::string part = rest.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        if (end == std::string::npos) break;
        start = end + 1u;
    }
    for (std::size_t i = 0; i < parts.size(); ++i) result.path += (i == 0 ? "" : "/") + parts[i];
    return result;
}

// "sce_lbn0x5e0_size0x1000" -> {lba, size}
std::optional<std::pair<std::uint64_t, std::uint64_t>> parse_lbn_path(const std::string &path) {
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    const auto lbn = lower.find("sce_lbn");
    const auto size = lower.find("_size");
    if (lbn != 0u || size == std::string::npos) return std::nullopt;
    const auto parse = [](const std::string &text) -> std::optional<std::uint64_t> {
        std::string digits = text;
        int base = 10;
        if (digits.starts_with("0x")) {
            digits = digits.substr(2);
            base = 16;
        }
        std::uint64_t value{};
        const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value, base);
        if (ec != std::errc{} || ptr != digits.data() + digits.size()) return std::nullopt;
        return value;
    };
    const auto lba = parse(lower.substr(7, size - 7));
    const auto length = parse(lower.substr(size + 5));
    if (!lba || !length) return std::nullopt;
    return std::make_pair(*lba, *length);
}

std::filesystem::path host_path(const std::string &path) { return io().memory_stick / path; }

bool trace_io() {
    static const bool enabled = portablekit::env("TRACE_IO") != nullptr;
    return enabled;
}

std::int64_t open_file(const std::string &full_path, std::uint32_t flags) {
    const SplitPath split = split_path(full_path);
    OpenFile file;
    file.path = full_path;
    if (split.device == Device::Disc) {
        if (!io().disc) return static_cast<std::int32_t>(io_error::kDeviceNotFound);
        if ((flags & kOpenWrite) != 0u) return static_cast<std::int32_t>(io_error::kReadOnly);
        if (split.path.empty()) {
            // The device itself, with no path: the whole image as a stream of
            // sectors, which is how a game reads the disc without going
            // through the filesystem. This has to be tested before find(),
            // because an empty path finds the root directory and the game
            // then gets a directory handle where it wanted the disc.
            file.kind = OpenFile::Kind::Disc;
            file.sector_units = true;
            file.size = io().disc->size_bytes() / IsoImage::kSectorSize;
        } else if (const auto lbn = parse_lbn_path(split.path)) {
            file.kind = OpenFile::Kind::Disc;
            file.disc_offset = lbn->first * IsoImage::kSectorSize;
            file.size = lbn->second;
        } else if (const auto entry = io().disc->find(split.path)) {
            file.kind = entry->directory ? OpenFile::Kind::Directory : OpenFile::Kind::Disc;
            file.disc_offset = static_cast<std::uint64_t>(entry->lba) * IsoImage::kSectorSize;
            file.size = entry->size;
        } else {
            return static_cast<std::int32_t>(io_error::kFileNotFound);
        }
    } else if (split.device == Device::MemoryStick) {
        const auto path = host_path(split.path);
        std::error_code ec;
        const bool exists = std::filesystem::exists(path, ec);
        if (!exists && (flags & kOpenCreate) == 0u) return static_cast<std::int32_t>(io_error::kFileNotFound);
        std::ios::openmode mode = std::ios::binary | std::ios::in;
        if ((flags & kOpenWrite) != 0u) {
            std::filesystem::create_directories(path.parent_path(), ec);
            mode |= std::ios::out;
            if (!exists || (flags & kOpenTruncate) != 0u) mode |= std::ios::trunc;
            if ((flags & kOpenAppend) != 0u) mode |= std::ios::app;
        }
        file.kind = OpenFile::Kind::Host;
        file.host = std::make_unique<std::fstream>(path, mode);
        if (!*file.host) return static_cast<std::int32_t>(io_error::kFileNotFound);
        file.size = std::filesystem::exists(path, ec) ? std::filesystem::file_size(path, ec) : 0u;
    } else {
        return static_cast<std::int32_t>(io_error::kDeviceNotFound);
    }
    const std::uint32_t fd = io().next_fd++;
    io().files.emplace(fd, std::move(file));
    return fd;
}

void write_date_time(psprecomp::GuestMemory &memory, std::uint32_t address, std::time_t time) {
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif
    memory.store16(address, static_cast<std::uint16_t>(tm.tm_year + 1900));
    memory.store16(address + 2u, static_cast<std::uint16_t>(tm.tm_mon + 1));
    memory.store16(address + 4u, static_cast<std::uint16_t>(tm.tm_mday));
    memory.store16(address + 6u, static_cast<std::uint16_t>(tm.tm_hour));
    memory.store16(address + 8u, static_cast<std::uint16_t>(tm.tm_min));
    memory.store16(address + 10u, static_cast<std::uint16_t>(tm.tm_sec));
    memory.store32(address + 12u, 0u);
}

// SceIoStat: mode, attr, size(64), ctime, atime, mtime, private[6].
void write_stat(psprecomp::GuestMemory &memory, std::uint32_t address, bool directory, std::uint64_t size,
                std::uint32_t lba) {
    for (std::uint32_t i = 0; i < 88u; ++i) memory.store8(address + i, 0u);
    memory.store32(address, (directory ? 0x1000u : 0x2000u) | 0x1FFu);
    memory.store32(address + 4u, (directory ? 0x10u : 0x20u) | 0x7u);
    store64(memory, address + 8u, size);
    const std::time_t now = std::time(nullptr);
    write_date_time(memory, address + 16u, now);
    write_date_time(memory, address + 32u, now);
    write_date_time(memory, address + 48u, now);
    memory.store32(address + 64u, lba);
}

} // namespace

std::size_t read_open_file(std::uint32_t fd, std::uint64_t offset, std::uint8_t *output, std::size_t size) {
    const auto found = io().files.find(fd);
    if (found == io().files.end()) return 0u;
    OpenFile &file = found->second;
    const std::span<std::uint8_t> out(output, size);
    if (file.kind == OpenFile::Kind::Disc) {
        const std::uint64_t file_size = file.filter ? file.unfiltered_size : file.size;
        if (!io().disc || offset >= file_size) return 0u;
        return io().disc->read(file.disc_offset + offset, out.first(std::min<std::uint64_t>(size, file_size - offset)));
    }
    if (file.kind == OpenFile::Kind::Host) {
        file.host->clear();
        file.host->seekg(static_cast<std::streamoff>(offset));
        file.host->read(reinterpret_cast<char *>(output), static_cast<std::streamsize>(size));
        const auto count = static_cast<std::size_t>(file.host->gcount());
        file.host->clear();
        return count;
    }
    return 0u;
}

bool set_open_file_filter(std::uint32_t fd, std::shared_ptr<hle_extension::FileFilter> filter) {
    const auto found = io().files.find(fd);
    if (found == io().files.end()) return false;
    OpenFile &file = found->second;
    if ((file.kind != OpenFile::Kind::Disc && file.kind != OpenFile::Kind::Host) || file.sector_units) return false;
    if (filter) {
        if (!file.filter) file.unfiltered_size = file.size;
        file.filter = std::move(filter);
        file.size = file.filter->size();
    } else if (file.filter) {
        file.filter.reset();
        file.size = file.unfiltered_size;
    }
    if (trace_io())
        std::cerr << "[io] filter fd=" << fd << " " << file.path << (file.filter ? " set, size " : " removed, size ")
                  << file.size << "\n";
    return true;
}

// sceIoRead through a file's filter: `requested` bytes of what the filter
// gives at the position, which moves by as many.
static std::uint32_t read_filtered(Runtime &rt, std::uint32_t fd, OpenFile &file, std::uint32_t address,
                                   std::uint32_t requested) {
    const std::uint64_t available = file.position < file.size ? file.size - file.position : 0u;
    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(std::min<std::uint64_t>(requested, available)));
    const std::size_t count = buffer.empty() ? 0u : file.filter->read(file.position, buffer.data(), buffer.size());
    buffer.resize(std::min(count, buffer.size()));
    rt.memory().copy_in(address, buffer);
    if (file.kind == OpenFile::Kind::Disc) {
        load_trace::note_disc_read(buffer.size());
        fast_loading::note_disc_read();
    } else {
        load_trace::note_memory_stick_read(buffer.size());
    }
    if (trace_io())
        std::cerr << "[io] read fd=" << fd << " " << file.path << " filtered offset=" << file.position
                  << " got=" << buffer.size() << " bytes -> " << psprecomp::hex32(address) << "\n";
    file.position += buffer.size();
    return static_cast<std::uint32_t>(buffer.size());
}

std::int64_t read_guest_file(const std::string &path, std::uint64_t offset, std::uint8_t *output, std::size_t size) {
    const std::int64_t fd = open_file(path, kOpenRead);
    if (fd < 0) return fd;
    const std::size_t count = read_open_file(static_cast<std::uint32_t>(fd), offset, output, size);
    io().files.erase(static_cast<std::uint32_t>(fd));
    return static_cast<std::int64_t>(count);
}

namespace {

// The work of sceIoRead, sceIoWrite and sceIoLseek, shared with their
// asynchronous forms.
std::uint32_t read_file_op(Runtime &rt, std::uint32_t fd, std::uint32_t address, std::uint32_t requested) {
    auto found = io().files.find(fd);
    if (found == io().files.end() ||
        (found->second.kind != OpenFile::Kind::Disc && found->second.kind != OpenFile::Kind::Host))
        return io_error::kBadFileDescriptor;
    OpenFile &file = found->second;
    if (file.filter) return read_filtered(rt, fd, file, address, requested);
    std::vector<std::uint8_t> buffer;
    std::uint32_t result = 0u;
    if (file.kind == OpenFile::Kind::Disc) {
        const std::uint64_t available = file.position < file.size ? file.size - file.position : 0u;
        requested = static_cast<std::uint32_t>(std::min<std::uint64_t>(requested, available));
        const std::uint64_t unit = file.sector_units ? IsoImage::kSectorSize : 1u;
        buffer.resize(static_cast<std::size_t>(requested) * unit);
        const std::size_t count = io().disc->read(file.disc_offset + file.position * unit, buffer);
        buffer.resize(count);
        // A raw read answers in sectors, and a partial sector is not one.
        result = static_cast<std::uint32_t>(count / unit);
    } else {
        buffer.resize(requested);
        file.host->clear();
        file.host->seekg(static_cast<std::streamoff>(file.position));
        file.host->read(reinterpret_cast<char *>(buffer.data()), requested);
        buffer.resize(static_cast<std::size_t>(file.host->gcount()));
        result = static_cast<std::uint32_t>(buffer.size());
    }
    rt.memory().copy_in(address, buffer);
    // A disc read is what tells a load from play (kernel/fast_loading.hpp).
    if (file.kind == OpenFile::Kind::Disc) {
        load_trace::note_disc_read(buffer.size());
        fast_loading::note_disc_read();
    } else {
        load_trace::note_memory_stick_read(buffer.size());
    }
    if (trace_io())
        std::cerr << "[io] read fd=" << fd << " " << file.path
                  << (file.sector_units ? " sector=" : " offset=") << file.position << " got=" << result
                  << (file.sector_units ? " sectors" : " bytes") << " -> " << psprecomp::hex32(address) << "\n";
    file.position += file.kind == OpenFile::Kind::Disc && file.sector_units ? result : buffer.size();
    return result;
}

std::uint32_t write_file_op(Runtime &rt, std::uint32_t fd, std::uint32_t address, std::uint32_t size) {
    if (fd == 1u || fd == 2u) {
        std::string text(size, '\0');
        for (std::uint32_t i = 0; i < size; ++i) text[i] = static_cast<char>(rt.memory().load8(address + i));
        std::cerr << "[guest] " << text;
        return size;
    }
    auto found = io().files.find(fd);
    if (found == io().files.end() || found->second.kind != OpenFile::Kind::Host) {
        return io_error::kBadFileDescriptor;
    }
    std::vector<char> data(size);
    for (std::uint32_t i = 0; i < size; ++i) data[i] = static_cast<char>(rt.memory().load8(address + i));
    OpenFile &file = found->second;
    file.host->clear();
    file.host->seekp(static_cast<std::streamoff>(file.position));
    file.host->write(data.data(), size);
    file.host->flush();
    file.position += size;
    file.size = std::max(file.size, file.position);
    return size;
}

std::int64_t lseek_op(std::uint32_t fd, std::int64_t offset, std::uint32_t whence) {
    auto found = io().files.find(fd);
    if (found == io().files.end()) return static_cast<std::int32_t>(io_error::kBadFileDescriptor);
    OpenFile &file = found->second;
    std::int64_t base = 0;
    switch (whence) {
    case 0u: base = 0; break;
    case 1u: base = static_cast<std::int64_t>(file.position); break;
    case 2u: base = static_cast<std::int64_t>(file.size); break;
    default:
        return static_cast<std::int32_t>(io_error::kInvalidArgument);
    }
    const std::int64_t target = base + offset;
    if (target < 0) {
        return static_cast<std::int32_t>(io_error::kInvalidArgument);
    }
    file.position = static_cast<std::uint64_t>(target);
    if (trace_io())
        std::cerr << "[io] lseek fd=" << fd << " " << file.path << " -> " << file.position << "\n";
    return static_cast<std::int64_t>(file.position);
}

namespace async_error {
constexpr std::uint32_t kBusy = 0x80020329u;     // SCE_KERNEL_ERROR_ASYNC_BUSY
constexpr std::uint32_t kNoAsync = 0x8002032Au;  // SCE_KERNEL_ERROR_NOASYNC
} // namespace async_error

// Records the result of an operation started on `fd` and tells the handle's
// callback, if it has one, as a PSP does when the operation completes.
void complete_async(OpenFile &file, std::int64_t result) {
    file.async_result = result;
    if (file.async_callback != 0) kernel().notify_callback(file.async_callback, file.async_callback_argument);
}

// The handle an asynchronous call names, if it can start one: it exists and
// has no result waiting to be collected.
OpenFile *async_handle(std::uint32_t fd, std::uint32_t &error) {
    const auto found = io().files.find(fd);
    if (found == io().files.end()) {
        error = io_error::kBadFileDescriptor;
        return nullptr;
    }
    if (found->second.async_result || found->second.release_after_async) {
        error = async_error::kBusy;
        return nullptr;
    }
    return &found->second;
}

// Hands over the result of the operation in flight: the call's own v0 is 0
// and the result goes to *result (64-bit). A handle opened or closed
// asynchronously for nothing more goes away once its result is taken.
std::uint32_t collect_async(Runtime &rt, std::uint32_t fd, std::uint32_t result_address) {
    const auto found = io().files.find(fd);
    if (found == io().files.end()) return io_error::kBadFileDescriptor;
    if (!found->second.async_result) return async_error::kNoAsync;
    const std::int64_t result = *found->second.async_result;
    found->second.async_result.reset();
    if (result_address != 0u) store64(rt.memory(), result_address, static_cast<std::uint64_t>(result));
    if (trace_io()) std::cerr << "[io] async result fd=" << fd << " -> " << result << "\n";
    if (found->second.release_after_async) io().files.erase(found);
    return 0u;
}

// sceIoIoctl(fd, command, in, in size, out, out size). What a disc file
// answers, as the games that ask use it (traced with <prefix>_TRACE_IO):
//   0x01020006  its first sector on the disc (4 bytes out). Patapon asks it
//               of each archive it opens, before it seeks and reads.
//   0x01020007  its size in bytes (4 bytes out).
//   0x01010005  seek: 16 bytes in, a 64-bit byte offset and a 32-bit whence
//               (0 from the start, 1 from here, 2 from the end). Patapon
//               seeks its archives so, all from the start, then reads.
// Anything else is logged once and answered 0, as before.
std::uint32_t ioctl_op(Runtime &rt, AllegrexContext &ctx) {
    const std::uint32_t fd = arg(ctx, 0);
    const std::uint32_t command = arg(ctx, 1);
    const std::uint32_t input = arg(ctx, 2);
    const std::uint32_t input_size = arg(ctx, 3);
    const std::uint32_t output = ctx.gpr[8];        // t0
    const std::uint32_t output_size = ctx.gpr[9];   // t1
    auto &memory = rt.memory();
    const auto found = io().files.find(fd);
    if (found == io().files.end()) return io_error::kBadFileDescriptor;
    const OpenFile &file = found->second;
    if (file.kind == OpenFile::Kind::Disc && command == 0x01010005u && input != 0u && input_size >= 12u) {
        const auto offset = static_cast<std::int64_t>(static_cast<std::uint64_t>(memory.load32(input)) |
                                                      (static_cast<std::uint64_t>(memory.load32(input + 4u)) << 32u));
        const std::int64_t position = lseek_op(fd, offset, memory.load32(input + 8u));
        return position < 0 ? static_cast<std::uint32_t>(position) : 0u;
    }
    if (file.kind == OpenFile::Kind::Disc && !file.sector_units && output != 0u && output_size >= 4u) {
        if (command == 0x01020006u) {
            const auto sector = static_cast<std::uint32_t>(file.disc_offset / IsoImage::kSectorSize);
            memory.store32(output, sector);
            if (trace_io()) std::cerr << "[io] ioctl " << file.path << " first sector -> " << sector << "\n";
            return 0u;
        }
        if (command == 0x01020007u) {
            memory.store32(output, static_cast<std::uint32_t>(file.size));
            if (trace_io()) std::cerr << "[io] ioctl " << file.path << " size -> " << file.size << "\n";
            return 0u;
        }
    }
    std::string description = "[io] ioctl fd=" + std::to_string(fd) + " " + file.path + " cmd=" +
                               psprecomp::hex32(command) + " in=" + std::to_string(input_size) + " bytes";
    if (input != 0u && input_size != 0u && input_size <= 64u) {
        description += ":";
        for (std::uint32_t i = 0; i < input_size; ++i)
            description += " " + psprecomp::hex32(memory.load8(input + i)).substr(8u);
    }
    description += " out=" + psprecomp::hex32(output) + "/" + std::to_string(output_size);
    log_once("ioctl-" + psprecomp::hex32(command), description + " (unhandled, returning 0)");
    return 0u;
}

void register_async_io(HleRegistrar &hle) {
    hle.add("IoFileMgrForUser", "sceIoIoctlAsync", [](Runtime &rt, AllegrexContext &ctx) {
        std::uint32_t error = 0u;
        OpenFile *file = async_handle(arg(ctx, 0), error);
        if (file == nullptr) {
            kernel().finish(ctx, error);
            return;
        }
        const std::uint32_t result = ioctl_op(rt, ctx);
        complete_async(*file, static_cast<std::int32_t>(result));
        kernel().finish(ctx, 0u);
    });
    // sceIoOpenAsync(path, flags, mode): a handle at once; the open's own
    // result (the handle, or an error) when the game collects it.
    hle.add("IoFileMgrForUser", "sceIoOpenAsync", [](Runtime &rt, AllegrexContext &ctx) {
        const std::string path = read_cstring(rt.memory(), arg(ctx, 0), 256u);
        const std::int64_t fd = open_file(path, arg(ctx, 1));
        if (trace_io() || fd < 0)
            std::cerr << "[io] open async " << path << " flags=" << psprecomp::hex32(arg(ctx, 1)) << " -> "
                      << psprecomp::hex32(static_cast<std::uint32_t>(fd)) << "\n";
        if (fd >= 0) {
            complete_async(io().files.at(static_cast<std::uint32_t>(fd)), fd);
            kernel().finish(ctx, static_cast<std::uint32_t>(fd));
            return;
        }
        // A handle that exists only to report the failure.
        OpenFile failed;
        failed.kind = OpenFile::Kind::Failed;
        failed.path = path;
        failed.release_after_async = true;
        const std::uint32_t handle = io().next_fd++;
        failed.async_result = fd;
        io().files.emplace(handle, std::move(failed));
        kernel().finish(ctx, handle);
    });
    hle.add("IoFileMgrForUser", "sceIoCloseAsync", [](Runtime &, AllegrexContext &ctx) {
        std::uint32_t error = 0u;
        OpenFile *file = async_handle(arg(ctx, 0), error);
        if (file == nullptr) {
            kernel().finish(ctx, error);
            return;
        }
        file->host.reset();
        file->filter.reset();
        file->kind = OpenFile::Kind::Failed;
        file->release_after_async = true;
        complete_async(*file, 0);
        kernel().finish(ctx, 0u);
    });
    hle.add("IoFileMgrForUser", "sceIoReadAsync", [](Runtime &rt, AllegrexContext &ctx) {
        std::uint32_t error = 0u;
        OpenFile *file = async_handle(arg(ctx, 0), error);
        if (file == nullptr) {
            kernel().finish(ctx, error);
            return;
        }
        const std::uint32_t result = read_file_op(rt, arg(ctx, 0), arg(ctx, 1), arg(ctx, 2));
        complete_async(*file, static_cast<std::int32_t>(result));
        kernel().finish(ctx, 0u);
    });
    hle.add("IoFileMgrForUser", "sceIoWriteAsync", [](Runtime &rt, AllegrexContext &ctx) {
        std::uint32_t error = 0u;
        OpenFile *file = async_handle(arg(ctx, 0), error);
        if (file == nullptr) {
            kernel().finish(ctx, error);
            return;
        }
        const std::uint32_t result = write_file_op(rt, arg(ctx, 0), arg(ctx, 1), arg(ctx, 2));
        complete_async(*file, static_cast<std::int32_t>(result));
        kernel().finish(ctx, 0u);
    });
    // sceIoLseekAsync(fd, SceOff offset, whence): offset in a2:a3, whence in t0.
    hle.add("IoFileMgrForUser", "sceIoLseekAsync", [](Runtime &, AllegrexContext &ctx) {
        std::uint32_t error = 0u;
        OpenFile *file = async_handle(arg(ctx, 0), error);
        if (file == nullptr) {
            kernel().finish(ctx, error);
            return;
        }
        complete_async(*file, lseek_op(arg(ctx, 0), static_cast<std::int64_t>(arg64(ctx, 2)), arg(ctx, 4)));
        kernel().finish(ctx, 0u);
    });
    hle.add("IoFileMgrForUser", "sceIoLseek32Async", [](Runtime &, AllegrexContext &ctx) {
        std::uint32_t error = 0u;
        OpenFile *file = async_handle(arg(ctx, 0), error);
        if (file == nullptr) {
            kernel().finish(ctx, error);
            return;
        }
        const auto offset = static_cast<std::int64_t>(static_cast<std::int32_t>(arg(ctx, 1)));
        complete_async(*file, lseek_op(arg(ctx, 0), offset, arg(ctx, 2)));
        kernel().finish(ctx, 0u);
    });
    // Every operation has finished by the time it is asked about, so waiting
    // and polling are the same: 0, with the result in *res. Poll answers 1
    // only while something is in flight, which here is never.
    const auto collect = [](Runtime &rt, AllegrexContext &ctx) {
        kernel().finish(ctx, collect_async(rt, arg(ctx, 0), arg(ctx, 1)));
    };
    hle.add("IoFileMgrForUser", "sceIoWaitAsync", collect);
    hle.add("IoFileMgrForUser", "sceIoWaitAsyncCB", collect);
    hle.add("IoFileMgrForUser", "sceIoPollAsync", collect);
    // (fd, poll, res): the same whether it would wait or poll.
    hle.add("IoFileMgrForUser", "sceIoGetAsyncStat", [](Runtime &rt, AllegrexContext &ctx) {
        kernel().finish(ctx, collect_async(rt, arg(ctx, 0), arg(ctx, 2)));
    });
    hle.add("IoFileMgrForUser", "sceIoSetAsyncCallback", [](Runtime &, AllegrexContext &ctx) {
        const auto found = io().files.find(arg(ctx, 0));
        if (found == io().files.end()) {
            kernel().finish(ctx, io_error::kBadFileDescriptor);
            return;
        }
        found->second.async_callback = static_cast<SceUID>(arg(ctx, 1));
        found->second.async_callback_argument = arg(ctx, 2);
        // An operation that finished before the callback was set (they all
        // finish when issued here; on a PSP an asynchronous open is still
        // running when the game sets its callback next) is reported now.
        // Patapon opens its files asynchronously, sets the callback, and
        // waits for it.
        if (found->second.async_result && found->second.async_callback != 0)
            kernel().notify_callback(found->second.async_callback, found->second.async_callback_argument);
        kernel().finish(ctx, 0u);
    });
    // The priority of the thread a PSP runs asynchronous calls on; there is
    // no such thread here. (fd, priority); fd -1 sets the default.
    hle.add("IoFileMgrForUser", "sceIoChangeAsyncPriority", [](Runtime &, AllegrexContext &ctx) {
        const std::uint32_t fd = arg(ctx, 0);
        kernel().finish(ctx, fd == 0xFFFFFFFFu || io().files.contains(fd) ? 0u : io_error::kBadFileDescriptor);
    });
}

} // namespace

void register_io(HleRegistrar &hle, const std::filesystem::path &disc_image, const std::filesystem::path &memory_stick) {
    if (!disc_image.empty()) io().disc = std::make_unique<IsoImage>(disc_image);
    io().memory_stick = memory_stick;

    hle.add("IoFileMgrForUser", "sceIoOpen", [](Runtime &rt, AllegrexContext &ctx) {
        const std::string path = read_cstring(rt.memory(), arg(ctx, 0), 256u);
        const std::int64_t fd = open_file(path, arg(ctx, 1));
        if (trace_io() || fd < 0)
            std::cerr << "[io] open " << path << " flags=" << psprecomp::hex32(arg(ctx, 1)) << " -> "
                      << psprecomp::hex32(static_cast<std::uint32_t>(fd)) << "\n";
        kernel().finish(ctx, static_cast<std::uint32_t>(fd));
    });
    hle.add("IoFileMgrForUser", "sceIoClose", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, io().files.erase(arg(ctx, 0)) != 0u ? 0u : io_error::kBadFileDescriptor);
    });
    hle.add("IoFileMgrForUser", "sceIoRead", [](Runtime &rt, AllegrexContext &ctx) {
        kernel().finish(ctx, read_file_op(rt, arg(ctx, 0), arg(ctx, 1), arg(ctx, 2)));
    });
    hle.add("IoFileMgrForUser", "sceIoWrite", [](Runtime &rt, AllegrexContext &ctx) {
        kernel().finish(ctx, write_file_op(rt, arg(ctx, 0), arg(ctx, 1), arg(ctx, 2)));
    });
    hle.add("IoFileMgrForUser", "sceIoLseek", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish64(ctx, static_cast<std::uint64_t>(lseek_op(arg(ctx, 0), static_cast<std::int64_t>(arg64(ctx, 2)), arg(ctx, 4))));
    });
    // sceIoLseek32(fd, offset, whence): a 32-bit offset and result.
    hle.add("IoFileMgrForUser", "sceIoLseek32", [](Runtime &, AllegrexContext &ctx) {
        const auto offset = static_cast<std::int64_t>(static_cast<std::int32_t>(arg(ctx, 1)));
        kernel().finish(ctx, static_cast<std::uint32_t>(lseek_op(arg(ctx, 0), offset, arg(ctx, 2))));
    });
    register_async_io(hle);
    hle.add("IoFileMgrForUser", "sceIoGetstat", [](Runtime &rt, AllegrexContext &ctx) {
        const std::string path = read_cstring(rt.memory(), arg(ctx, 0), 256u);
        const SplitPath split = split_path(path);
        std::uint32_t result = io_error::kFileNotFound;
        if (split.device == Device::Disc && io().disc) {
            if (const auto entry = io().disc->find(split.path)) {
                write_stat(rt.memory(), arg(ctx, 1), entry->directory, entry->size, entry->lba);
                result = 0u;
            }
        } else if (split.device == Device::MemoryStick) {
            std::error_code ec;
            const auto host = host_path(split.path);
            if (std::filesystem::exists(host, ec)) {
                const bool directory = std::filesystem::is_directory(host, ec);
                write_stat(rt.memory(), arg(ctx, 1), directory, directory ? 0u : std::filesystem::file_size(host, ec), 0u);
                result = 0u;
            }
        }
        if (trace_io() || result != 0u)
            std::cerr << "[io] getstat " << path << " -> " << psprecomp::hex32(result) << "\n";
        kernel().finish(ctx, result);
    });
    hle.add("IoFileMgrForUser", "sceIoDopen", [](Runtime &rt, AllegrexContext &ctx) {
        const std::string path = read_cstring(rt.memory(), arg(ctx, 0), 256u);
        const SplitPath split = split_path(path);
        bool exists = false;
        if (split.device == Device::Disc && io().disc) {
            const auto entry = io().disc->find(split.path);
            exists = split.path.empty() || (entry && entry->directory);
        } else if (split.device == Device::MemoryStick) {
            std::error_code ec;
            exists = std::filesystem::is_directory(host_path(split.path), ec);
        }
        if (!exists) {
            std::cerr << "[io] dopen " << path << " -> not found\n";
            kernel().finish(ctx, io_error::kFileNotFound);
            return;
        }
        OpenFile file;
        file.kind = OpenFile::Kind::Directory;
        file.path = path;
        // "." and ".." first in a memory stick folder, as FAT has them (not
        // at its root). The disc lists none: Vice City Stories walks
        // PSP_GAME recursively and descends into every folder it is given,
        // "." included.
        const bool root = split.path.empty();
        if (!root && split.device == Device::MemoryStick) {
            file.entries.push_back({".", true, 0u, 0u});
            file.entries.push_back({"..", true, 0u, 0u});
        }
        if (split.device == Device::Disc) {
            for (const std::string &name : io().disc->list(split.path)) {
                const auto entry = io().disc->find(root ? name : split.path + "/" + name);
                if (entry) file.entries.push_back({name, entry->directory, entry->size, entry->lba});
            }
        } else {
            std::error_code ec;
            for (const auto &entry : std::filesystem::directory_iterator(host_path(split.path), ec)) {
                std::error_code size_ec;
                const bool directory = entry.is_directory(size_ec);
                file.entries.push_back({install::path_to_utf8(entry.path().filename()), directory,
                                        directory ? 0u : entry.file_size(size_ec), 0u});
            }
        }
        const std::uint32_t fd = io().next_fd++;
        io().files.emplace(fd, std::move(file));
        kernel().finish(ctx, fd);
    });
    // SceIoDirent: a SceIoStat (88 bytes), then the name (256), then d_private.
    // 1 for an entry, 0 past the last.
    hle.add("IoFileMgrForUser", "sceIoDread", [](Runtime &rt, AllegrexContext &ctx) {
        const auto found = io().files.find(arg(ctx, 0));
        if (found == io().files.end() || found->second.kind != OpenFile::Kind::Directory) {
            kernel().finish(ctx, io_error::kBadFileDescriptor);
            return;
        }
        OpenFile &directory = found->second;
        if (directory.next_entry >= directory.entries.size()) {
            kernel().finish(ctx, 0u);
            return;
        }
        const OpenFile::DirectoryEntry &entry = directory.entries[directory.next_entry++];
        auto &memory = rt.memory();
        const std::uint32_t dirent = arg(ctx, 1);
        write_stat(memory, dirent, entry.directory, entry.size, entry.lba);
        for (std::uint32_t i = 0; i < 256u; ++i)
            memory.store8(dirent + 88u + i, i < entry.name.size() ? static_cast<std::uint8_t>(entry.name[i]) : 0u);
        if (trace_io()) std::cerr << "[io] dread " << directory.path << " -> " << entry.name << "\n";
        kernel().finish(ctx, 1u);
    });
    hle.add("IoFileMgrForUser", "sceIoDclose", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, io().files.erase(arg(ctx, 0)) != 0u ? 0u : io_error::kBadFileDescriptor);
    });
    hle.add("IoFileMgrForUser", "sceIoRename", [](Runtime &rt, AllegrexContext &ctx) {
        const SplitPath from = split_path(read_cstring(rt.memory(), arg(ctx, 0), 256u));
        const SplitPath to = split_path(read_cstring(rt.memory(), arg(ctx, 1), 256u));
        if (from.device != Device::MemoryStick || to.device != Device::MemoryStick) {
            kernel().finish(ctx, io_error::kReadOnly);
            return;
        }
        std::error_code ec;
        std::filesystem::rename(host_path(from.path), host_path(to.path), ec);
        kernel().finish(ctx, ec ? io_error::kFileNotFound : 0u);
    });
    // sceIoIoctl(fd, command, in, in_size, out, out_size). What a game asks of
    // a file this way is device-specific and undocumented here, so nothing is
    // guessed at: every call is reported once with the buffer the game filled
    // in, which is how the command should be worked out.
    hle.add("IoFileMgrForUser", "sceIoIoctl", [](Runtime &rt, AllegrexContext &ctx) {
        kernel().finish(ctx, ioctl_op(rt, ctx));
    });

    hle.add("IoFileMgrForUser", "sceIoDevctl", [](Runtime &rt, AllegrexContext &ctx) {
        const std::string device = read_cstring(rt.memory(), arg(ctx, 0), 64u);
        const std::uint32_t command = arg(ctx, 1);
        const std::uint32_t input = arg(ctx, 2);
        const std::uint32_t output = arg(ctx, 4);
        const std::uint32_t output_length = arg(ctx, 5);
        auto &memory = rt.memory();
        switch (command) {
        case 0x02015804u:    // register memory stick insert/eject callback (ms0:)
        case 0x02415821u: {  // the same for fatms0:
            // The game waits for the first notification before it continues, and
            // refuses to save while it has not been told a card is inserted, so
            // report the card as present right away.
            const std::uint32_t callback = input != 0u ? memory.load32(input) : 0u;
            if (callback != 0u) kernel().notify_callback(static_cast<SceUID>(callback), 1u);
            break;
        }
        case 0x02015805u: // unregister
        case 0x02415822u:
            break;
        case 0x02025801u: // memory stick state: 4 = inserted and ready
        case 0x02025806u: // memory stick inserted
            if (output != 0u && output_length >= 4u) memory.store32(output, command == 0x02025801u ? 4u : 1u);
            break;
        case 0x02425823u: // fatms inserted
            if (output != 0u && output_length >= 4u) memory.store32(output, 1u);
            break;
        // A pair of commands on the UMD device that ask the drive to read a
        // range of sectors ahead of the game reading them. What each one is
        // for was read out of the game's own code around the call, not
        // guessed: see the comment on kDiscReadAhead below.
        case kDiscReadAheadStart:
        case kDiscReadAheadWait: {
            if (split_path(device).device != Device::Disc) {
                kernel().finish(ctx, io_error::kDeviceNotFound);
                return;
            }
            const std::uint32_t input_length = arg(ctx, 3);
            if (command == kDiscReadAheadStart) {
                if (input == 0u || input_length < 16u || output == 0u || output_length < 4u) {
                    kernel().finish(ctx, io_error::kInvalidArgument);
                    return;
                }
                DiscReadAhead request;
                request.lba = memory.load32(input + 4u);
                request.sectors = memory.load32(input + 12u);
                const std::uint32_t id = io().next_read_ahead++;
                io().read_ahead.emplace(id, request);
                memory.store32(output, id);
                if (trace_io())
                    std::cerr << "[io] " << device << " read ahead " << request.sectors
                              << " sectors from " << request.lba << " -> id " << id << "\n";
                kernel().finish(ctx, 0u);
                return;
            }
            if (input == 0u || input_length < 4u) {
                kernel().finish(ctx, io_error::kInvalidArgument);
                return;
            }
            const std::uint32_t id = memory.load32(input);
            const auto found = io().read_ahead.find(id);
            if (found == io().read_ahead.end()) {
                std::cerr << "[io] " << device << " asked to wait for read ahead " << id
                          << ", which was never started\n";
                kernel().finish(ctx, io_error::kInvalidArgument);
                return;
            }
            // Nothing is outstanding: the image is a host file, so the range
            // was already there when it was asked for. Zero is what says so —
            // see the comment on kDiscReadAheadWait.
            if (trace_io())
                std::cerr << "[io] " << device << " read ahead " << id << " of " << found->second.sectors
                          << " sectors from " << found->second.lba << " has nothing outstanding\n";
            io().read_ahead.erase(found);
            kernel().finish(ctx, 0u);
            return;
        }
        case 0x02425818u: { // free space: input holds a pointer to the info block
            const std::uint32_t info = input != 0u ? memory.load32(input) : 0u;
            if (info != 0u) {
                constexpr std::uint32_t kSectorSize = 0x200u;
                constexpr std::uint32_t kSectorsPerCluster = 0x20u;
                constexpr std::uint32_t kFreeClusters = 0x100000u / 0x10u; // ~1 GiB free
                memory.store32(info, kFreeClusters * 2u);
                memory.store32(info + 4u, kFreeClusters);
                memory.store32(info + 8u, kFreeClusters);
                memory.store32(info + 12u, kSectorSize);
                memory.store32(info + 16u, kSectorsPerCluster);
            }
            break;
        }
        default: {
            // Report the buffers too: what a device is being asked for is
            // usually legible in what the game sends and how much room it
            // leaves for the answer.
            const std::uint32_t input_length = arg(ctx, 3);
            std::string description = "[io] devctl " + device + " cmd=" + psprecomp::hex32(command) +
                                      " in=" + std::to_string(input_length) + " out=" +
                                      std::to_string(output_length);
            if (input != 0u && input_length != 0u && input_length <= 64u) {
                description += " sent:";
                for (std::uint32_t i = 0; i < input_length; ++i)
                    description += " " + psprecomp::hex32(memory.load8(input + i)).substr(8u);
            }
            log_once("devctl:" + device + psprecomp::hex32(command), description + " (unhandled, returning 0)");
        }
            break;
        }
        kernel().finish(ctx, 0u);
    });

    hle.add("sceUmdUser", "sceUmdActivate", [](Runtime &, AllegrexContext &ctx) { kernel().finish(ctx, 0u); });
    hle.add("sceUmdUser", "sceUmdGetDriveStat",
            [](Runtime &, AllegrexContext &ctx) { kernel().finish(ctx, drive_status()); });
    hle.add("sceUmdUser", "sceUmdGetErrorStat", [](Runtime &, AllegrexContext &ctx) { kernel().finish(ctx, 0u); });

    // The disc never changes here: there is one image, it is present from the
    // start and it is never ejected. A game registers a callback to hear about
    // that, so it is notified once with the state it is already in, and then
    // never again.
    hle.add("sceUmdUser", "sceUmdRegisterUMDCallBack", [](Runtime &, AllegrexContext &ctx) {
        const auto callback = static_cast<SceUID>(arg(ctx, 0));
        if (kernel().callbacks.find(callback) == kernel().callbacks.end()) {
            kernel().finish(ctx, error::kUnknownCbid);
            return;
        }
        umd_callback() = callback;
        kernel().notify_callback(callback, drive_status());
        kernel().finish(ctx, 0u);
    });
    hle.add("sceUmdUser", "sceUmdUnRegisterUMDCallBack", [](Runtime &, AllegrexContext &ctx) {
        const auto callback = static_cast<SceUID>(arg(ctx, 0));
        if (umd_callback() != callback) {
            kernel().finish(ctx, error::kUnknownCbid);
            return;
        }
        umd_callback() = 0;
        kernel().finish(ctx, 0u);
    });
    // Waiting for the drive to reach a state it is already in returns at once,
    // which is every state this host has.
    // 1 when a disc is in the drive.
    hle.add("sceUmdUser", "sceUmdCheckMedium", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, io().disc ? 1u : 0u);
    });
    const auto wait_drive = [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, (drive_status() & arg(ctx, 0)) != 0u ? 0u : error::kWaitTimeout);
    };
    hle.add("sceUmdUser", "sceUmdWaitDriveStatCB", wait_drive);
    hle.add("sceUmdUser", "sceUmdWaitDriveStat", wait_drive);
    // (stat, timeout in microseconds): the same, the timeout never reached.
    hle.add("sceUmdUser", "sceUmdWaitDriveStatWithTimer", wait_drive);
}

} // namespace portablekit
