#include "profile.hpp"
#include "system.hpp"

#include "profile.hpp"

#include "hle/hle_common.hpp"
#include "kernel/kernel.hpp"
#include "overlays.hpp"

#include "psprecomp/common.hpp"

#include <algorithm>
#include <map>
#include <vector>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

namespace portablekit {
namespace {

// "No recompiled function registered at 0x0A002118" -> 0x0A002118
std::optional<std::uint32_t> parse_missing_function_address(const std::string &stop_reason) {
    constexpr std::string_view kPrefix = "No recompiled function registered at 0x";
    const auto position = stop_reason.find(kPrefix);
    if (position == std::string::npos) return std::nullopt;
    const std::string digits = stop_reason.substr(position + kPrefix.size());
    std::uint32_t address{};
    const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), address, 16);
    if (ec != std::errc{} || ptr == digits.data()) return std::nullopt;
    return address;
}



struct StubState {
    std::string label;
    std::uint64_t calls{};
};

void register_logging_stub(Runtime &runtime, const psprecomp::PspImport &import) {
    const std::string name = runtime.nids().resolve(import.library, import.nid).value_or(psprecomp::hex32(import.nid));
    auto state = std::make_shared<StubState>();
    state->label = import.library + "::" + name;
    runtime.register_hle(import.library, import.nid, [state](Runtime &, AllegrexContext &ctx) {
        if (state->calls++ == 0u) {
            std::cerr << "[hle-stub] " << state->label << " a0=" << psprecomp::hex32(ctx.gpr[4])
                      << " a1=" << psprecomp::hex32(ctx.gpr[5]) << " a2=" << psprecomp::hex32(ctx.gpr[6])
                      << " a3=" << psprecomp::hex32(ctx.gpr[7]) << " ra=" << psprecomp::hex32(ctx.gpr[31]) << "\n";
        }
        kernel().finish(ctx, 0u);
    });
}

std::uint32_t load_image_end(const psprecomp::Elf32Image &elf) {
    const std::uint32_t load_base = game().load_base;
    std::uint64_t end = 0u;
    for (std::size_t i = 0; i < elf.segments().size(); ++i) {
        const auto &segment = elf.segments()[i];
        if (segment.type != 1u) continue;
        end = std::max<std::uint64_t>(end, elf.segment_runtime_address(i, load_base) + segment.memory_size);
    }
    return static_cast<std::uint32_t>(end);
}

} // namespace

std::filesystem::path memory_stick_directory(const std::filesystem::path &game_dir) { return game_dir / "ms0"; }

void install_system(Runtime &runtime, const psprecomp::Elf32Image &elf, const ProfilePaths &paths) {
    const std::string_view boot_path = game().boot_path;
    const auto module = elf.find_module_info(runtime.memory(), game().load_base);
    if (!module) throw psprecomp::Error("PSP module info not found in executable");

    kernel().install(runtime, module->gp, load_image_end(elf));
    install_overlay_support(runtime);

    HleRegistrar hle(runtime);
    register_threadman(hle);
    register_sysmem(hle);
    register_io(hle, paths.disc_image, paths.memory_stick);
    register_system(hle);
    register_media(hle);
    register_atrac(hle);
    register_mpeg(hle);
    register_psmfplayer(hle);
    register_psmf(hle);
    register_font(hle);
    register_utility(hle, paths.memory_stick);
    register_adhoc(hle);
    if (const ExtraHleHook hook = active_register_extra_hle(); hook != nullptr) hook(hle);

    const bool strict = env_set("STRICT_HLE");
    std::size_t stubbed = 0u;
    // Every import this game makes that nothing implements, by library. It is
    // the list a port is worked through, and reading it off the executable
    // beats inferring it from the source: a call registered in a loop, or
    // under a name that does not appear as a literal, is easy to miss by eye.
    std::map<std::string, std::vector<std::string>> unimplemented;
    const auto imports = elf.scan_imports(runtime.memory(), *module);
    for (const auto &import : imports) {
        if (hle.bound(import.library, import.nid)) continue;
        unimplemented[import.library].push_back(
            runtime.nids().resolve(import.library, import.nid).value_or(psprecomp::hex32(import.nid)));
        if (strict) continue;
        register_logging_stub(runtime, import);
        ++stubbed;
    }
    if (env_set("LIST_STUBS")) {
        std::size_t total = 0u;
        for (const auto &[library, names] : unimplemented) total += names.size();
        std::cout << "Unimplemented imports: " << total << " of " << imports.size() << "\n";
        for (auto &[library, names] : unimplemented) {
            std::sort(names.begin(), names.end());
            std::cout << "  " << library << " (" << names.size() << ")\n";
            for (const std::string &name : names) std::cout << "    " << name << "\n";
        }
    }
    // Without a corpus there is nothing at the import stubs, so the
    // interpreter would run their own two instructions and carry on with
    // whatever was in v0. Bind them, which costs an AOT build nothing.
    std::size_t bound_stubs = 0u;
    for (const auto &import : imports)
        if (runtime.register_import_stub(import.stub_address, import.library, import.nid)) ++bound_stubs;
    if (bound_stubs != 0u) std::cout << "Import stubs: " << bound_stubs << " bound for interpretation\n";
    std::cout << "HLE imports: " << imports.size() << " total, " << hle.count() << " implemented, " << stubbed
              << " logging stubs" << (strict ? " (strict mode)" : "") << "\n";

    auto &ctx = runtime.cpu();
    auto &memory = runtime.memory();
    for (std::size_t i = 0; i < boot_path.size(); ++i)
        memory.store8(kBootArgumentAddress + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(boot_path[i]));
    memory.store8(kBootArgumentAddress + static_cast<std::uint32_t>(boot_path.size()), 0u);
    ctx.set_gpr(4, static_cast<std::uint32_t>(boot_path.size() + 1u));
    ctx.set_gpr(5, kBootArgumentAddress);
    if (const PatchImageHook hook = active_patch_loaded_image(); hook != nullptr) hook(runtime, elf);
    kernel().start_loader_thread(ctx, elf.runtime_entry(game().load_base), 0u);
}

} // namespace portablekit
