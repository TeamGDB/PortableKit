#pragma once

// Allegrex user-mode instructions that no game run on PortableKit has been
// seen to execute yet. The interpreter and the recompiled code both run them
// through execute_extra_instruction(), so the two cannot disagree, and the
// first time each one runs it says so on stderr: that is the moment to check
// its result against what the game expects and move it to the verified list
// in docs/INSTRUCTION_COVERAGE.md.
//
// The semantics were written from public descriptions of the PSP's VFPU and
// cross-checked against other emulators' documented behaviour, never taken
// from a console. Where a prefix is rewritten by the hardware for one of
// these (vdet, vsrt*, vbfy2), the simpler reading is used and says so.
//
// Generated code declares execute_extra_instruction() itself where it uses it
// rather than including this header, so adding an instruction here does not
// change every generated unit.

#include <cstdint>
#include <string_view>

namespace psprecomp {

class Runtime;
struct AllegrexContext;

// Runs one instruction for which is_extra_instruction() is true. `pc` is only
// for the first-use message.
void execute_extra_instruction(Runtime &rt, AllegrexContext &ctx, std::uint32_t pc, std::uint32_t word);

// The mnemonic of an extra instruction, or an empty view when `word` is not one.
[[nodiscard]] std::string_view extra_instruction_name(std::uint32_t word) noexcept;

[[nodiscard]] inline bool is_extra_instruction(std::uint32_t word) noexcept {
    return !extra_instruction_name(word).empty();
}

} // namespace psprecomp
