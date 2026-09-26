#pragma once

// Applying HLE extension modules (include/portablekit/hle_extension.hpp) on
// top of the HLE a program registers itself. See docs/HLE_EXTENSIONS.md.

#include "kernel/kernel.hpp"
#include "portablekit/hle_extension.hpp"
#include "psprecomp/elf32.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <span>
#include <string>
#include <vector>

namespace portablekit {

class HleRegistrar;

// One module linked into the program, as its portablekit_add_hle_extension()
// call declares it.
struct HleExtensionModule {
    const char *name;    // the C identifier its entry is named after
    const char *title;   // what the logs call it; the name when none was given
    const char *version; // may be empty
    const char *license; // the licence it states, e.g. "MIT"
    void (*entry)(hle_extension::Registry &);
};

// The modules this program was built with, in the order the build was given
// them. Defined by a source the build generates for each program
// (cmake/PortableKit.cmake); empty when it was given none.
[[nodiscard]] std::span<const HleExtensionModule> linked_hle_extensions();

// A function an extension module provides and the program now runs.
struct HleExtensionBinding {
    std::string library;
    std::uint32_t nid{};
    std::string name;   // as the module or the NID table names it; hex NID otherwise
    std::string module;
    bool overrides{};   // replaced the program's own implementation
    // A chained override: what it passes calls on to, the program's own
    // ("the built-in") or an earlier module's title; empty for any other.
    std::string wraps;
};

// A function an extension module added that the program does not run, and why.
struct HleExtensionSkip {
    std::string library;
    std::uint32_t nid{};
    std::string name;
    std::string module;
    std::string reason;
};

struct HleExtensionModuleSummary {
    std::string name;
    std::size_t functions{}; // active, overrides included
    std::size_t overrides{};
    std::size_t skipped{};
};

struct HleExtensionReport {
    bool disabled{};         // modules were linked and the off switch ignored them
    std::size_t linked{};    // modules linked into the program
    std::vector<HleExtensionModuleSummary> modules;
    std::vector<HleExtensionBinding> active;
    std::vector<HleExtensionSkip> skipped;

    [[nodiscard]] const HleExtensionBinding *find(const std::string &library, std::uint32_t nid) const;
};

// How the program's own HLE is asked and changed: whether it implements a
// function, and binding one (replacing whatever was bound before).
struct HleExtensionTarget {
    std::function<bool(const std::string &library, std::uint32_t nid)> implemented;
    std::function<void(const std::string &library, std::uint32_t nid, hle_extension::Handler handler)> bind;
    // The handler bound now, or an empty one; what a chained override wraps.
    std::function<hle_extension::Handler(const std::string &library, std::uint32_t nid)> current;
};

// Calls each module's entry and binds what it added, in this order of
// precedence:
//   - Mode::FillIn binds only a function the program does not implement;
//     otherwise the program's own stays and the function is reported skipped.
//   - Mode::Override binds whether or not the program implements it, and
//     counts as an override only where it does.
//   - A chained override (Registry::override_chained) wraps whatever is
//     bound, the program's own or an earlier module's, and always binds.
//   - Otherwise, between modules and within one, the first to add a
//     function keeps it; a later addition is reported skipped.
// Names the NID table does not know are added to it, so logs show them.
// Throws psprecomp::Error for a function without a handler or library.
[[nodiscard]] HleExtensionReport apply_hle_extensions(psprecomp::Runtime &runtime,
                                                      std::span<const HleExtensionModule> modules,
                                                      const HleExtensionTarget &target);

// One line per module: "HLE extension: <title> <version> (<licence>)", for
// the startup log and for a program's --version.
void print_hle_extension_modules(std::ostream &out, std::span<const HleExtensionModule> modules);
// The startup lines: one per module ("HLE extensions: 2 functions (1 overrides
// a built-in) from example"), then one per override and per skipped function.
void print_hle_extension_summary(std::ostream &out, const HleExtensionReport &report);
// For <prefix>_LIST_STUBS: the game's imports that extension modules serve,
// and which module serves each.
void print_hle_extension_imports(std::ostream &out, const HleExtensionReport &report,
                                 std::span<const psprecomp::PspImport> imports);

// The arguments of a call into the game as the kernel takes them, and how
// many there are. Throws psprecomp::Error past kGuestCallMaxArguments.
[[nodiscard]] std::pair<GuestCallArguments, std::uint32_t> guest_call_arguments(
    std::span<const std::uint32_t> arguments);

// The host's side (hle_extension_host.cpp): applies linked_hle_extensions() to
// `hle`, unless <prefix>_NO_HLE_EXTENSIONS is set, and prints the summary.
[[nodiscard]] HleExtensionReport install_linked_hle_extensions(psprecomp::Runtime &runtime, HleRegistrar &hle);

} // namespace portablekit
