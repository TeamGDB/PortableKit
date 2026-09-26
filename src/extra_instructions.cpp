#include "psprecomp/extra_instructions.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/runtime.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>

// UNVERIFIED: not yet exercised by a real game. Every instruction in this
// file. docs/INSTRUCTION_COVERAGE.md lists them; take one out of here (into the
// ordinary decoder, interpreter and code generator paths, or leave it here and
// move it to the verified list) once a game has run it and its result has been
// checked against what the game expects.

namespace psprecomp {
namespace {

enum class Extra : std::uint8_t {
    None,
    Ll, Sc,
    LvlQ, LvrQ, SvlQ, SvrQ,
    Vsbn, Vdet, Vwbn,
    Vrnds, Vsbz, Vlgb, Vi2c, Vi2us,
    Vsrt1, Vsrt2, Vsrt3, Vsrt4, Vbfy2,
    Vmfvc, Vmtvc, Vt4444, Vt5551,
    Count,
};

constexpr std::array<std::string_view, static_cast<std::size_t>(Extra::Count)> kNames{
    "", "ll", "sc", "lvl.q", "lvr.q", "svl.q", "svr.q",
    "vsbn", "vdet", "vwbn",
    "vrnds", "vsbz", "vlgb", "vi2c", "vi2us",
    "vsrt1", "vsrt2", "vsrt3", "vsrt4", "vbfy2",
    "vmfvc", "vmtvc", "vt4444", "vt5551",
};

// The encodings, from the Allegrex opcode map: the major opcode in bits 26-31,
// and for the VFPU groups the sub-operation fields the decoder already uses
// for the neighbours of each of these.
Extra classify(std::uint32_t word) noexcept {
    const std::uint32_t op = word >> 26u;
    switch (op) {
    case 0x30u: return Extra::Ll;
    case 0x38u: return Extra::Sc;
    case 0x35u: return (word & 2u) == 0u ? Extra::LvlQ : Extra::LvrQ;
    case 0x3Du: return (word & 2u) == 0u ? Extra::SvlQ : Extra::SvrQ;
    case 0x18u: return ((word >> 23u) & 7u) == 2u ? Extra::Vsbn : Extra::None;
    case 0x19u: return ((word >> 23u) & 7u) == 6u ? Extra::Vdet : Extra::None;
    case 0x34u: {
        const std::uint32_t group = (word >> 21u) & 31u;
        const std::uint32_t operation = (word >> 16u) & 31u;
        if (group >= 24u) return Extra::Vwbn;  // 110100 11: an 8-bit exponent in bits 16-23
        if (group == 1u) {
            switch (operation) {
            case 0u: return Extra::Vrnds;
            case 22u: return Extra::Vsbz;
            case 23u: return Extra::Vlgb;
            case 29u: return Extra::Vi2c;
            case 30u: return Extra::Vi2us;
            default: return Extra::None;
            }
        }
        if (group == 2u) {
            switch (operation) {
            case 0u: return Extra::Vsrt1;
            case 1u: return Extra::Vsrt2;
            case 3u: return Extra::Vbfy2;
            case 8u: return Extra::Vsrt3;
            case 9u: return Extra::Vsrt4;
            case 16u: return Extra::Vmfvc;
            case 17u: return Extra::Vmtvc;
            case 25u: return Extra::Vt4444;
            case 26u: return Extra::Vt5551;
            default: return Extra::None;
            }
        }
        return Extra::None;
    }
    default:
        return Extra::None;
    }
}

std::atomic<std::uint32_t> g_reported{0u};

void report_first_use(Extra which, std::uint32_t pc, std::uint32_t word) {
    const std::uint32_t bit = 1u << static_cast<std::uint32_t>(which);
    if ((g_reported.fetch_or(bit, std::memory_order_relaxed) & bit) != 0u) return;
    std::fprintf(stderr,
                 "[instruction] first use of %.*s (0x%08X) at 0x%08X: UNVERIFIED, not yet exercised by a real "
                 "game; check the result and update docs/INSTRUCTION_COVERAGE.md\n",
                 static_cast<int>(kNames[static_cast<std::size_t>(which)].size()),
                 kNames[static_cast<std::size_t>(which)].data(), word, pc);
    std::fflush(stderr);
}

std::uint32_t vd_of(std::uint32_t word) noexcept { return word & 0x7Fu; }
std::uint32_t vs_of(std::uint32_t word) noexcept { return (word >> 8u) & 0x7Fu; }
std::uint32_t vt_of(std::uint32_t word) noexcept { return (word >> 16u) & 0x7Fu; }
std::uint32_t length_of(std::uint32_t word) noexcept {
    return (((word >> 7u) & 1u) | (((word >> 15u) & 1u) << 1u)) + 1u;
}
std::uint32_t bits(float value) noexcept { return std::bit_cast<std::uint32_t>(value); }
float from_bits(std::uint32_t value) noexcept { return std::bit_cast<float>(value); }

// A result whose first lane is computed and whose other lanes are the
// (prefixed) source's, as vsbn, vsbz, vlgb and vwbn do for a longer vector.
void write_first_lane_result(AllegrexContext &ctx, std::uint32_t word, float *source, float first) {
    source[0] = first;
    ctx.write_vfpu_vector_with_destination_prefix(source, vd_of(word), length_of(word));
}

std::uint32_t vector_register_of_memory_op(std::uint32_t word) noexcept {
    return ((word >> 16u) & 0x1Fu) | ((word & 1u) << 5u);
}
std::uint32_t address_of_memory_op(const AllegrexContext &ctx, std::uint32_t word, std::uint32_t offset_mask) noexcept {
    const auto offset = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(word & offset_mask)));
    return ctx.gpr[(word >> 21u) & 31u] + offset;
}

// Two lanes compared the way the sort steps compare them.
float lane_min(float a, float b) noexcept { return std::fmin(a, b); }
float lane_max(float a, float b) noexcept { return std::fmax(a, b); }

std::uint32_t pack_colour(std::uint32_t colour, bool is_4444) noexcept {
    const std::uint32_t r = colour & 0xFFu;
    const std::uint32_t g = (colour >> 8u) & 0xFFu;
    const std::uint32_t b = (colour >> 16u) & 0xFFu;
    const std::uint32_t a = (colour >> 24u) & 0xFFu;
    if (is_4444) return ((a >> 4u) << 12u) | ((b >> 4u) << 8u) | ((g >> 4u) << 4u) | (r >> 4u);
    return ((a >> 7u) << 15u) | ((b >> 3u) << 10u) | ((g >> 3u) << 5u) | (r >> 3u);
}

} // namespace

std::string_view extra_instruction_name(std::uint32_t word) noexcept {
    return kNames[static_cast<std::size_t>(classify(word))];
}

void execute_extra_instruction(Runtime &rt, AllegrexContext &ctx, std::uint32_t pc, std::uint32_t word) {
    const Extra which = classify(word);
    if (which == Extra::None) throw Error("execute_extra_instruction: not an extra instruction");
    report_first_use(which, pc, word);
    auto &memory = rt.memory();
    const std::uint32_t length = length_of(word);

    switch (which) {
    // ll/sc: the PSP has one core, and the runtime switches guest threads only
    // at system calls, so nothing can come between an ll and its sc: the sc
    // always succeeds and says so with rt = 1.
    case Extra::Ll: {
        const std::uint32_t rt_index = (word >> 16u) & 31u;
        const std::uint32_t value = memory.aot_load32(address_of_memory_op(ctx, word, 0xFFFFu));
        if (rt_index != 0u) ctx.gpr[rt_index] = value;
        break;
    }
    case Extra::Sc: {
        const std::uint32_t rt_index = (word >> 16u) & 31u;
        memory.aot_store32(address_of_memory_op(ctx, word, 0xFFFFu), ctx.gpr[rt_index]);
        if (rt_index != 0u) ctx.gpr[rt_index] = 1u;
        break;
    }

    // The unaligned quad pair. The address is word aligned; its word within
    // the 16-byte line is `offset`. lvl.q fills the top lanes with the words
    // from the start of the line up to the address (the address's word goes
    // to w), lvr.q the bottom lanes with the words from the address to the end
    // of the line (the address's word goes to x). The other lanes keep their
    // values; together the two load a quad at any word address. No prefixes.
    case Extra::LvlQ:
    case Extra::LvrQ:
    case Extra::SvlQ:
    case Extra::SvrQ: {
        const std::uint32_t vector = vector_register_of_memory_op(word);
        const std::uint32_t address = address_of_memory_op(ctx, word, 0xFFFCu);
        const std::uint32_t offset = (address >> 2u) & 3u;
        float lanes[4]{};
        ctx.read_vfpu_vector(lanes, vector, 4u);
        if (which == Extra::LvlQ) {
            for (std::uint32_t i = 0u; i <= offset; ++i) lanes[3u - i] = from_bits(memory.aot_load32(address - 4u * i));
            ctx.write_vfpu_vector(lanes, vector, 4u);
        } else if (which == Extra::LvrQ) {
            for (std::uint32_t i = 0u; i <= 3u - offset; ++i) lanes[i] = from_bits(memory.aot_load32(address + 4u * i));
            ctx.write_vfpu_vector(lanes, vector, 4u);
        } else if (which == Extra::SvlQ) {
            for (std::uint32_t i = 0u; i <= offset; ++i) memory.aot_store32(address - 4u * i, bits(lanes[3u - i]));
        } else {
            for (std::uint32_t i = 0u; i <= 3u - offset; ++i) memory.aot_store32(address + 4u * i, bits(lanes[i]));
        }
        break;
    }

    // vsbn: s.x with its exponent replaced by 127 + t.x read as an integer, so
    // 2^t times s.x's mantissa. Zero, denormals, infinities and NaNs pass.
    case Extra::Vsbn: {
        float source[4]{};
        float target[4]{};
        ctx.read_vfpu_vector_with_source_prefix(source, vs_of(word), length, 0u);
        ctx.read_vfpu_vector_with_source_prefix(target, vt_of(word), length, 1u);
        const std::uint32_t s = bits(source[0]);
        const std::uint32_t exponent = (127u + bits(target[0])) & 0xFFu;
        const std::uint32_t old_exponent = s & 0x7F800000u;
        const std::uint32_t result =
            old_exponent != 0u && old_exponent != 0x7F800000u ? (s & ~0x7F800000u) | (exponent << 23u) : s;
        write_first_lane_result(ctx, word, source, from_bits(result));
        break;
    }

    // vdet.p: s.x*t.y - s.y*t.x, into a single. The hardware rewrites the t
    // prefix's swizzle for this one; the ordinary prefixes are applied here.
    case Extra::Vdet: {
        float source[4]{};
        float target[4]{};
        ctx.read_vfpu_vector_with_source_prefix(source, vs_of(word), length, 0u);
        ctx.read_vfpu_vector_with_source_prefix(target, vt_of(word), length, 1u);
        const float result[4]{source[0] * target[1] - source[1] * target[0], 0.0f, 0.0f, 0.0f};
        ctx.write_vfpu_vector_with_destination_prefix(result, vd_of(word), 1u);
        break;
    }

    // vwbn: s.x rescaled to the exponent in bits 16-23, its mantissa (with the
    // hidden bit) shifted by the difference, at most 15 places. Zero,
    // denormals, infinities and NaNs get the exponent bits ORed in.
    case Extra::Vwbn: {
        float source[4]{};
        ctx.read_vfpu_vector_with_source_prefix(source, vs_of(word), length, 0u);
        const std::uint32_t exponent = (word >> 16u) & 0xFFu;
        const std::uint32_t s = bits(source[0]);
        const std::uint32_t old_exponent = (s >> 23u) & 0xFFu;
        std::uint32_t result = s | (exponent << 23u);
        if (old_exponent != 0u && old_exponent != 0xFFu) {
            std::uint32_t mantissa = (s & 0x007FFFFFu) | 0x00800000u;
            if (exponent > old_exponent) mantissa >>= (exponent - old_exponent) & 0xFu;
            else mantissa <<= (old_exponent - exponent) & 0xFu;
            result = (s & 0x80000000u) | (mantissa & 0x007FFFFFu) | (exponent << 23u);
        }
        write_first_lane_result(ctx, word, source, from_bits(result));
        break;
    }

    // vrnds: seeds the VFPU random generator from the integer bits of a
    // scalar (the register in the vd field, as the encoding places it). The
    // framework's generator is its own, not the console's, so the numbers
    // drawn after a seed differ from a PSP's in any case.
    case Extra::Vrnds: {
        float seed[4]{};
        ctx.read_vfpu_vector_with_source_prefix(seed, vd_of(word), 1u, 0u);
        const std::uint32_t value = bits(seed[0]);
        ctx.vfpu_ctrl[8] = value ^ 0x9E3779B9u;
        ctx.vfpu_ctrl[9] = value * 0x85EBCA6Bu + 0x243F6A88u;
        ctx.vfpu_ctrl[10] = ~value;
        ctx.vfpu_ctrl[11] = value + 0xB7E15162u;
        ctx.eat_vfpu_prefixes();
        break;
    }

    // vsbz: s.x's mantissa with the exponent of 1.0, so a value in [1, 2);
    // the sign goes. Zero, denormals and NaNs pass.
    case Extra::Vsbz: {
        float source[4]{};
        ctx.read_vfpu_vector_with_source_prefix(source, vs_of(word), length, 0u);
        const std::uint32_t s = bits(source[0]);
        const bool passes = std::isnan(source[0]) || (s & 0x7F800000u) == 0u;
        write_first_lane_result(ctx, word, source, passes ? source[0] : from_bits((127u << 23u) | (s & 0x007FFFFFu)));
        break;
    }

    // vlgb: s.x's unbiased exponent as a float; -infinity for zero and
    // denormals, s.x itself for infinities and NaNs.
    case Extra::Vlgb: {
        float source[4]{};
        ctx.read_vfpu_vector_with_source_prefix(source, vs_of(word), length, 0u);
        const std::uint32_t exponent = (bits(source[0]) >> 23u) & 0xFFu;
        const float result = exponent == 0xFFu ? source[0]
                           : exponent == 0u   ? -INFINITY
                                              : static_cast<float>(static_cast<int>(exponent) - 127);
        write_first_lane_result(ctx, word, source, result);
        break;
    }

    // vi2c: the top byte of each integer lane, packed into one word (lane 0 in
    // the low byte). vi2us: each lane clamped at zero and shifted down 15, two
    // per word. The unclamped vi2s and the unsigned vi2uc are the decoder's.
    case Extra::Vi2c:
    case Extra::Vi2us: {
        float source[4]{};
        ctx.read_vfpu_vector_with_source_prefix(source, vs_of(word), length, 0u);
        float result[4]{};
        std::uint32_t result_length = 1u;
        if (which == Extra::Vi2c) {
            std::uint32_t packed = 0u;
            for (std::uint32_t lane = 0u; lane < length; ++lane) packed |= (bits(source[lane]) >> 24u) << (lane * 8u);
            result[0] = from_bits(packed);
        } else {
            result_length = (length + 1u) / 2u;
            for (std::uint32_t lane = 0u; lane < result_length; ++lane) {
                const auto low = std::max(0, std::bit_cast<std::int32_t>(source[lane * 2u]));
                const auto high = lane * 2u + 1u < length ? std::max(0, std::bit_cast<std::int32_t>(source[lane * 2u + 1u])) : 0;
                result[lane] = from_bits((static_cast<std::uint32_t>(low) >> 15u) |
                                         ((static_cast<std::uint32_t>(high) >> 15u) << 16u));
            }
        }
        ctx.write_vfpu_vector_with_destination_prefix(result, vd_of(word), result_length);
        break;
    }

    // The sort steps, on a quad (x, y, z, w):
    //   vsrt1 (min(x,y), max(x,y), min(z,w), max(z,w))
    //   vsrt2 (min(x,w), min(y,z), max(y,z), max(x,w))
    //   vsrt3 (max(x,y), min(x,y), max(z,w), min(z,w))
    //   vsrt4 (max(x,w), max(y,z), min(y,z), min(x,w))
    // vbfy2, the butterfly across halves: (x+z, y+w, x-z, y-w).
    // The hardware forces the t swizzle (and for vbfy2 the s negation) inside
    // the prefixes; only the source prefix on s is applied here.
    case Extra::Vsrt1:
    case Extra::Vsrt2:
    case Extra::Vsrt3:
    case Extra::Vsrt4:
    case Extra::Vbfy2: {
        float s[4]{};
        ctx.read_vfpu_vector_with_source_prefix(s, vs_of(word), length, 0u);
        float d[4]{};
        switch (which) {
        case Extra::Vsrt1: d[0] = lane_min(s[0], s[1]); d[1] = lane_max(s[0], s[1]); d[2] = lane_min(s[2], s[3]); d[3] = lane_max(s[2], s[3]); break;
        case Extra::Vsrt2: d[0] = lane_min(s[0], s[3]); d[1] = lane_min(s[1], s[2]); d[2] = lane_max(s[1], s[2]); d[3] = lane_max(s[0], s[3]); break;
        case Extra::Vsrt3: d[0] = lane_max(s[0], s[1]); d[1] = lane_min(s[0], s[1]); d[2] = lane_max(s[2], s[3]); d[3] = lane_min(s[2], s[3]); break;
        case Extra::Vsrt4: d[0] = lane_max(s[0], s[3]); d[1] = lane_max(s[1], s[2]); d[2] = lane_min(s[1], s[2]); d[3] = lane_min(s[0], s[3]); break;
        default: d[0] = s[0] + s[2]; d[1] = s[1] + s[3]; d[2] = s[0] - s[2]; d[3] = s[1] - s[3]; break;
        }
        ctx.write_vfpu_vector_with_destination_prefix(d, vd_of(word), length);
        break;
    }

    // vmfvc/vmtvc: a VFPU control register (0-15, as mfv/mtv number them from
    // 128) to or from a vector scalar. The control register is bits 8-14 for
    // vmfvc and bits 0-6 for vmtvc. Neither consumes the prefixes.
    case Extra::Vmfvc: {
        const std::uint32_t control = (word >> 8u) & 0x7Fu;
        ctx.set_vfpu_scalar_bits(vd_of(word), control < 16u ? ctx.vfpu_ctrl[control] : 0u);
        break;
    }
    case Extra::Vmtvc: {
        const std::uint32_t control = word & 0x7Fu;
        if (control < 16u) ctx.set_vfpu_scalar_bits(128u + control, ctx.vfpu_scalar_bits(vs_of(word)));
        break;
    }

    // vt4444/vt5551: packed 8888 colours to 16 bits, two per word, as vt5650
    // does. 4444 keeps the top four bits of each channel; 5551 five of red,
    // green and blue and the top bit of alpha.
    case Extra::Vt4444:
    case Extra::Vt5551: {
        float source[4]{};
        ctx.read_vfpu_vector_with_source_prefix(source, vs_of(word), length, 0u);
        const std::uint32_t result_length = (length + 1u) / 2u;
        float result[4]{};
        for (std::uint32_t lane = 0u; lane < result_length; ++lane) {
            const std::uint32_t low = pack_colour(bits(source[lane * 2u]), which == Extra::Vt4444);
            const std::uint32_t high =
                lane * 2u + 1u < length ? pack_colour(bits(source[lane * 2u + 1u]), which == Extra::Vt4444) : 0u;
            result[lane] = from_bits(low | (high << 16u));
        }
        ctx.write_vfpu_vector_with_destination_prefix(result, vd_of(word), result_length);
        break;
    }

    case Extra::None:
    case Extra::Count:
        break;
    }
}

} // namespace psprecomp
