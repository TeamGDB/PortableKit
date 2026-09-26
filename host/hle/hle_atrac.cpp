// sceAtrac3plus: a game's music. A track is an ATRAC3 or ATRAC3plus WAVE file,
// either read whole into guest memory (sceAtracSetDataAndGetID) or streamed:
// the start of the file in a buffer, and the rest added by the game as it
// plays (sceAtracSetHalfwayBufferAndGetID, sceAtracAddStreamData, or a
// SetData whose buffer is smaller than the file). The library
// decodes it one frame per call into 16-bit stereo PCM, which the game's own
// decode threads then hand to sceAudio. Frames are decoded with FFmpeg (audio/atrac_decoder),
// so without it nothing here is bound and the imports stay logging stubs.
//
// Sample positions follow the library's convention: position 0 is the first
// sample the encoder was given. The decoded stream starts earlier, by the
// encoder delay recorded in the `fact` chunk plus the decoder's own delay, and
// loop points in the `smpl` chunk are counted including the encoder delay.
#include "../profile.hpp"
#include "hle_common.hpp"

#include "audio/atrac_decoder.hpp"

#include "psprecomp/common.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <vector>

namespace portablekit {
namespace {

namespace atrac_error {
inline constexpr std::uint32_t kParamFail = 0x80630001u;
inline constexpr std::uint32_t kNoAtracId = 0x80630003u;
inline constexpr std::uint32_t kBadCodecType = 0x80630004u;
inline constexpr std::uint32_t kBadAtracId = 0x80630005u;
inline constexpr std::uint32_t kUnknownFormat = 0x80630006u;
inline constexpr std::uint32_t kBadCodecParam = 0x80630008u;
inline constexpr std::uint32_t kNoData = 0x80630010u;
inline constexpr std::uint32_t kSizeTooSmall = 0x80630011u;
inline constexpr std::uint32_t kBadSample = 0x80630015u;
inline constexpr std::uint32_t kNoLoopInformation = 0x80630021u;
inline constexpr std::uint32_t kSecondBufferNotNeeded = 0x80630022u;
inline constexpr std::uint32_t kAllDataDecoded = 0x80630024u;
} // namespace atrac_error

constexpr std::uint16_t kFormatAtrac3 = 0x0270u;
constexpr std::uint16_t kFormatExtensible = 0xFFFEu;
// Samples the decoder itself delays its output by, per codec.
constexpr std::uint32_t kAtrac3DecoderDelay = 69u;
constexpr std::uint32_t kAtrac3PlusDecoderDelay = 368u;
// "Every byte of the file is in the buffer", reported instead of a frame count.
constexpr std::int32_t kRemainAllDataOnMemory = -1;
constexpr std::size_t kMaxAtracIds = 6u;

bool trace_atrac() {
    static const bool enabled = portablekit::env("TRACE_ATRAC") != nullptr;
    return enabled;
}

void trace(const std::string &line) {
    if (trace_atrac()) std::cerr << "[atrac] " << line << "\n";
}

// What SetData learns from the WAVE header.
struct TrackInfo {
    audio::AtracCodec codec{audio::AtracCodec::Atrac3};
    std::uint32_t channels{};
    std::uint32_t block_align{};
    std::vector<std::uint8_t> extradata;
    std::uint32_t file_size{};
    std::uint32_t data_offset{};
    std::uint32_t data_size{};
    std::int32_t end_sample{};    // last playable position
    std::int32_t loop_start{-1};  // positions, -1 without a loop
    std::int32_t loop_end{-1};
    std::uint32_t skip{};         // decoded samples before position 0
};

struct AtracContext {
    // False for an id sceAtracGetAtracID has handed out with no data yet.
    bool loaded{};
    TrackInfo track;
    std::uint32_t buffer{};
    std::uint32_t buffer_size{};
    std::int32_t loop_num{};
    std::int32_t position{};  // next position DecodeData returns
    audio::AtracDecoder decoder;
    // The frame the decoder produces next if fed sequentially, and the last
    // frame it produced, kept because a call rarely consumes a whole frame.
    std::int64_t next_frame{};
    std::int64_t cached_frame{-1};
    std::vector<std::int16_t> cached;

    // Streaming: the buffer holds only part of the file, and the game adds
    // the rest as it plays. Every byte it adds is copied here when it is
    // added, so frames decode from this copy and what the game does to its
    // buffer afterwards cannot change them. The buffer is used as a ring only
    // to tell the game where to write next.
    bool streaming{};
    std::uint32_t written{};              // bytes of the file delivered, from its start
    // Where the game adds data next. It follows `written` until the whole
    // file has been added; a track that loops then asks again from the frame
    // the loop starts in, as many times as it loops (`wraps`), while the
    // decoder has gone round the loop `loops_done` times.
    std::uint32_t cursor{};
    std::uint32_t wraps{};
    std::uint32_t loops_done{};
    std::uint32_t stored_from{};          // file offset of stored[0]
    std::vector<std::uint8_t> stored;

    // The codec sceAtracGetAtracID was asked for (0x1000 ATRAC3plus, 0x1001
    // ATRAC3), and, once sceAtracLowLevelInitDecoder has run, the frames the
    // game feeds the decoder itself: their size and the channels it wants.
    std::uint32_t codec_type{0x1000u};
    bool low_level{};
    std::uint32_t low_level_frame_bytes{};
    std::uint32_t low_level_out_channels{2u};
};

std::array<std::unique_ptr<AtracContext>, kMaxAtracIds> &contexts() {
    static std::array<std::unique_ptr<AtracContext>, kMaxAtracIds> table;
    return table;
}

// An id with data set; an id with none behaves as unknown data.
AtracContext *find_context(std::uint32_t id) {
    AtracContext *context = id < kMaxAtracIds ? contexts()[id].get() : nullptr;
    return context != nullptr && context->loaded ? context : nullptr;
}

// Any id handed out, with data or not.
bool id_in_use(std::uint32_t id) { return id < kMaxAtracIds && contexts()[id] != nullptr; }

// Unknown ids and released ids fail differently on hardware.
std::uint32_t context_error(std::uint32_t id) {
    return id < kMaxAtracIds ? atrac_error::kNoData : atrac_error::kBadAtracId;
}

std::uint32_t read_le32(const std::vector<std::uint8_t> &bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1u]) << 8u) |
           (static_cast<std::uint32_t>(bytes[offset + 2u]) << 16u) |
           (static_cast<std::uint32_t>(bytes[offset + 3u]) << 24u);
}

std::uint16_t read_le16(const std::vector<std::uint8_t> &bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset] | (bytes[offset + 1u] << 8u));
}

// Parses the RIFF WAVE header at the start of the guest buffer. Returns an
// error code on failure.
std::optional<std::uint32_t> parse_header(const psprecomp::GuestMemory &memory, std::uint32_t buffer,
                                          std::uint32_t buffer_size, TrackInfo &info) {
    if (buffer_size < 12u) return atrac_error::kSizeTooSmall;
    // The header is small; everything up to the data chunk fits in far less.
    const std::uint32_t header_bytes = std::min<std::uint32_t>(buffer_size, 0x1000u);
    std::vector<std::uint8_t> bytes(header_bytes);
    memory.copy_out(buffer, bytes);
    if (read_le32(bytes, 0u) != 0x46464952u || read_le32(bytes, 8u) != 0x45564157u)  // "RIFF", "WAVE"
        return atrac_error::kUnknownFormat;
    info.file_size = read_le32(bytes, 4u) + 8u;

    bool have_format = false;
    std::optional<std::uint32_t> fact_samples;
    std::uint32_t fact_offset = 0u;
    std::optional<std::pair<std::uint32_t, std::uint32_t>> loop;
    std::size_t offset = 12u;
    while (offset + 8u <= bytes.size()) {
        const std::uint32_t id = read_le32(bytes, offset);
        const std::uint32_t size = read_le32(bytes, offset + 4u);
        const std::size_t body = offset + 8u;
        if (id == 0x61746164u) {  // "data"
            info.data_offset = static_cast<std::uint32_t>(body);
            info.data_size = size;
            break;
        }
        if (body + size > bytes.size()) return atrac_error::kSizeTooSmall;
        if (id == 0x20746D66u && size >= 16u) {  // "fmt "
            const std::uint16_t tag = read_le16(bytes, body);
            info.channels = read_le16(bytes, body + 2u);
            info.block_align = read_le16(bytes, body + 12u);
            if (tag == kFormatAtrac3) {
                info.codec = audio::AtracCodec::Atrac3;
                if (size >= 18u + 14u) info.extradata.assign(bytes.begin() + static_cast<std::ptrdiff_t>(body + 18u),
                                                              bytes.begin() + static_cast<std::ptrdiff_t>(body + 32u));
            } else if (tag == kFormatExtensible) {
                info.codec = audio::AtracCodec::Atrac3Plus;
            } else {
                return atrac_error::kBadCodecType;
            }
            have_format = true;
        } else if (id == 0x74636166u && size >= 4u) {  // "fact"
            fact_samples = read_le32(bytes, body);
            if (size >= 8u) fact_offset = read_le32(bytes, body + 4u);
        } else if (id == 0x6C706D73u && size >= 36u) {  // "smpl"
            const std::uint32_t loops = read_le32(bytes, body + 28u);
            if (loops > 0u && size >= 36u + 24u) loop = {read_le32(bytes, body + 36u + 8u), read_le32(bytes, body + 36u + 12u)};
        }
        offset = body + size + (size & 1u);
    }
    if (!have_format || info.data_offset == 0u) return atrac_error::kUnknownFormat;
    if (info.channels == 0u || info.channels > 2u || info.block_align == 0u) return atrac_error::kBadCodecParam;

    const std::uint32_t frame_samples = static_cast<std::uint32_t>(audio::atrac_frame_samples(info.codec));
    info.skip = fact_offset + (info.codec == audio::AtracCodec::Atrac3 ? kAtrac3DecoderDelay : kAtrac3PlusDecoderDelay);
    const std::uint64_t decoded = static_cast<std::uint64_t>(info.data_size / info.block_align) * frame_samples;
    const std::uint64_t playable = decoded > info.skip ? decoded - info.skip : 0u;
    const std::uint64_t samples = fact_samples ? std::min<std::uint64_t>(*fact_samples, playable) : playable;
    if (samples == 0u) return atrac_error::kUnknownFormat;
    info.end_sample = static_cast<std::int32_t>(samples - 1u);
    if (loop && loop->first >= fact_offset && loop->second >= loop->first) {
        info.loop_start = static_cast<std::int32_t>(loop->first - fact_offset);
        info.loop_end = std::min(static_cast<std::int32_t>(loop->second - fact_offset), info.end_sample);
    }
    return std::nullopt;
}

bool looping(const AtracContext &context) {
    return context.track.loop_start >= 0 && context.loop_num != 0;
}

// The file offset of the frame the loop starts in.
std::uint32_t loop_offset(const AtracContext &context) {
    const TrackInfo &track = context.track;
    const auto frame_samples = static_cast<std::int64_t>(audio::atrac_frame_samples(track.codec));
    const std::int64_t frame = (static_cast<std::int64_t>(std::max(track.loop_start, 0)) + track.skip) / frame_samples;
    return track.data_offset + static_cast<std::uint32_t>(frame) * track.block_align;
}

// A file offset as a position in the order the track plays: each pass round
// the loop adds the loop's length.
std::uint64_t play_order(const AtracContext &context, std::uint32_t offset, std::uint32_t passes) {
    const std::uint32_t loop_bytes = context.track.file_size - std::min(loop_offset(context), context.track.file_size);
    return offset + static_cast<std::uint64_t>(passes) * loop_bytes;
}

// Decodes stream frame `frame` into the cache, reseeding the decoder when the
// request is not the frame that follows the last one it decoded.
bool load_frame(const psprecomp::GuestMemory &memory, AtracContext &context, std::int64_t frame) {
    if (frame == context.cached_frame) return true;
    const TrackInfo &track = context.track;
    const std::size_t frame_samples = audio::atrac_frame_samples(track.codec);
    context.cached.assign(frame_samples * 2u, 0);
    std::vector<std::uint8_t> bytes(track.block_align);
    const auto decode = [&](std::int64_t index) {
        const std::uint64_t offset = track.data_offset + static_cast<std::uint64_t>(index) * track.block_align;
        if (context.streaming) {
            if (offset < context.stored_from ||
                offset + track.block_align > context.stored_from + context.stored.size())
                return false;
            const auto start = context.stored.begin() + static_cast<std::ptrdiff_t>(offset - context.stored_from);
            std::copy(start, start + track.block_align, bytes.begin());
            return context.decoder.decode(bytes, context.cached.data()) != 0u;
        }
        if (offset + track.block_align > std::min(context.buffer_size, track.data_offset + track.data_size)) return false;
        memory.copy_out(context.buffer + static_cast<std::uint32_t>(offset), bytes);
        return context.decoder.decode(bytes, context.cached.data()) != 0u;
    };
    if (frame != context.next_frame) {
        // A frame's output depends on the ones before it: after a seek, two
        // frames of warm-up make the decoder's state, and so its output,
        // identical to decoding straight through.
        context.decoder.reset();
        for (std::int64_t warm_up = std::max<std::int64_t>(frame - 2, 0); warm_up < frame; ++warm_up)
            (void)decode(warm_up);
    }
    const bool decoded = decode(frame);
    if (!decoded) std::fill(context.cached.begin(), context.cached.end(), std::int16_t{0});
    context.next_frame = frame + 1;
    context.cached_frame = frame;
    return decoded;
}

void release_context(std::uint32_t id) {
    if (id < kMaxAtracIds) contexts()[id].reset();
}

// The file offset of the frame the next DecodeData starts in.
std::uint32_t current_frame_offset(const AtracContext &context) {
    const TrackInfo &track = context.track;
    const auto frame_samples = static_cast<std::int64_t>(audio::atrac_frame_samples(track.codec));
    const std::int64_t frame = (static_cast<std::int64_t>(context.position) + track.skip) / frame_samples;
    return track.data_offset + static_cast<std::uint32_t>(frame) * track.block_align;
}

// Drops stored bytes the decoder can no longer need: everything before the
// current frame, less the two frames of warm-up a seek decodes first.
void trim_stored(AtracContext &context) {
    const std::uint32_t keep_from_wanted = current_frame_offset(context);
    const std::uint32_t warm_up = 2u * context.track.block_align;
    std::uint32_t keep_from = keep_from_wanted > warm_up ? keep_from_wanted - warm_up : 0u;
    // A track that loops decodes its loop again from these bytes: the game
    // is asked for them again too, but what it adds then is not needed.
    if (context.track.loop_start >= 0) {
        const std::uint32_t loop_from = loop_offset(context);
        keep_from = std::min(keep_from, loop_from > warm_up ? loop_from - warm_up : 0u);
    }
    if (keep_from <= context.stored_from) return;
    const std::uint32_t drop = std::min<std::uint32_t>(keep_from - context.stored_from,
                                                       static_cast<std::uint32_t>(context.stored.size()));
    context.stored.erase(context.stored.begin(), context.stored.begin() + drop);
    context.stored_from += drop;
}

// Copies `size` bytes the game has just written at `address` as the file's
// bytes from `written` on.
void store_added(const psprecomp::GuestMemory &memory, AtracContext &context, std::uint32_t address,
                 std::uint32_t size) {
    if (context.cursor >= context.written) {
        if (context.stored.empty()) context.stored_from = context.written;
        const std::size_t at = context.stored.size();
        context.stored.resize(at + size);
        memory.copy_out(address, std::span(context.stored).subspan(at, size));
        context.written += size;
    }
    context.cursor += size;
    if (context.cursor >= context.track.file_size && looping(context)) {
        context.cursor = loop_offset(context);
        ++context.wraps;
    }
}

// Frames in the buffer the game has not decoded yet, or "all of it" once the
// whole file has been delivered.
std::int32_t remain_frames(const AtracContext &context) {
    if (!context.streaming || context.cursor >= context.track.file_size) return kRemainAllDataOnMemory;
    const std::uint64_t delivered = play_order(context, context.cursor, context.wraps);
    const std::uint64_t from = play_order(context, current_frame_offset(context), context.loops_done);
    if (delivered <= from) return 0;
    return static_cast<std::int32_t>((delivered - from) / context.track.block_align);
}

// Where the game writes next, how much fits without overwriting a byte not
// yet decoded or running past the ring's end, and the file offset to read it
// from.
//
// The ring is whole frames: games write a frame at a time wherever they are
// told, so there must always be room for one. Frames start where the data
// starts in the buffer, after the header the first fill put there, and file
// frame f lives in slot f mod the number of frames that fit. A buffer that
// holds the whole file is then simply the file. Where a PSP puts its slots
// has not been established; the game only writes where it is told, and
// frames decode from the copy made when they are added, so it does not
// change what is heard.
struct StreamWrite {
    std::uint32_t address{};
    std::uint32_t writable{};
    std::uint32_t file_offset{};
};

std::uint32_t ring_frames(const AtracContext &context) {
    const std::uint32_t data_in_buffer = std::min(context.track.data_offset, context.buffer_size);
    return (context.buffer_size - data_in_buffer) / context.track.block_align;
}

// The address file offset `offset` (in the data) is written to.
std::uint32_t ring_address(const AtracContext &context, std::uint32_t offset) {
    const TrackInfo &track = context.track;
    const std::uint32_t frames = std::max(ring_frames(context), 1u);
    const std::uint32_t into = offset - std::min(offset, track.data_offset);
    return context.buffer + track.data_offset + (into / track.block_align % frames) * track.block_align +
           into % track.block_align;
}

StreamWrite stream_write(const AtracContext &context) {
    const TrackInfo &track = context.track;
    const std::uint32_t frames = ring_frames(context);
    if (frames == 0u) return {context.buffer, 0u, context.cursor};
    const std::uint32_t ring = frames * track.block_align;
    const std::uint32_t written = std::max(context.cursor, track.data_offset);
    const std::uint64_t delivered = play_order(context, written, context.wraps);
    const std::uint64_t playing = play_order(context, current_frame_offset(context), context.loops_done);
    const std::uint64_t pending = delivered - std::min(delivered, playing);
    std::uint32_t writable = pending < ring ? ring - static_cast<std::uint32_t>(pending) : 0u;
    // Contiguous: up to the end of the last slot.
    const std::uint32_t into_ring = (written - track.data_offset) % ring;
    writable = std::min(writable, ring - into_ring);
    writable = std::min(writable, track.file_size - std::min(written, track.file_size));
    return {ring_address(context, written), writable, written};
}

// The file offset of the frame sample `sample` is decoded from.
std::uint32_t reset_offset(const AtracContext &context, std::int32_t sample) {
    const TrackInfo &track = context.track;
    const auto frame_samples = static_cast<std::int64_t>(audio::atrac_frame_samples(track.codec));
    const std::int64_t frame = (static_cast<std::int64_t>(sample) + track.skip) / frame_samples;
    return track.data_offset + static_cast<std::uint32_t>(frame) * track.block_align;
}

void write_s32(psprecomp::GuestMemory &memory, std::uint32_t address, std::int32_t value) {
    if (address != 0u) memory.store32(address, static_cast<std::uint32_t>(value));
}

void finish_traced(AllegrexContext &ctx, const char *name, std::uint32_t result, const std::string &details = {}) {
    if (trace_atrac()) {
        std::ostringstream line;
        line << name << "(" << psprecomp::hex32(arg(ctx, 0)) << ", " << psprecomp::hex32(arg(ctx, 1)) << ", "
             << psprecomp::hex32(arg(ctx, 2)) << ", " << psprecomp::hex32(arg(ctx, 3)) << ") -> "
             << psprecomp::hex32(result);
        if (!details.empty()) line << " " << details;
        trace(line.str());
    }
    kernel().finish(ctx, result);
}

struct LoadResult {
    std::uint32_t result{};  // the id, or an error
    std::string details;
};

// Reads the header, opens the decoder and puts the track in slot `id`, or in
// the first free one. `streaming`: only `read_size` bytes of the file are in
// the buffer yet; otherwise all of it that ever will be.
LoadResult load_track(const psprecomp::GuestMemory &memory, std::optional<std::uint32_t> id, std::uint32_t buffer,
                      std::uint32_t read_size, std::uint32_t buffer_size, bool streaming) {
    auto context = std::make_unique<AtracContext>();
    if (read_size > buffer_size) return {atrac_error::kSizeTooSmall, "read size past the buffer"};
    if (auto failed = parse_header(memory, buffer, read_size, context->track)) return {*failed, "bad header"};
    const TrackInfo &track = context->track;
    // A whole-file call (SetData, SetDataAndGetID) given a buffer smaller
    // than the file streams too: the buffer holds its first bufferSize bytes
    // and the game refills it through GetStreamDataInfo/AddStreamData.
    if (!streaming && buffer_size < track.file_size) streaming = true;
    if (!context->decoder.open(track.codec, track.channels, track.block_align, track.extradata))
        return {atrac_error::kBadCodecParam, "decoder refused the stream"};
    auto &table = contexts();
    std::uint32_t slot = 0u;
    if (id) {
        slot = *id;
    } else {
        const auto free = std::find_if(table.begin(), table.end(), [](const auto &entry) { return !entry; });
        if (free == table.end()) return {atrac_error::kNoAtracId, {}};
        slot = static_cast<std::uint32_t>(free - table.begin());
    }
    context->loaded = true;
    context->buffer = buffer;
    context->buffer_size = buffer_size;
    context->streaming = streaming;
    if (streaming) store_added(memory, *context, buffer, read_size);
    std::ostringstream details;
    details << "file=" << track.file_size << " align=" << track.block_align << " channels=" << track.channels
            << " end=" << track.end_sample << " loop=" << track.loop_start << ".." << track.loop_end
            << " skip=" << track.skip;
    if (streaming) details << " streamed, " << read_size << " of it in a buffer of " << buffer_size;
    table[slot] = std::move(context);
    return {slot, details.str()};
}

void register_atrac_functions(HleRegistrar &hle) {
    // sceAtracSetDataAndGetID(buffer, bufferSize) -> atracID
    hle.add("sceAtrac3plus", "sceAtracSetDataAndGetID", [](Runtime &rt, AllegrexContext &ctx) {
        const auto id = load_track(rt.memory(), std::nullopt, arg(ctx, 0), arg(ctx, 1), arg(ctx, 1), false);
        finish_traced(ctx, "sceAtracSetDataAndGetID", id.result, id.details);
    });
    // sceAtracSetHalfwayBufferAndGetID(buffer, readSize, bufferSize) -> atracID:
    // the first readSize bytes of the file are in a buffer of bufferSize, and
    // the game adds the rest through sceAtracAddStreamData as it plays.
    hle.add("sceAtrac3plus", "sceAtracSetHalfwayBufferAndGetID", [](Runtime &rt, AllegrexContext &ctx) {
        const auto id = load_track(rt.memory(), std::nullopt, arg(ctx, 0), arg(ctx, 1), arg(ctx, 2), true);
        finish_traced(ctx, "sceAtracSetHalfwayBufferAndGetID", id.result, id.details);
    });
    // sceAtracGetAtracID(codecType) -> an id with no data yet, for SetData.
    hle.add("sceAtrac3plus", "sceAtracGetAtracID", [](Runtime &, AllegrexContext &ctx) {
        auto &table = contexts();
        const auto slot = std::find_if(table.begin(), table.end(), [](const auto &entry) { return !entry; });
        if (slot == table.end()) {
            finish_traced(ctx, "sceAtracGetAtracID", atrac_error::kNoAtracId);
            return;
        }
        *slot = std::make_unique<AtracContext>();
        (*slot)->codec_type = arg(ctx, 0);
        finish_traced(ctx, "sceAtracGetAtracID", static_cast<std::uint32_t>(slot - table.begin()));
    });
    // sceAtracSetData(id, buffer, bufferSize): SetDataAndGetID for an id
    // already handed out.
    hle.add("sceAtrac3plus", "sceAtracSetData", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t id = arg(ctx, 0);
        if (!id_in_use(id)) {
            finish_traced(ctx, "sceAtracSetData", context_error(id));
            return;
        }
        const auto loaded = load_track(rt.memory(), id, arg(ctx, 1), arg(ctx, 2), arg(ctx, 2), false);
        finish_traced(ctx, "sceAtracSetData", loaded.result == id ? 0u : loaded.result, loaded.details);
    });
    // sceAtracLowLevelInitDecoder(id, params): the game will hand the decoder
    // raw frames itself (sceAtracLowLevelDecode), with no file around them.
    // params is three words: the stream's channels, the channels it wants
    // out, and the bytes of one frame.
    hle.try_add("sceAtrac3plus", "sceAtracLowLevelInitDecoder", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t id = arg(ctx, 0);
        if (!id_in_use(id)) {
            finish_traced(ctx, "sceAtracLowLevelInitDecoder", context_error(id));
            return;
        }
        AtracContext &context = *contexts()[id];
        const auto &memory = rt.memory();
        const std::uint32_t params = arg(ctx, 1);
        if (!memory.contains(params, 12u)) {
            finish_traced(ctx, "sceAtracLowLevelInitDecoder", atrac_error::kBadCodecParam);
            return;
        }
        const std::uint32_t channels = memory.load32(params);
        const std::uint32_t out_channels = memory.load32(params + 4u);
        const std::uint32_t frame_bytes = memory.load32(params + 8u);
        const bool atrac3 = context.codec_type == 0x1001u;
        std::vector<std::uint8_t> extradata;
        if (atrac3) {
            // What a WAVE file's fmt chunk would carry for ATRAC3: 1, the
            // samples per channel of a frame (32 bits), the coding mode and
            // its copy, a frame factor of 1, and 0. Joint stereo only for the
            // smallest stereo frames (192 bytes): Tenkawa's 304-byte stereo
            // frames fail to decode as joint stereo and decode as plain.
            const auto joint = static_cast<std::uint8_t>(channels == 2u && frame_bytes <= 0xC0u ? 1u : 0u);
            extradata = {1, 0, 0x00, 0x04, 0, 0, joint, 0, joint, 0, 1, 0, 0, 0};
        }
        const bool opened = context.decoder.open(atrac3 ? audio::AtracCodec::Atrac3 : audio::AtracCodec::Atrac3Plus,
                                                 channels, frame_bytes, extradata);
        context.low_level = opened;
        context.low_level_frame_bytes = frame_bytes;
        context.low_level_out_channels = out_channels == 1u ? 1u : 2u;
        std::ostringstream details;
        details << (atrac3 ? "ATRAC3" : "ATRAC3plus") << " channels=" << channels << " out=" << out_channels
                << " frame=" << frame_bytes << (opened ? "" : " (the decoder refused it)");
        finish_traced(ctx, "sceAtracLowLevelInitDecoder", opened ? 0u : atrac_error::kBadCodecParam, details.str());
    });
    // sceAtracLowLevelDecode(id, source, sourceBytesConsumed, samples,
    // sampleBytesWritten): one frame from source, decoded into samples.
    hle.try_add("sceAtrac3plus", "sceAtracLowLevelDecode", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t id = arg(ctx, 0);
        if (!id_in_use(id) || !contexts()[id]->low_level) {
            finish_traced(ctx, "sceAtracLowLevelDecode", id_in_use(id) ? atrac_error::kBadCodecParam : context_error(id));
            return;
        }
        AtracContext &context = *contexts()[id];
        auto &memory = rt.memory();
        const std::uint32_t source = arg(ctx, 1);
        const std::uint32_t consumed = arg(ctx, 2);
        const std::uint32_t samples = arg(ctx, 3);
        const std::uint32_t written = arg(ctx, 4);
        const std::uint32_t bytes = context.low_level_frame_bytes;
        const std::uint8_t *frame = memory.raw_pointer(source, bytes);
        std::vector<std::int16_t> decoded(audio::atrac_frame_samples(context.decoder.codec()) * 2u);
        const std::size_t count =
            frame != nullptr ? context.decoder.decode(std::span<const std::uint8_t>(frame, bytes), decoded.data()) : 0u;
        const std::uint32_t out_channels = context.low_level_out_channels;
        const std::uint32_t out_bytes = static_cast<std::uint32_t>(count) * out_channels * 2u;
        if (count != 0u && memory.contains(samples, out_bytes)) {
            for (std::size_t i = 0; i < count; ++i) {
                if (out_channels == 1u) {
                    const auto mono = static_cast<std::int16_t>((decoded[i * 2u] + decoded[i * 2u + 1u]) / 2);
                    memory.store16(samples + static_cast<std::uint32_t>(i) * 2u, static_cast<std::uint16_t>(mono));
                } else {
                    memory.store16(samples + static_cast<std::uint32_t>(i) * 4u, static_cast<std::uint16_t>(decoded[i * 2u]));
                    memory.store16(samples + static_cast<std::uint32_t>(i) * 4u + 2u,
                                   static_cast<std::uint16_t>(decoded[i * 2u + 1u]));
                }
            }
        }
        if (consumed != 0u && memory.contains(consumed, 4u)) memory.store32(consumed, bytes);
        if (written != 0u && memory.contains(written, 4u)) memory.store32(written, out_bytes);
        finish_traced(ctx, "sceAtracLowLevelDecode", 0u, count == 0u ? "nothing decoded" : "");
    });
    // sceAtracReinit(at3plusIds, at3Ids): how many ids each codec may use.
    // Every id here decodes either, so there is nothing to divide.
    hle.add("sceAtrac3plus", "sceAtracReinit", [](Runtime &, AllegrexContext &ctx) {
        finish_traced(ctx, "sceAtracReinit", 0u);
    });

    hle.add("sceAtrac3plus", "sceAtracReleaseAtracID", [](Runtime &, AllegrexContext &ctx) {
        const std::uint32_t id = arg(ctx, 0);
        if (!id_in_use(id)) {
            finish_traced(ctx, "sceAtracReleaseAtracID", context_error(id));
            return;
        }
        release_context(id);
        finish_traced(ctx, "sceAtracReleaseAtracID", 0u);
    });

    // sceAtracDecodeData(id, outSamples, outCount, outEnd, outRemainFrame):
    // one frame, or what is left of it before the loop end or the track end.
    hle.add("sceAtrac3plus", "sceAtracDecodeData", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t id = arg(ctx, 0);
        AtracContext *context = find_context(id);
        if (context == nullptr) {
            finish_traced(ctx, "sceAtracDecodeData", context_error(id));
            return;
        }
        auto &memory = rt.memory();
        const std::uint32_t output = arg(ctx, 1);
        const std::uint32_t count_address = arg(ctx, 2);
        const std::uint32_t end_address = arg(ctx, 3);
        const std::uint32_t remain_address = arg(ctx, 4);
        const TrackInfo &track = context->track;
        write_s32(memory, remain_address, remain_frames(*context));
        if (context->position > track.end_sample) {
            write_s32(memory, count_address, 0);
            write_s32(memory, end_address, 1);
            finish_traced(ctx, "sceAtracDecodeData", atrac_error::kAllDataDecoded);
            return;
        }

        // Past the loop end already (the loop count was set late), play out.
        const bool loops = looping(*context) && context->position <= track.loop_end;
        const std::int32_t stop = loops ? track.loop_end : track.end_sample;
        const auto frame_samples = static_cast<std::int64_t>(audio::atrac_frame_samples(track.codec));
        const std::int64_t decoded_index = static_cast<std::int64_t>(context->position) + track.skip;
        const std::int64_t frame = decoded_index / frame_samples;
        const std::int64_t first = decoded_index % frame_samples;
        const auto count = static_cast<std::int32_t>(
            std::min<std::int64_t>(frame_samples - first, static_cast<std::int64_t>(stop) - context->position + 1));
        const bool decoded = load_frame(memory, *context, frame);
        if (!decoded && context->streaming)
            log_once("atrac-underrun", "[atrac] a streamed track was decoded past the data the game had added; "
                                       "that frame is silent");
        if (output != 0u) {
            if (std::uint8_t *destination = memory.raw_pointer(output, static_cast<std::size_t>(count) * 4u)) {
                const std::int16_t *source = context->cached.data() + first * 2;
                for (std::int32_t i = 0; i < count * 2; ++i) {
                    const auto value = static_cast<std::uint16_t>(source[i]);
                    destination[i * 2] = static_cast<std::uint8_t>(value);
                    destination[i * 2 + 1] = static_cast<std::uint8_t>(value >> 8u);
                }
            }
        }
        const std::int32_t played = context->position;
        context->position += count;
        if (context->position > stop && loops) {
            context->position = track.loop_start;
            ++context->loops_done;
            if (context->loop_num > 0) --context->loop_num;
        }
        const bool ended = context->position > track.end_sample;
        if (context->streaming) trim_stored(*context);
        write_s32(memory, count_address, count);
        write_s32(memory, end_address, ended ? 1 : 0);
        write_s32(memory, remain_address, remain_frames(*context));
        if (trace_atrac()) {
            std::ostringstream details;
            details << "at=" << played << " count=" << count << " next=" << context->position
                    << (decoded ? "" : " (silent)") << (ended ? " end" : "");
            finish_traced(ctx, "sceAtracDecodeData", 0u, details.str());
            return;
        }
        kernel().finish(ctx, 0u);
    });

    // sceAtracGetRemainFrame(id, outRemainFrame)
    hle.add("sceAtrac3plus", "sceAtracGetRemainFrame", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t id = arg(ctx, 0);
        const AtracContext *context = find_context(id);
        if (context == nullptr) {
            finish_traced(ctx, "sceAtracGetRemainFrame", context_error(id));
            return;
        }
        const std::int32_t remain = remain_frames(*context);
        write_s32(rt.memory(), arg(ctx, 1), remain);
        finish_traced(ctx, "sceAtracGetRemainFrame", 0u, "remain=" + std::to_string(remain));
    });

    // sceAtracGetSoundSample(id, outEndSample, outLoopStart, outLoopEnd)
    hle.add("sceAtrac3plus", "sceAtracGetSoundSample", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t id = arg(ctx, 0);
        const AtracContext *context = find_context(id);
        if (context == nullptr) {
            finish_traced(ctx, "sceAtracGetSoundSample", context_error(id));
            return;
        }
        write_s32(rt.memory(), arg(ctx, 1), context->track.end_sample);
        write_s32(rt.memory(), arg(ctx, 2), context->track.loop_start);
        write_s32(rt.memory(), arg(ctx, 3), context->track.loop_end);
        finish_traced(ctx, "sceAtracGetSoundSample", 0u);
    });

    // sceAtracGetBitrate(id, outKbps)
    hle.add("sceAtrac3plus", "sceAtracGetBitrate", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t id = arg(ctx, 0);
        const AtracContext *context = find_context(id);
        if (context == nullptr) {
            finish_traced(ctx, "sceAtracGetBitrate", context_error(id));
            return;
        }
        // Bytes per frame times frames per second, in the library's rounding.
        std::uint32_t bitrate = context->track.block_align * 352'800u / 1000u;
        if (context->track.codec == audio::AtracCodec::Atrac3) bitrate = (bitrate + 511u) >> 10u;
        else bitrate = ((bitrate >> 11u) + 8u) & 0xFFFFFFF0u;
        rt.memory().store32(arg(ctx, 1), bitrate);
        finish_traced(ctx, "sceAtracGetBitrate", 0u, "kbps=" + std::to_string(bitrate));
    });

    // sceAtracSetLoopNum(id, loops): -1 loops forever.
    hle.add("sceAtrac3plus", "sceAtracSetLoopNum", [](Runtime &, AllegrexContext &ctx) {
        const std::uint32_t id = arg(ctx, 0);
        AtracContext *context = find_context(id);
        if (context == nullptr) {
            finish_traced(ctx, "sceAtracSetLoopNum", context_error(id));
            return;
        }
        if (context->track.loop_start < 0) {
            finish_traced(ctx, "sceAtracSetLoopNum", atrac_error::kNoLoopInformation);
            return;
        }
        context->loop_num = static_cast<std::int32_t>(arg(ctx, 1));
        // Loops set after the whole file was added: the next data the game
        // adds is the loop's again.
        if (context->streaming && context->cursor >= context->track.file_size && looping(*context)) {
            context->cursor = loop_offset(*context);
            ++context->wraps;
        }
        finish_traced(ctx, "sceAtracSetLoopNum", 0u);
    });

    // sceAtracGetLoopStatus(id, outLoopNum, outLoopStatus)
    hle.add("sceAtrac3plus", "sceAtracGetLoopStatus", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t id = arg(ctx, 0);
        const AtracContext *context = find_context(id);
        if (context == nullptr) {
            finish_traced(ctx, "sceAtracGetLoopStatus", context_error(id));
            return;
        }
        write_s32(rt.memory(), arg(ctx, 1), context->loop_num);
        write_s32(rt.memory(), arg(ctx, 2), looping(*context) ? 1 : 0);
        finish_traced(ctx, "sceAtracGetLoopStatus", 0u);
    });

    // sceAtracGetNextDecodePosition(id, outSample)
    hle.add("sceAtrac3plus", "sceAtracGetNextDecodePosition", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t id = arg(ctx, 0);
        const AtracContext *context = find_context(id);
        if (context == nullptr) {
            finish_traced(ctx, "sceAtracGetNextDecodePosition", context_error(id));
            return;
        }
        if (context->position > context->track.end_sample) {
            finish_traced(ctx, "sceAtracGetNextDecodePosition", atrac_error::kAllDataDecoded);
            return;
        }
        write_s32(rt.memory(), arg(ctx, 1), context->position);
        finish_traced(ctx, "sceAtracGetNextDecodePosition", 0u, "position=" + std::to_string(context->position));
    });

    // sceAtracGetStreamDataInfo(id, outWritePointer, outWritableBytes, outReadOffset):
    // where the game writes next, how much, and from where in the file. With
    // the whole file in memory there is never room to add data.
    hle.add("sceAtrac3plus", "sceAtracGetStreamDataInfo", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t id = arg(ctx, 0);
        const AtracContext *context = find_context(id);
        if (context == nullptr) {
            finish_traced(ctx, "sceAtracGetStreamDataInfo", context_error(id));
            return;
        }
        auto &memory = rt.memory();
        StreamWrite write{context->buffer, 0u, context->track.file_size};
        if (context->streaming) write = stream_write(*context);
        if (arg(ctx, 1) != 0u) memory.store32(arg(ctx, 1), write.address);
        if (arg(ctx, 2) != 0u) memory.store32(arg(ctx, 2), write.writable);
        if (arg(ctx, 3) != 0u) memory.store32(arg(ctx, 3), write.file_offset);
        std::ostringstream details;
        details << "write=" << psprecomp::hex32(write.address) << " bytes=" << write.writable
                << " from=" << write.file_offset;
        finish_traced(ctx, "sceAtracGetStreamDataInfo", 0u, details.str());
    });
    // sceAtracGetSecondBufferInfo(id, outPosition, outBytes): where a
    // looping streamed track wants the file's end kept, when its buffer
    // cannot hold it. Tracks loop here from the bytes the game added the
    // first time (see trim_stored), so none ever needs one. God of War
    // (UCES00842) asks after setting up its streamed music.
    hle.add("sceAtrac3plus", "sceAtracGetSecondBufferInfo", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t id = arg(ctx, 0);
        if (find_context(id) == nullptr) {
            finish_traced(ctx, "sceAtracGetSecondBufferInfo", context_error(id));
            return;
        }
        write_s32(rt.memory(), arg(ctx, 1), 0);
        write_s32(rt.memory(), arg(ctx, 2), 0);
        finish_traced(ctx, "sceAtracGetSecondBufferInfo", atrac_error::kSecondBufferNotNeeded);
    });
    // sceAtracAddStreamData(id, bytesAdded): the game has written that many
    // bytes where GetStreamDataInfo said.
    hle.add("sceAtrac3plus", "sceAtracAddStreamData", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t id = arg(ctx, 0);
        AtracContext *context = find_context(id);
        if (context == nullptr) {
            finish_traced(ctx, "sceAtracAddStreamData", context_error(id));
            return;
        }
        const std::uint32_t added = arg(ctx, 1);
        if (!context->streaming) {
            finish_traced(ctx, "sceAtracAddStreamData", added == 0u ? 0u : atrac_error::kParamFail, "not streamed");
            return;
        }
        const StreamWrite write = stream_write(*context);
        if (added > write.writable) {
            finish_traced(ctx, "sceAtracAddStreamData", atrac_error::kParamFail,
                          "more than the " + std::to_string(write.writable) + " bytes there was room for");
            return;
        }
        store_added(rt.memory(), *context, write.address, added);
        finish_traced(ctx, "sceAtracAddStreamData", 0u,
                      "written=" + std::to_string(context->written) + " next=" + std::to_string(context->cursor));
    });
    // sceAtracGetNextSample(id, outSamples): how many samples the next
    // DecodeData returns.
    hle.add("sceAtrac3plus", "sceAtracGetNextSample", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t id = arg(ctx, 0);
        const AtracContext *context = find_context(id);
        if (context == nullptr) {
            finish_traced(ctx, "sceAtracGetNextSample", context_error(id));
            return;
        }
        const TrackInfo &track = context->track;
        std::int32_t count = 0;
        if (context->position <= track.end_sample) {
            const bool loops = looping(*context) && context->position <= track.loop_end;
            const std::int32_t stop = loops ? track.loop_end : track.end_sample;
            const auto frame_samples = static_cast<std::int64_t>(audio::atrac_frame_samples(track.codec));
            const std::int64_t first = (static_cast<std::int64_t>(context->position) + track.skip) % frame_samples;
            count = static_cast<std::int32_t>(
                std::min<std::int64_t>(frame_samples - first, static_cast<std::int64_t>(stop) - context->position + 1));
        }
        write_s32(rt.memory(), arg(ctx, 1), count);
        finish_traced(ctx, "sceAtracGetNextSample", 0u, "samples=" + std::to_string(count));
    });

    // sceAtracGetBufferInfoForResetting(id, sample, outBufferInfo): where data
    // for a restart at `sample` would have to go. Nothing, with the whole file
    // in memory. The structure is two {writePointer, writableBytes,
    // minWriteBytes, readOffset} records, for the first and second buffer.
    hle.add("sceAtrac3plus", "sceAtracGetBufferInfoForResetting", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t id = arg(ctx, 0);
        const AtracContext *context = find_context(id);
        if (context == nullptr) {
            finish_traced(ctx, "sceAtracGetBufferInfoForResetting", context_error(id));
            return;
        }
        const auto sample = static_cast<std::int32_t>(arg(ctx, 1));
        const std::uint32_t info = arg(ctx, 2);
        if (info == 0u) {
            finish_traced(ctx, "sceAtracGetBufferInfoForResetting", atrac_error::kParamFail);
            return;
        }
        if (sample < 0 || sample > context->track.end_sample) {
            finish_traced(ctx, "sceAtracGetBufferInfoForResetting", atrac_error::kBadSample);
            return;
        }
        auto &memory = rt.memory();
        std::array<std::uint32_t, 8> fields = {context->buffer, 0u, 0u, context->track.file_size,
                                               context->buffer, 0u, 0u, 0u};
        if (context->streaming) {
            // Refill the buffer from the start of the frame the sample is in.
            const std::uint32_t from = reset_offset(*context, sample);
            const std::uint32_t ring = ring_frames(*context) * context->track.block_align;
            fields[0] = ring_address(*context, from);
            fields[1] = std::min(ring - (fields[0] - context->buffer - context->track.data_offset),
                                 context->track.file_size - from);
            fields[2] = std::min(fields[1], context->track.block_align);
            fields[3] = from;
        }
        for (std::size_t i = 0; i < fields.size(); ++i) memory.store32(info + static_cast<std::uint32_t>(i * 4u), fields[i]);
        finish_traced(ctx, "sceAtracGetBufferInfoForResetting", 0u);
    });

    // sceAtracResetPlayPosition(id, sample, bytesWrittenFirst, bytesWrittenSecond)
    hle.add("sceAtrac3plus", "sceAtracResetPlayPosition", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t id = arg(ctx, 0);
        AtracContext *context = find_context(id);
        if (context == nullptr) {
            finish_traced(ctx, "sceAtracResetPlayPosition", context_error(id));
            return;
        }
        const auto sample = static_cast<std::int32_t>(arg(ctx, 1));
        if (sample < 0 || sample > context->track.end_sample) {
            finish_traced(ctx, "sceAtracResetPlayPosition", atrac_error::kBadSample);
            return;
        }
        if (context->streaming) {
            // The game has refilled the buffer from where
            // GetBufferInfoForResetting said.
            const std::uint32_t from = reset_offset(*context, sample);
            const std::uint32_t added = std::min(arg(ctx, 2), context->buffer_size);
            const std::uint32_t address = ring_address(*context, from);
            context->stored.clear();
            context->written = from;
            context->cursor = from;
            context->wraps = 0u;
            context->loops_done = 0u;
            store_added(rt.memory(), *context, address, added);
            context->cached_frame = -1;
            context->next_frame = -1;
        }
        context->position = sample;
        finish_traced(ctx, "sceAtracResetPlayPosition", 0u);
    });
}

} // namespace

void register_atrac(HleRegistrar &hle) {
    if (!audio::AtracDecoder::available()) {
        std::cout << "Audio: built without FFmpeg; streamed music is silent\n";
        return;
    }
    register_atrac_functions(hle);
}

} // namespace portablekit
