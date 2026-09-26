// HLE extension modules: a function filled in where the program has none, one
// overridden where the module asks, one left to the program where it does
// not, and the first module to add a function keeping it. Uses the example
// module in examples/hle_extension/ and a runtime with no kernel or game.

#include "hle/hle_extensions.hpp"

#include "psprecomp/runtime.hpp"

#include <cstdio>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <utility>

void portablekit_hle_extension_example(portablekit::hle_extension::Registry &registry);

namespace portablekit {
InterruptCall &test_last_queued_call(); // hle_extension_test_host.cpp
}

namespace {

namespace ext = portablekit::hle_extension;
using portablekit::HleExtensionModule;

int g_failures = 0;

void check(bool condition, const char *what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) ++g_failures;
}

constexpr std::uint32_t kLoadDefaultCert = 0x87797BDDu;   // sceHttp, filled in by the example
constexpr std::uint32_t kDcacheWritebackAll = 0x79D1C3FAu; // UtilsForUser, overridden by the example
constexpr std::uint32_t kBuiltinResult = 0xB0117u;
constexpr std::uint32_t kUntouched = 0xDEADu;

// A program's own HLE, as far as the extension layer can see it.
struct Program {
    psprecomp::Runtime runtime;
    std::set<std::pair<std::string, std::uint32_t>> bound;

    void builtin(const std::string &library, std::uint32_t nid, std::uint32_t result) {
        runtime.register_hle(library, nid, [result](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, result);
        });
        bound.insert({library, nid});
    }
    portablekit::HleExtensionTarget target() {
        return {
            .implemented = [this](const std::string &library, std::uint32_t nid) {
                return bound.contains({library, nid});
            },
            .bind = [this](const std::string &library, std::uint32_t nid, ext::Handler handler) {
                runtime.register_hle(library, nid, std::move(handler));
                bound.insert({library, nid});
            },
        };
    }
    // v0 after the game calls library::nid; kUntouched when nothing set it.
    std::uint32_t call(const std::string &library, std::uint32_t nid) {
        psprecomp::AllegrexContext ctx{};
        ctx.set_gpr(2, kUntouched);
        runtime.invoke_import(library, nid, ctx);
        return ctx.gpr[2];
    }
};

constexpr HleExtensionModule kExample{"example", "Example HLE extension", "1.0", "MIT",
                                      &portablekit_hle_extension_example};

// A second module that adds the example's fill-in again, and a fill-in of a
// function the program implements.
void second_module(ext::Registry &registry) {
    registry.add("sceHttp", kLoadDefaultCert, "sceHttpsLoadDefaultCert",
                 [](ext::Runtime &, ext::AllegrexContext &ctx) { ext::finish(ctx, 2u); });
    registry.add("UtilsForUser", 0x3EE30821u, "sceKernelDcacheWritebackRange",
                 [](ext::Runtime &, ext::AllegrexContext &ctx) { ext::finish(ctx, 2u); });
}
constexpr HleExtensionModule kSecond{"second", "second", "", "MIT", &second_module};

void broken_module(ext::Registry &registry) { registry.add("sceHttp", 1u, "nothing", nullptr); }
constexpr HleExtensionModule kBroken{"broken", "broken", "", "MIT", &broken_module};

} // namespace

int main() {
    // The example alone, over a program that implements the override's target.
    {
        Program program;
        program.builtin("UtilsForUser", kDcacheWritebackAll, kBuiltinResult);
        check(program.call("UtilsForUser", kDcacheWritebackAll) == kBuiltinResult, "the built-in answers first");
        const HleExtensionModule modules[] = {kExample};
        const auto report = portablekit::apply_hle_extensions(program.runtime, modules, program.target());
        check(program.call("sceHttp", kLoadDefaultCert) == 0u, "a function the program lacks is filled in");
        check(program.call("UtilsForUser", kDcacheWritebackAll) == 0u,
              "a function the module marks as an override replaces the built-in");
        check(report.active.size() == 2u && report.skipped.empty(), "both of the example's functions are active");
        const auto *fill = report.find("sceHttp", kLoadDefaultCert);
        const auto *over = report.find("UtilsForUser", kDcacheWritebackAll);
        check(fill != nullptr && !fill->overrides && fill->module == "Example HLE extension",
              "the fill-in is reported as the example's and not as an override");
        check(over != nullptr && over->overrides, "the override is reported as one");
        check(report.modules.size() == 1u && report.modules[0].functions == 2u && report.modules[0].overrides == 1u,
              "the module's summary counts two functions, one override");
        check(program.runtime.nids().resolve("sceHttp", kLoadDefaultCert) == "sceHttpsLoadDefaultCert",
              "the module's names reach the NID table");

        std::ostringstream summary;
        portablekit::print_hle_extension_summary(summary, report);
        check(summary.str().find("HLE extensions: 2 functions (1 overriding built-ins) from Example HLE extension") !=
                  std::string::npos,
              "the startup line names the module and counts its overrides");
        check(summary.str().find("UtilsForUser::sceKernelDcacheWritebackAll is Example HLE extension's, "
                                 "overriding the built-in") != std::string::npos,
              "each override is logged");

        const psprecomp::PspImport imports[] = {{"sceHttp", kLoadDefaultCert, 0u}, {"IoFileMgrForUser", 1u, 0u}};
        std::ostringstream listing;
        portablekit::print_hle_extension_imports(listing, report, imports);
        check(listing.str().find("Imports served by HLE extensions: 1") != std::string::npos &&
                  listing.str().find("sceHttp::sceHttpsLoadDefaultCert  Example HLE extension (fills in)") !=
                      std::string::npos,
              "the stub listing says which imports come from which module");

        std::ostringstream version;
        portablekit::print_hle_extension_modules(version, modules);
        check(version.str() == "HLE extension: Example HLE extension 1.0 (MIT)\n",
              "the module line gives its title, version and licence");
    }

    // A fill-in never replaces the program's own, and the first module to add
    // a function keeps it.
    {
        Program program;
        program.builtin("UtilsForUser", 0x3EE30821u, kBuiltinResult);
        const HleExtensionModule modules[] = {kExample, kSecond};
        const auto report = portablekit::apply_hle_extensions(program.runtime, modules, program.target());
        check(program.call("UtilsForUser", 0x3EE30821u) == kBuiltinResult,
              "a fill-in of a function the program implements leaves the built-in");
        check(program.call("sceHttp", kLoadDefaultCert) == 0u, "the first module to add a function keeps it");
        check(program.call("UtilsForUser", kDcacheWritebackAll) == 0u,
              "an override of a function the program lacks is bound");
        const auto *over = report.find("UtilsForUser", kDcacheWritebackAll);
        check(over != nullptr && !over->overrides, "and is not counted as an override");
        check(report.skipped.size() == 2u && report.modules.size() == 2u && report.modules[1].functions == 0u &&
                  report.modules[1].skipped == 2u,
              "both of the second module's functions are reported unused");
    }

    // Nothing linked: nothing changes.
    {
        Program program;
        program.builtin("UtilsForUser", kDcacheWritebackAll, kBuiltinResult);
        const auto report = portablekit::apply_hle_extensions(program.runtime, {}, program.target());
        check(report.active.empty() && report.modules.empty() &&
                  program.call("UtilsForUser", kDcacheWritebackAll) == kBuiltinResult,
              "with no modules the program's HLE is untouched");
    }

    // A function without a handler is refused at startup.
    {
        Program program;
        const HleExtensionModule modules[] = {kBroken};
        bool refused = false;
        try {
            (void)portablekit::apply_hle_extensions(program.runtime, modules, program.target());
        } catch (const std::exception &) {
            refused = true;
        }
        check(refused, "a function without a handler is refused");
    }

    // Calls into the game with more than four arguments: t0-t3 after a0-a3.
    {
        constexpr std::uint32_t kCallback = 0x08900040u;
        Program program;
        std::uint32_t seen[8]{};
        const auto calling = [&](ext::Registry &registry) {
            registry.add("TestLibrary", 1u, "callsBack", [&](ext::Runtime &, ext::AllegrexContext &ctx) {
                ext::call_guest(ctx, kCallback, {11u, 12u, 13u, 14u, 15u, 16u, 17u, 18u},
                                [&](ext::AllegrexContext &back, std::uint32_t) {
                                    for (unsigned i = 0; i < 8u; ++i) seen[i] = ext::arg(back, i);
                                    ext::finish(back, 0u);
                                });
            });
            registry.add("TestLibrary", 2u, "queuesFive", [](ext::Runtime &, ext::AllegrexContext &ctx) {
                ext::queue_guest_call(kCallback, {1u, 2u, 3u, 4u, 5u});
                ext::finish(ctx, 0u);
            });
        };
        static std::function<void(ext::Registry &)> entry;
        entry = calling;
        const HleExtensionModule modules[] = {
            {"calls", "calls", "", "MIT", [](ext::Registry &registry) { entry(registry); }}};
        (void)portablekit::apply_hle_extensions(program.runtime, modules, program.target());
        (void)program.call("TestLibrary", 1u);
        bool all = true;
        for (unsigned i = 0; i < 8u; ++i) all = all && seen[i] == 11u + i;
        check(all, "a call into the game receives all eight argument registers, a0-a3 and t0-t3");

        (void)program.call("TestLibrary", 2u);
        const portablekit::InterruptCall &queued = portablekit::test_last_queued_call();
        check(queued.function == kCallback && queued.argument_count == 5u && queued.arguments[4] == 5u &&
                  queued.arguments[5] == 0u,
              "a queued call keeps its five arguments and says how many");
        psprecomp::AllegrexContext ctx{};
        for (unsigned r = 8u; r < 12u; ++r) ctx.set_gpr(r, 0xAAu);
        portablekit::load_guest_call_arguments(ctx, queued.arguments, queued.argument_count);
        check(ext::arg(ctx, 4) == 5u && ctx.gpr[9] == 0xAAu && ctx.gpr[10] == 0xAAu && ctx.gpr[11] == 0xAAu,
              "five arguments set t0 and leave t1-t3 as they were");

        psprecomp::AllegrexContext four{};
        for (unsigned r = 8u; r < 12u; ++r) four.set_gpr(r, 0xAAu);
        portablekit::load_guest_call_arguments(four, {1u, 2u, 3u, 4u}, 4u);
        check(four.gpr[4] == 1u && four.gpr[7] == 4u && four.gpr[8] == 0xAAu && four.gpr[11] == 0xAAu,
              "a call of four arguments sets a0-a3 only, as calls always did");

        bool refused = false;
        const std::uint32_t nine[9]{};
        try {
            (void)portablekit::guest_call_arguments(nine);
        } catch (const std::exception &) {
            refused = true;
        }
        check(refused, "a ninth argument is refused");
    }

    if (g_failures != 0) std::printf("%d failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
