#pragma once

#include "system.hpp"

#include "psprecomp/runtime.hpp"

namespace portablekit {

// Loads the overlay libraries and installs the dispatch-miss hook that
// recognises the overlay currently in a slot and registers its recompiled
// corpus. The libraries are read from PORTABLEKIT_OVERLAY_DIR, or from overlays/
// next to the executable. With <prefix>_DUMP_OVERLAYS set, an unknown overlay is
// written out instead so it can be recompiled.
void install_overlay_support(psprecomp::Runtime &runtime);

// Drops the corpus of any slot whose contents no longer match it. The guest
// flushes the instruction cache right after loading an overlay, which is when
// this is called; the next jump into the slot then installs the right corpus.
void revalidate_overlays(psprecomp::Runtime &runtime);

// The guest loaded code: a slot whose image matched no corpus is searched
// again at the next call into it.
void forget_unmatched_overlays();

// Asked first when dispatch finds no function at an address, before the
// overlay corpora. A program that loads the game's main corpus while the game
// already runs (the desktop app, which starts under the interpreter and
// switches to compiled code when it is ready) registers it from here and
// returns true to have the dispatch retried.
using CodeMissHook = bool (*)(psprecomp::Runtime &, psprecomp::AllegrexContext &, std::uint32_t pc);
void set_code_miss_hook(CodeMissHook hook);

} // namespace portablekit
