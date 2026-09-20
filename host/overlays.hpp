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

} // namespace portablekit
