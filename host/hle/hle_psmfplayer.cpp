// scePsmfPlayer: the stock movie player library (libpsmfplayer.prx), which a
// game loads from its disc and hands a file name. The library reads the file
// itself, decodes it and gives the game finished pictures and PCM.
//
// Here the file is read through the same I/O as the game's own reads, split
// into access units by movie/psmf_demuxer, pictures are decoded with FFmpeg's
// H.264 decoder and written into the game's buffer in the pixel format it
// configured, and audio frames are decoded with FFmpeg's ATRAC3plus decoder.
// Pictures follow the audio clock once the game takes the audio, and the
// kernel's clock until then or when the movie has no sound.
//
// The structure layouts, status values and error codes are the library's
// documented interface; the pacing is this file's own. Without FFmpeg the
// calls stay logging stubs and a game goes on as if the movie had not played.
#include "../profile.hpp"
#include "hle_common.hpp"

#include "audio/atrac_decoder.hpp"
#include "movie/avc_decoder.hpp"
#include "movie/psmf_demuxer.hpp"

#include "psprecomp/common.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace portablekit {
namespace {

namespace player_error {
inline constexpr std::uint32_t kInvalidStatus = 0x80616001u;
inline constexpr std::uint32_t kInvalidStream = 0x80616003u;
inline constexpr std::uint32_t kBufferSize = 0x80616005u;
inline constexpr std::uint32_t kInvalidConfig = 0x80616006u;
inline constexpr std::uint32_t kInvalidParam = 0x80616008u;
inline constexpr std::uint32_t kNoMoreData = 0x8061600Cu;
inline constexpr std::uint32_t kIllegalArgument = 0x800200D2u;
} // namespace player_error

enum Status : std::uint32_t {
    kNone = 0x0u,
    kInit = 0x1u,
    kStandby = 0x2u,
    kPlaying = 0x4u,
    kFinished = 0x200u,
};

constexpr std::int32_t kModePause = 3;
constexpr std::int32_t kModeLast = 5;

// ConfigPlayer keys.
constexpr std::int32_t kConfigLoop = 0;       // 0 loops, 1 does not
constexpr std::int32_t kConfigPixelType = 1;  // a GE pixel format, -1 for the default

// GE pixel formats the picture can be written in.
constexpr std::uint32_t kPixel5650 = 0u;
constexpr std::uint32_t kPixel5551 = 1u;
constexpr std::uint32_t kPixel4444 = 2u;
constexpr std::uint32_t kPixel8888 = 3u;

constexpr std::uint32_t kPsmfMagic = 0x464D5350u;  // "PSMF"
constexpr std::uint32_t kAudioSamples = 2048u;
constexpr std::uint32_t kAudioOutBytes = kAudioSamples * 4u;
constexpr std::uint32_t kMinimumBuffer = 0x00285800u;
// The picture is written this wide when the game leaves the width 0.
constexpr std::uint32_t kDefaultFrameWidth = 512u;
constexpr std::int64_t kTicksPerSecond = 90000;
constexpr std::int64_t kAudioSampleRate = 44100;
// Packs read from the file at a time.
constexpr std::size_t kReadPacks = 32u;

bool trace_psmf() {
    static const bool enabled = portablekit::env("TRACE_MPEG") != nullptr;
    return enabled;
}

std::uint32_t be32(const std::uint8_t *bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 24u) | (static_cast<std::uint32_t>(bytes[1]) << 16u) |
           (static_cast<std::uint32_t>(bytes[2]) << 8u) | bytes[3];
}

struct Player {
    std::uint32_t status{kInit};
    std::uint32_t pixel_type{kPixel8888};
    bool loop{};
    std::int32_t play_mode{};
    std::int32_t play_speed{};

    // The movie, from its header.
    std::string path;
    std::uint64_t stream_offset{};  // the first pack, from the start of the file
    std::uint64_t stream_size{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t video_streams{};
    std::uint32_t audio_streams{};

    // Playing it.
    std::uint64_t read{};  // bytes of the stream demultiplexed
    std::unique_ptr<movie::PsmfDemuxer> demuxer;
    std::unique_ptr<movie::AvcDecoder> video;
    std::unique_ptr<audio::AtracDecoder> audio;
    std::optional<movie::Picture> picture;        // the picture on show
    std::optional<movie::AccessUnit> next_video;  // the next picture's unit, not due yet
    std::int64_t first_pts{-1};
    std::int64_t picture_pts{-1};  // relative to first_pts
    bool video_ended{};
    bool audio_ended{};
    std::uint64_t audio_samples{};  // handed to the game since the start
    std::uint64_t start_us{};       // kernel time the movie started
    bool audio_taken{};             // the game takes the audio, so it is the clock
};

std::map<std::uint32_t, std::unique_ptr<Player>> &players() {
    static std::map<std::uint32_t, std::unique_ptr<Player>> table;
    return table;
}

// The game passes a pointer to its handle; the handle written there is that
// same address.
Player *find_player(const psprecomp::GuestMemory &memory, std::uint32_t pointer) {
    if (!memory.contains(pointer, 4u)) return nullptr;
    const auto found = players().find(memory.load32(pointer));
    return found != players().end() ? found->second.get() : nullptr;
}

void finish_traced(AllegrexContext &ctx, const char *name, std::uint32_t result, const std::string &details = {},
                   unsigned argc = 1u) {
    if (trace_psmf()) {
        std::ostringstream line;
        line << "[psmfplayer] " << name << "(";
        for (unsigned i = 0; i < argc; ++i) line << (i != 0u ? ", " : "") << psprecomp::hex32(arg(ctx, i));
        line << ") -> " << psprecomp::hex32(result);
        if (!details.empty()) line << " " << details;
        std::cerr << line.str() << "\n";
    }
    kernel().finish(ctx, result);
}

// Starts reading the movie from its first pack.
void rewind(Player &player) {
    player.read = 0u;
    player.demuxer = std::make_unique<movie::PsmfDemuxer>();
    player.video = std::make_unique<movie::AvcDecoder>();
    (void)player.video->open();
    player.audio = std::make_unique<audio::AtracDecoder>();
    player.picture.reset();
    player.next_video.reset();
    player.first_pts = -1;
    player.picture_pts = -1;
    player.video_ended = false;
    player.audio_ended = false;
    player.audio_samples = 0u;
    player.audio_taken = false;
    player.start_us = kernel().now_us();
}

// Feeds the demultiplexer until `ready` holds or the stream is used up.
template <typename Ready> void feed(Player &player, Ready ready) {
    std::vector<std::uint8_t> packs;
    while (!ready() && player.read < player.stream_size) {
        const std::uint64_t left = player.stream_size - player.read;
        const auto size = static_cast<std::size_t>(std::min<std::uint64_t>(left, kReadPacks * movie::kPackSize));
        packs.resize(size);
        const std::int64_t count = read_guest_file(player.path, player.stream_offset + player.read, packs.data(), size);
        if (count <= 0) {
            player.read = player.stream_size;
            break;
        }
        for (std::size_t at = 0; at + movie::kPackSize <= static_cast<std::size_t>(count); at += movie::kPackSize)
            (void)player.demuxer->push_pack(std::span<const std::uint8_t>(packs.data() + at, movie::kPackSize));
        player.read += static_cast<std::uint64_t>(count);
        if (static_cast<std::size_t>(count) < size) player.read = player.stream_size;
    }
    if (player.read >= player.stream_size) player.demuxer->end_of_stream();
}

// Where the movie is now, 90 kHz from its start: the audio handed out once
// the game takes it, the time since the start until then.
std::int64_t clock_pts(const Player &player) {
    if (player.audio_taken)
        return static_cast<std::int64_t>(player.audio_samples) * kTicksPerSecond / kAudioSampleRate;
    return static_cast<std::int64_t>(kernel().now_us() - player.start_us) * kTicksPerSecond / 1'000'000;
}

// Decodes pictures until the one on show is the latest that is due. The first
// picture is shown as soon as there is one.
void advance_video(Player &player) {
    const std::int64_t now = clock_pts(player);
    for (;;) {
        if (!player.next_video) {
            feed(player, [&] { return player.demuxer->video_ready(); });
            player.next_video = player.demuxer->pop_video();
            if (!player.next_video) {
                movie::Picture last;
                if (player.video->drain(last)) player.picture = std::move(last);
                player.video_ended = true;
                return;
            }
        }
        std::int64_t pts = player.next_video->pts;
        if (pts < 0) pts = player.first_pts < 0 ? 0 : player.first_pts + player.picture_pts + movie::kVideoFrameTicks;
        if (player.first_pts < 0) player.first_pts = pts;
        const std::int64_t relative = pts - player.first_pts;
        if (player.picture && relative > now) return;
        movie::Picture decoded;
        if (player.video->decode(player.next_video->data, decoded)) player.picture = std::move(decoded);
        player.picture_pts = relative;
        player.next_video.reset();
    }
}

// Writes the picture on show into the game's buffer, `stride` pixels a row.
void write_picture(psprecomp::GuestMemory &memory, const Player &player, std::uint32_t buffer, std::uint32_t stride) {
    if (!player.picture) return;
    const movie::Picture &picture = *player.picture;
    const std::uint32_t rows = std::min(picture.height, player.height != 0u ? player.height : picture.height);
    const std::uint32_t columns = std::min({picture.width, stride, player.width != 0u ? player.width : picture.width});
    const std::uint32_t chroma_width = (picture.width + 1u) / 2u;
    const std::uint32_t bytes = player.pixel_type == kPixel8888 ? 4u : 2u;
    std::vector<std::uint8_t> line(static_cast<std::size_t>(columns) * bytes);
    for (std::uint32_t row = 0; row < rows; ++row) {
        for (std::uint32_t column = 0; column < columns; ++column) {
            // BT.601; limited-range luma is stretched to full range.
            int y = picture.y[static_cast<std::size_t>(row) * picture.width + column];
            const std::size_t chroma = static_cast<std::size_t>(row / 2u) * chroma_width + column / 2u;
            const int cb = picture.cb[chroma] - 128;
            const int cr = picture.cr[chroma] - 128;
            int scale = 1 << 16;
            if (!picture.full_range) {
                y -= 16;
                scale = 76309;  // 255/219
            }
            const int luma = y * scale;
            const auto red = static_cast<std::uint32_t>(std::clamp((luma + 91881 * cr) >> 16, 0, 255));
            const auto green = static_cast<std::uint32_t>(std::clamp((luma - 22554 * cb - 46802 * cr) >> 16, 0, 255));
            const auto blue = static_cast<std::uint32_t>(std::clamp((luma + 116130 * cb) >> 16, 0, 255));
            std::uint8_t *pixel = &line[static_cast<std::size_t>(column) * bytes];
            std::uint32_t packed = 0u;
            switch (player.pixel_type) {
            case kPixel5650: packed = (red >> 3u) | ((green >> 2u) << 5u) | ((blue >> 3u) << 11u); break;
            case kPixel5551: packed = (red >> 3u) | ((green >> 3u) << 5u) | ((blue >> 3u) << 10u) | 0x8000u; break;
            case kPixel4444: packed = (red >> 4u) | ((green >> 4u) << 4u) | ((blue >> 4u) << 8u) | 0xF000u; break;
            default:
                pixel[0] = static_cast<std::uint8_t>(red);
                pixel[1] = static_cast<std::uint8_t>(green);
                pixel[2] = static_cast<std::uint8_t>(blue);
                pixel[3] = 0xFFu;
                continue;
            }
            pixel[0] = static_cast<std::uint8_t>(packed);
            pixel[1] = static_cast<std::uint8_t>(packed >> 8u);
        }
        const std::uint32_t at = buffer + row * stride * bytes;
        if (!memory.contains(at, static_cast<std::uint32_t>(line.size()))) return;
        memory.copy_in(at, line);
    }
}

// The movie is over once both streams are, unless it loops.
void check_end(Player &player) {
    const bool audio_done = player.audio_streams == 0u || !player.audio_taken || player.audio_ended;
    if (player.status != kPlaying || !player.video_ended || !audio_done) return;
    if (player.loop) {
        rewind(player);
        return;
    }
    player.status = kFinished;
}

// Reads the PSMF header: where the stream is, how big, the streams in it and
// the picture size.
std::uint32_t set_psmf(Player &player, const std::string &path) {
    std::array<std::uint8_t, movie::kPackSize> header{};
    const std::int64_t count = read_guest_file(path, 0u, header.data(), header.size());
    if (count < 0x90) return player_error::kIllegalArgument;
    if ((static_cast<std::uint32_t>(header[0]) | (header[1] << 8u) | (header[2] << 16u) |
         (static_cast<std::uint32_t>(header[3]) << 24u)) != kPsmfMagic)
        return player_error::kIllegalArgument;
    player.path = path;
    player.stream_offset = be32(&header[0x08]);
    player.stream_size = be32(&header[0x0C]);
    const std::uint32_t streams = (static_cast<std::uint32_t>(header[0x80]) << 8u) | header[0x81];
    if (streams > 128u) return player_error::kIllegalArgument;
    player.video_streams = 0u;
    player.audio_streams = 0u;
    for (std::uint32_t i = 0; i < streams && 0x82u + i * 16u + 16u <= header.size(); ++i) {
        const std::uint8_t id = header[0x82u + i * 16u];
        if ((id & 0xE0u) == 0xE0u) ++player.video_streams;
        else if (id == 0xBDu) ++player.audio_streams;
    }
    player.width = header[0x8E] * 16u;
    player.height = header[0x8F] * 16u;
    return 0u;
}

void register_player(HleRegistrar &hle) {
    // scePsmfPlayerCreate(player, {buffer, bufferSize, threadPriority})
    hle.add("scePsmfPlayer", "scePsmfPlayerCreate", [](Runtime &rt, AllegrexContext &ctx) {
        auto &memory = rt.memory();
        const std::uint32_t handle = arg(ctx, 0);
        const std::uint32_t data = arg(ctx, 1);
        if (!memory.contains(handle, 4u) || !memory.contains(data, 12u)) {
            finish_traced(ctx, "scePsmfPlayerCreate", player_error::kIllegalArgument, {}, 2u);
            return;
        }
        if (memory.load32(data + 4u) < kMinimumBuffer) {
            memory.store32(handle, 0u);
            finish_traced(ctx, "scePsmfPlayerCreate", player_error::kBufferSize, {}, 2u);
            return;
        }
        auto player = std::make_unique<Player>();
        rewind(*player);
        players()[handle] = std::move(player);
        memory.store32(handle, handle);
        finish_traced(ctx, "scePsmfPlayerCreate", 0u, {}, 2u);
    });
    hle.add("scePsmfPlayer", "scePsmfPlayerDelete", [](Runtime &rt, AllegrexContext &ctx) {
        auto &memory = rt.memory();
        if (find_player(memory, arg(ctx, 0)) == nullptr) {
            finish_traced(ctx, "scePsmfPlayerDelete", player_error::kInvalidStatus);
            return;
        }
        players().erase(memory.load32(arg(ctx, 0)));
        memory.store32(arg(ctx, 0), 0u);
        finish_traced(ctx, "scePsmfPlayerDelete", 0u);
    });
    // scePsmfPlayerSetPsmf(player, path)
    const auto set = [](Runtime &rt, AllegrexContext &ctx) {
        auto &memory = rt.memory();
        Player *player = find_player(memory, arg(ctx, 0));
        if (player == nullptr || player->status != kInit) {
            finish_traced(ctx, "scePsmfPlayerSetPsmf", player_error::kInvalidStatus, {}, 2u);
            return;
        }
        const std::string path = read_cstring(memory, arg(ctx, 1), 256u);
        const std::uint32_t result = set_psmf(*player, path);
        if (result == 0u) player->status = kStandby;
        std::ostringstream details;
        details << path << " " << player->width << "x" << player->height << " video=" << player->video_streams
                << " audio=" << player->audio_streams << " size=" << player->stream_size;
        finish_traced(ctx, "scePsmfPlayerSetPsmf", result, details.str(), 2u);
    };
    hle.add("scePsmfPlayer", "scePsmfPlayerSetPsmf", set);
    (void)hle.try_add("scePsmfPlayer", "scePsmfPlayerSetPsmfCB", set);
    hle.add("scePsmfPlayer", "scePsmfPlayerReleasePsmf", [](Runtime &rt, AllegrexContext &ctx) {
        Player *player = find_player(rt.memory(), arg(ctx, 0));
        if (player == nullptr || player->status < kStandby) {
            finish_traced(ctx, "scePsmfPlayerReleasePsmf", player_error::kInvalidStatus);
            return;
        }
        rewind(*player);
        player->status = kInit;
        finish_traced(ctx, "scePsmfPlayerReleasePsmf", 0u);
    });
    // scePsmfPlayerStart(player, {videoCodec, videoStream, audioCodec,
    // audioStream, playMode, playSpeed}, initPts)
    hle.add("scePsmfPlayer", "scePsmfPlayerStart", [](Runtime &rt, AllegrexContext &ctx) {
        auto &memory = rt.memory();
        Player *player = find_player(memory, arg(ctx, 0));
        const std::uint32_t data = arg(ctx, 1);
        if (player == nullptr || player->status == kInit) {
            finish_traced(ctx, "scePsmfPlayerStart", player_error::kInvalidStatus, {}, 3u);
            return;
        }
        if (!memory.contains(data, 24u)) {
            finish_traced(ctx, "scePsmfPlayerStart", player_error::kIllegalArgument, {}, 3u);
            return;
        }
        const auto mode = static_cast<std::int32_t>(memory.load32(data + 16u));
        if (mode < 0 || mode > kModeLast) {
            finish_traced(ctx, "scePsmfPlayerStart", player_error::kInvalidParam, {}, 3u);
            return;
        }
        if (static_cast<std::int32_t>(memory.load32(data + 4u)) >= static_cast<std::int32_t>(player->video_streams)) {
            finish_traced(ctx, "scePsmfPlayerStart", player_error::kInvalidConfig, {}, 3u);
            return;
        }
        if (arg(ctx, 2) != 0u)
            log_once("psmfplayer-seek", "[psmfplayer] starting a movie part-way through is not implemented; it "
                                        "starts from the beginning");
        player->play_mode = mode;
        player->play_speed = static_cast<std::int32_t>(memory.load32(data + 20u));
        rewind(*player);
        player->status = kPlaying;
        finish_traced(ctx, "scePsmfPlayerStart", 0u, "mode=" + std::to_string(mode), 3u);
    });
    hle.add("scePsmfPlayer", "scePsmfPlayerStop", [](Runtime &rt, AllegrexContext &ctx) {
        Player *player = find_player(rt.memory(), arg(ctx, 0));
        if (player == nullptr || player->status < kPlaying) {
            finish_traced(ctx, "scePsmfPlayerStop", player_error::kInvalidStatus);
            return;
        }
        player->status = kStandby;
        finish_traced(ctx, "scePsmfPlayerStop", 0u);
    });
    // Break stops a movie's own finishing; nothing here runs on its own.
    hle.add("scePsmfPlayer", "scePsmfPlayerBreak", [](Runtime &rt, AllegrexContext &ctx) {
        finish_traced(ctx, "scePsmfPlayerBreak",
                      find_player(rt.memory(), arg(ctx, 0)) != nullptr ? 0u : player_error::kInvalidStatus);
    });
    hle.add("scePsmfPlayer", "scePsmfPlayerUpdate", [](Runtime &rt, AllegrexContext &ctx) {
        Player *player = find_player(rt.memory(), arg(ctx, 0));
        if (player == nullptr || player->status < kPlaying) {
            finish_traced(ctx, "scePsmfPlayerUpdate", player_error::kInvalidStatus);
            return;
        }
        check_end(*player);
        finish_traced(ctx, "scePsmfPlayerUpdate", 0u);
    });
    // scePsmfPlayerGetVideoData(player, {frameWidth, displayBuffer, displayPts})
    hle.add("scePsmfPlayer", "scePsmfPlayerGetVideoData", [](Runtime &rt, AllegrexContext &ctx) {
        auto &memory = rt.memory();
        Player *player = find_player(memory, arg(ctx, 0));
        const std::uint32_t data = arg(ctx, 1);
        if (player == nullptr || player->status < kPlaying) {
            finish_traced(ctx, "scePsmfPlayerGetVideoData", player_error::kInvalidStatus, {}, 2u);
            return;
        }
        if (!memory.contains(data, 12u)) {
            finish_traced(ctx, "scePsmfPlayerGetVideoData", player_error::kIllegalArgument, {}, 2u);
            return;
        }
        if (player->play_mode != kModePause) advance_video(*player);
        if (!player->picture) {
            finish_traced(ctx, "scePsmfPlayerGetVideoData", player_error::kNoMoreData, "no picture yet", 2u);
            return;
        }
        std::uint32_t stride = memory.load32(data);
        stride = stride == 0u ? kDefaultFrameWidth : stride & ~1u;
        write_picture(memory, *player, memory.load32(data + 4u), stride);
        memory.store32(data + 8u, static_cast<std::uint32_t>(std::max<std::int64_t>(player->picture_pts, 0)));
        finish_traced(ctx, "scePsmfPlayerGetVideoData", 0u, "pts=" + std::to_string(player->picture_pts), 2u);
    });
    // scePsmfPlayerGetAudioData(player, buffer): 2048 stereo samples.
    hle.add("scePsmfPlayer", "scePsmfPlayerGetAudioData", [](Runtime &rt, AllegrexContext &ctx) {
        auto &memory = rt.memory();
        Player *player = find_player(memory, arg(ctx, 0));
        const std::uint32_t buffer = arg(ctx, 1);
        if (player == nullptr || player->status < kPlaying) {
            finish_traced(ctx, "scePsmfPlayerGetAudioData", player_error::kInvalidStatus, {}, 2u);
            return;
        }
        if (!memory.contains(buffer, kAudioOutBytes)) {
            finish_traced(ctx, "scePsmfPlayerGetAudioData", player_error::kIllegalArgument, {}, 2u);
            return;
        }
        if (player->play_mode == kModePause || player->audio_streams == 0u) {
            finish_traced(ctx, "scePsmfPlayerGetAudioData", player_error::kNoMoreData, {}, 2u);
            return;
        }
        feed(*player, [&] { return player->demuxer->audio_ready(); });
        const auto unit = player->demuxer->pop_audio();
        std::vector<std::int16_t> samples(static_cast<std::size_t>(kAudioSamples) * 2u, 0);
        if (!unit) {
            player->audio_ended = true;
            finish_traced(ctx, "scePsmfPlayerGetAudioData", player_error::kNoMoreData, "end", 2u);
            return;
        }
        if (!player->audio->is_open() && player->demuxer->audio_frame_size() > movie::kAtracFrameHeader)
            (void)player->audio->open(audio::AtracCodec::Atrac3Plus, std::max(player->demuxer->audio_channels(), 1u),
                                      static_cast<unsigned>(player->demuxer->audio_frame_size() - movie::kAtracFrameHeader),
                                      {});
        const bool decoded = player->audio->is_open() && player->audio->decode(unit->data, samples.data()) != 0u;
        std::vector<std::uint8_t> bytes(samples.size() * 2u);
        for (std::size_t i = 0; i < samples.size(); ++i) {
            const auto value = static_cast<std::uint16_t>(samples[i]);
            bytes[i * 2u] = static_cast<std::uint8_t>(value);
            bytes[i * 2u + 1u] = static_cast<std::uint8_t>(value >> 8u);
        }
        memory.copy_in(buffer, bytes);
        if (!player->audio_taken) {
            // From now on the audio is the clock: carry on from where the
            // kernel clock had got to.
            player->audio_samples = static_cast<std::uint64_t>(clock_pts(*player)) * kAudioSampleRate / kTicksPerSecond;
            player->audio_taken = true;
        }
        player->audio_samples += kAudioSamples;
        finish_traced(ctx, "scePsmfPlayerGetAudioData", 0u, decoded ? "" : "silent", 2u);
    });
    hle.add("scePsmfPlayer", "scePsmfPlayerGetAudioOutSize", [](Runtime &rt, AllegrexContext &ctx) {
        finish_traced(ctx, "scePsmfPlayerGetAudioOutSize",
                      find_player(rt.memory(), arg(ctx, 0)) != nullptr ? kAudioOutBytes : player_error::kInvalidStatus);
    });
    hle.add("scePsmfPlayer", "scePsmfPlayerGetCurrentStatus", [](Runtime &rt, AllegrexContext &ctx) {
        const Player *player = find_player(rt.memory(), arg(ctx, 0));
        const std::uint32_t status = player == nullptr || player->status == kNone ? player_error::kInvalidStatus : player->status;
        finish_traced(ctx, "scePsmfPlayerGetCurrentStatus", status);
    });
    // scePsmfPlayerGetPsmfInfo(player, {lastFrameTs, videoStreams, audioStreams,
    // pcmStreams, playerVersion}, outWidth, outHeight). The width and height
    // are written by the library version some games ship and not by others;
    // writing them when the game passes somewhere to write is harmless.
    hle.add("scePsmfPlayer", "scePsmfPlayerGetPsmfInfo", [](Runtime &rt, AllegrexContext &ctx) {
        auto &memory = rt.memory();
        const Player *player = find_player(memory, arg(ctx, 0));
        const std::uint32_t info = arg(ctx, 1);
        if (player == nullptr || player->status < kStandby) {
            finish_traced(ctx, "scePsmfPlayerGetPsmfInfo", player_error::kInvalidStatus, {}, 4u);
            return;
        }
        if (!memory.contains(info, 20u)) {
            finish_traced(ctx, "scePsmfPlayerGetPsmfInfo", player_error::kIllegalArgument, {}, 4u);
            return;
        }
        memory.store32(info, 0u);  // the last picture's time is not known without reading the file through
        memory.store32(info + 4u, player->video_streams);
        memory.store32(info + 8u, player->audio_streams);
        memory.store32(info + 12u, 0u);
        memory.store32(info + 16u, 0u);
        if (arg(ctx, 2) != 0u && memory.contains(arg(ctx, 2), 4u)) memory.store32(arg(ctx, 2), player->width);
        if (arg(ctx, 3) != 0u && memory.contains(arg(ctx, 3), 4u)) memory.store32(arg(ctx, 3), player->height);
        finish_traced(ctx, "scePsmfPlayerGetPsmfInfo", 0u,
                      std::to_string(player->width) + "x" + std::to_string(player->height), 4u);
    });
    // scePsmfPlayerConfigPlayer(player, key, value)
    hle.add("scePsmfPlayer", "scePsmfPlayerConfigPlayer", [](Runtime &rt, AllegrexContext &ctx) {
        Player *player = find_player(rt.memory(), arg(ctx, 0));
        const auto key = static_cast<std::int32_t>(arg(ctx, 1));
        const auto value = static_cast<std::int32_t>(arg(ctx, 2));
        std::uint32_t result = 0u;
        if (player == nullptr) result = player_error::kInvalidStatus;
        else if (key == kConfigLoop && (value == 0 || value == 1)) player->loop = value == 0;
        else if (key == kConfigPixelType && value >= -1 && value <= 3) player->pixel_type = value == -1 ? kPixel8888 : static_cast<std::uint32_t>(value);
        else result = key == kConfigLoop || key == kConfigPixelType ? player_error::kInvalidParam : player_error::kInvalidConfig;
        finish_traced(ctx, "scePsmfPlayerConfigPlayer", result, {}, 3u);
    });
    // scePsmfPlayerChangePlayMode(player, mode, speed): play and pause.
    hle.add("scePsmfPlayer", "scePsmfPlayerChangePlayMode", [](Runtime &rt, AllegrexContext &ctx) {
        Player *player = find_player(rt.memory(), arg(ctx, 0));
        const auto mode = static_cast<std::int32_t>(arg(ctx, 1));
        if (player == nullptr || player->status < kPlaying) {
            finish_traced(ctx, "scePsmfPlayerChangePlayMode", player_error::kInvalidStatus, {}, 3u);
            return;
        }
        if (mode < 0 || mode > kModeLast) {
            finish_traced(ctx, "scePsmfPlayerChangePlayMode", player_error::kInvalidConfig, {}, 3u);
            return;
        }
        player->play_mode = mode;
        finish_traced(ctx, "scePsmfPlayerChangePlayMode", 0u, {}, 3u);
    });
}

} // namespace

void register_psmfplayer(HleRegistrar &hle) {
    if (!movie::AvcDecoder::available()) return;
    register_player(hle);
}

} // namespace portablekit
