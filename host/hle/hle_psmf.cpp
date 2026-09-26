// scePsmf: the library a game uses to read a PSMF movie's header before it
// feeds the stream data to sceMpeg itself: how many streams of which kind
// there are, which one to play, and the picture size. The game hands over
// a SceuPsmf of its own; what the library keeps in it is not the game's to
// read, so the parsed header is kept here, by that structure's address.
//
// Traced in Patapon (UCES00995), which checks a new game's intro movie this
// way: SetPsmf, GetPsmfVersion (must not be negative), the number of
// streams of types 0 to 3 and in all, then for each stream SpecifyStream,
// GetCurrentStreamType and, for video, GetVideoInfo (width, height). The
// calls no traced game has made yet say UNVERIFIED on their first use, and
// the error codes are the library's as documented, not traced.

#include "../profile.hpp"
#include "hle_common.hpp"

#include "movie/psmf_header.hpp"
#include "psprecomp/common.hpp"

#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace portablekit {
namespace {

constexpr std::uint32_t kNotInitialized = 0x80615001u;  // no SetPsmf on this structure
constexpr std::uint32_t kNotFound = 0x80615025u;        // no such stream
constexpr std::uint32_t kInvalidPsmf = 0x80615501u;     // not a PSMF header
constexpr std::uint32_t kInvalidId = 0x80615100u;       // the current stream is of another kind

// A header and its stream table fit in the first sector of the file.
constexpr std::uint32_t kHeaderBytes = 2048u;

struct Psmf {
    movie::PsmfHeader header;
    int current{-1};
};

std::map<std::uint32_t, Psmf> &psmfs() {
    static std::map<std::uint32_t, Psmf> table;
    return table;
}

bool trace_psmf() {
    static const bool enabled = portablekit::env("TRACE_MPEG") != nullptr;
    return enabled;
}

void unverified(const char *name) {
    static std::set<std::string> said;
    if (said.insert(name).second) std::cerr << "[psmf] " << name << " (UNVERIFIED: no game traced yet)\n";
}

void finish(AllegrexContext &ctx, const char *name, std::uint32_t result) {
    if (trace_psmf())
        std::cerr << "[psmf] " << name << "(" << psprecomp::hex32(arg(ctx, 0)) << ", " << psprecomp::hex32(arg(ctx, 1))
                  << ", " << psprecomp::hex32(arg(ctx, 2)) << ") -> " << psprecomp::hex32(result) << "\n";
    kernel().finish(ctx, result);
}

std::optional<movie::PsmfHeader> read_header(const psprecomp::GuestMemory &memory, std::uint32_t address) {
    if (!memory.contains(address, kHeaderBytes)) return std::nullopt;
    std::vector<std::uint8_t> bytes(kHeaderBytes);
    memory.copy_out(address, bytes);
    return movie::parse_psmf_header(bytes);
}

// The structure's state, or the error that ends the call.
Psmf *find(AllegrexContext &ctx, const char *name) {
    const auto found = psmfs().find(arg(ctx, 0));
    if (found == psmfs().end()) {
        finish(ctx, name, kNotInitialized);
        return nullptr;
    }
    return &found->second;
}

const movie::PsmfStream *current(Psmf &psmf) {
    if (psmf.current < 0 || static_cast<std::size_t>(psmf.current) >= psmf.header.streams.size()) return nullptr;
    return &psmf.header.streams[static_cast<std::size_t>(psmf.current)];
}

} // namespace

void register_psmf(HleRegistrar &hle) {
    // scePsmfSetPsmf(psmf, data): the header at `data` describes the movie.
    hle.add("scePsmf", "scePsmfSetPsmf", [](Runtime &rt, AllegrexContext &ctx) {
        const auto header = read_header(rt.memory(), arg(ctx, 1));
        if (!header) {
            finish(ctx, "scePsmfSetPsmf", kInvalidPsmf);
            return;
        }
        psmfs()[arg(ctx, 0)] = Psmf{*header, -1};
        finish(ctx, "scePsmfSetPsmf", 0u);
    });
    hle.add("scePsmf", "scePsmfVerifyPsmf", [](Runtime &rt, AllegrexContext &ctx) {
        unverified("scePsmfVerifyPsmf");
        finish(ctx, "scePsmfVerifyPsmf", read_header(rt.memory(), arg(ctx, 0)) ? 0u : kInvalidPsmf);
    });
    // The version's four ASCII digits as a word, first byte lowest. The
    // game traced only tests that it is not negative.
    hle.add("scePsmf", "scePsmfGetPsmfVersion", [](Runtime &, AllegrexContext &ctx) {
        if (Psmf *psmf = find(ctx, "scePsmfGetPsmfVersion")) finish(ctx, "scePsmfGetPsmfVersion", psmf->header.version);
    });
    hle.add("scePsmf", "scePsmfGetNumberOfStreams", [](Runtime &, AllegrexContext &ctx) {
        if (Psmf *psmf = find(ctx, "scePsmfGetNumberOfStreams"))
            finish(ctx, "scePsmfGetNumberOfStreams", static_cast<std::uint32_t>(psmf->header.streams.size()));
    });
    // (psmf, type): 0 AVC, 1 ATRAC3plus, 2 PCM, 3 anything else.
    hle.add("scePsmf", "scePsmfGetNumberOfSpecificStreams", [](Runtime &, AllegrexContext &ctx) {
        Psmf *psmf = find(ctx, "scePsmfGetNumberOfSpecificStreams");
        if (psmf == nullptr) return;
        std::uint32_t count = 0u;
        for (const auto &stream : psmf->header.streams)
            if (stream.type() == static_cast<int>(arg(ctx, 1))) ++count;
        finish(ctx, "scePsmfGetNumberOfSpecificStreams", count);
    });
    // (psmf, index): the stream later calls are about.
    hle.add("scePsmf", "scePsmfSpecifyStream", [](Runtime &, AllegrexContext &ctx) {
        Psmf *psmf = find(ctx, "scePsmfSpecifyStream");
        if (psmf == nullptr) return;
        const auto index = static_cast<std::int32_t>(arg(ctx, 1));
        if (index < 0 || static_cast<std::size_t>(index) >= psmf->header.streams.size()) {
            finish(ctx, "scePsmfSpecifyStream", kNotFound);
            return;
        }
        psmf->current = index;
        finish(ctx, "scePsmfSpecifyStream", 0u);
    });
    // (psmf, type, channel)
    hle.add("scePsmf", "scePsmfSpecifyStreamWithStreamType", [](Runtime &, AllegrexContext &ctx) {
        Psmf *psmf = find(ctx, "scePsmfSpecifyStreamWithStreamType");
        if (psmf == nullptr) return;
        unverified("scePsmfSpecifyStreamWithStreamType");
        for (std::size_t i = 0; i < psmf->header.streams.size(); ++i) {
            const auto &stream = psmf->header.streams[i];
            if (stream.type() == static_cast<int>(arg(ctx, 1)) && stream.channel() == static_cast<int>(arg(ctx, 2))) {
                psmf->current = static_cast<int>(i);
                finish(ctx, "scePsmfSpecifyStreamWithStreamType", 0u);
                return;
            }
        }
        finish(ctx, "scePsmfSpecifyStreamWithStreamType", kNotFound);
    });
    // (psmf, type, n): the n-th stream of that type.
    hle.add("scePsmf", "scePsmfSpecifyStreamWithStreamTypeNumber", [](Runtime &, AllegrexContext &ctx) {
        Psmf *psmf = find(ctx, "scePsmfSpecifyStreamWithStreamTypeNumber");
        if (psmf == nullptr) return;
        unverified("scePsmfSpecifyStreamWithStreamTypeNumber");
        std::uint32_t seen = 0u;
        for (std::size_t i = 0; i < psmf->header.streams.size(); ++i) {
            if (psmf->header.streams[i].type() != static_cast<int>(arg(ctx, 1))) continue;
            if (seen++ == arg(ctx, 2)) {
                psmf->current = static_cast<int>(i);
                finish(ctx, "scePsmfSpecifyStreamWithStreamTypeNumber", 0u);
                return;
            }
        }
        finish(ctx, "scePsmfSpecifyStreamWithStreamTypeNumber", kNotFound);
    });
    hle.add("scePsmf", "scePsmfGetCurrentStreamNumber", [](Runtime &, AllegrexContext &ctx) {
        Psmf *psmf = find(ctx, "scePsmfGetCurrentStreamNumber");
        if (psmf == nullptr) return;
        unverified("scePsmfGetCurrentStreamNumber");
        finish(ctx, "scePsmfGetCurrentStreamNumber",
               psmf->current < 0 ? kNotFound : static_cast<std::uint32_t>(psmf->current));
    });
    // (psmf, int *type, int *channel)
    hle.add("scePsmf", "scePsmfGetCurrentStreamType", [](Runtime &rt, AllegrexContext &ctx) {
        Psmf *psmf = find(ctx, "scePsmfGetCurrentStreamType");
        if (psmf == nullptr) return;
        const movie::PsmfStream *stream = current(*psmf);
        if (stream == nullptr) {
            finish(ctx, "scePsmfGetCurrentStreamType", kNotFound);
            return;
        }
        auto &memory = rt.memory();
        if (arg(ctx, 1) != 0u) memory.store32(arg(ctx, 1), static_cast<std::uint32_t>(stream->type()));
        if (arg(ctx, 2) != 0u) memory.store32(arg(ctx, 2), static_cast<std::uint32_t>(stream->channel()));
        finish(ctx, "scePsmfGetCurrentStreamType", 0u);
    });
    // (psmf, info): width and height in pixels.
    hle.add("scePsmf", "scePsmfGetVideoInfo", [](Runtime &rt, AllegrexContext &ctx) {
        Psmf *psmf = find(ctx, "scePsmfGetVideoInfo");
        if (psmf == nullptr) return;
        const movie::PsmfStream *stream = current(*psmf);
        if (stream == nullptr || stream->type() != 0) {
            finish(ctx, "scePsmfGetVideoInfo", stream == nullptr ? kNotFound : kInvalidId);
            return;
        }
        if (arg(ctx, 1) != 0u) {
            rt.memory().store32(arg(ctx, 1), stream->info[0] * 16u);
            rt.memory().store32(arg(ctx, 1) + 4u, stream->info[1] * 16u);
        }
        finish(ctx, "scePsmfGetVideoInfo", 0u);
    });
    // (psmf, info): channels, and the sample rate (code 2 is 44.1 kHz, the
    // only rate PSMF audio has been seen at).
    hle.add("scePsmf", "scePsmfGetAudioInfo", [](Runtime &rt, AllegrexContext &ctx) {
        Psmf *psmf = find(ctx, "scePsmfGetAudioInfo");
        if (psmf == nullptr) return;
        unverified("scePsmfGetAudioInfo");
        const movie::PsmfStream *stream = current(*psmf);
        if (stream == nullptr || (stream->type() != 1 && stream->type() != 2)) {
            finish(ctx, "scePsmfGetAudioInfo", stream == nullptr ? kNotFound : kInvalidId);
            return;
        }
        if (arg(ctx, 1) != 0u) {
            rt.memory().store32(arg(ctx, 1), stream->info[0]);
            rt.memory().store32(arg(ctx, 1) + 4u, stream->info[1] == 2u ? 44100u : 0u);
        }
        finish(ctx, "scePsmfGetAudioInfo", 0u);
    });
    // (psmf, unsigned *out) for each.
    const auto write_out = [](const char *name, std::uint32_t (*value)(const movie::PsmfHeader &)) {
        return [name, value](Runtime &rt, AllegrexContext &ctx) {
            Psmf *psmf = find(ctx, name);
            if (psmf == nullptr) return;
            unverified(name);
            if (arg(ctx, 1) != 0u) rt.memory().store32(arg(ctx, 1), value(psmf->header));
            finish(ctx, name, 0u);
        };
    };
    hle.add("scePsmf", "scePsmfGetHeaderSize",
            write_out("scePsmfGetHeaderSize", [](const movie::PsmfHeader &h) { return h.header_size; }));
    hle.add("scePsmf", "scePsmfGetStreamSize",
            write_out("scePsmfGetStreamSize", [](const movie::PsmfHeader &h) { return h.stream_size; }));
    hle.add("scePsmf", "scePsmfGetPresentationStartTime",
            write_out("scePsmfGetPresentationStartTime",
                      [](const movie::PsmfHeader &h) { return static_cast<std::uint32_t>(h.start_time); }));
    hle.add("scePsmf", "scePsmfGetPresentationEndTime",
            write_out("scePsmfGetPresentationEndTime",
                      [](const movie::PsmfHeader &h) { return static_cast<std::uint32_t>(h.end_time); }));
}

} // namespace portablekit
