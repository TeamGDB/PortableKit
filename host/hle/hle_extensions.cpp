#include "hle_extensions.hpp"

#include "psprecomp/common.hpp"

#include <algorithm>
#include <map>
#include <ostream>
#include <set>
#include <utility>

namespace portablekit {
namespace {

std::string display_name(psprecomp::Runtime &runtime, const hle_extension::Function &function) {
    if (!function.name.empty()) return function.name;
    return runtime.nids().resolve(function.library, function.nid).value_or(psprecomp::hex32(function.nid));
}

} // namespace

const HleExtensionBinding *HleExtensionReport::find(const std::string &library, std::uint32_t nid) const {
    // The last binding is the outermost: the one the game reaches first.
    for (auto it = active.rbegin(); it != active.rend(); ++it)
        if (it->nid == nid && it->library == library) return &*it;
    return nullptr;
}

HleExtensionReport apply_hle_extensions(psprecomp::Runtime &runtime, std::span<const HleExtensionModule> modules,
                                        const HleExtensionTarget &target) {
    HleExtensionReport report;
    report.linked = modules.size();
    // Which module bound each function, so a later one does not replace it.
    std::map<std::pair<std::string, std::uint32_t>, std::string> taken;

    for (const HleExtensionModule &module : modules) {
        hle_extension::Registry registry;
        module.entry(registry);
        HleExtensionModuleSummary summary{.name = module.title};

        for (const hle_extension::Function &function : registry.functions()) {
            if (function.library.empty() || (!function.handler && !function.chained))
                throw psprecomp::Error(std::string("HLE extension ") + module.title + " added " +
                                       (function.library.empty() ? "a function without a library"
                                                                 : function.library + " " +
                                                                       psprecomp::hex32(function.nid) +
                                                                       " without a handler"));
            const std::string name = display_name(runtime, function);
            if (!function.name.empty() && !runtime.nids().resolve(function.library, function.nid))
                runtime.nids().add(function.library, function.nid, function.name);

            const auto key = std::make_pair(function.library, function.nid);
            const auto skip = [&](std::string reason) {
                report.skipped.push_back({function.library, function.nid, name, module.title, std::move(reason)});
                ++summary.skipped;
            };
            const bool implemented = target.implemented(function.library, function.nid);
            if (function.chained) {
                const auto earlier = taken.find(key);
                std::string wraps = earlier != taken.end() ? earlier->second
                                    : implemented          ? std::string("the built-in")
                                                           : std::string("nothing (a logging stub's behaviour)");
                hle_extension::Handler previous = target.current ? target.current(function.library, function.nid)
                                                                 : hle_extension::Handler{};
                if (!previous) {
                    const std::string label = function.library + "::" + name;
                    previous = [label](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                        hle_extension::log_once("previous:" + label,
                                                "[hle-extension] " + label + " passed on with nothing behind it");
                        hle_extension::finish(ctx, 0u);
                    };
                }
                target.bind(function.library, function.nid,
                            [chained = function.chained, previous = std::move(previous)](
                                psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { chained(rt, ctx, previous); });
                const bool overrides = implemented || earlier != taken.end();
                taken[key] = module.title;
                report.active.push_back({function.library, function.nid, name, module.title, overrides, std::move(wraps)});
                ++summary.functions;
                if (overrides) ++summary.overrides;
                continue;
            }
            if (const auto found = taken.find(key); found != taken.end()) {
                skip("already provided by " + found->second);
                continue;
            }
            if (implemented && function.mode == hle_extension::Mode::FillIn) {
                skip("implemented by the program; not marked as an override");
                continue;
            }
            const bool overrides = implemented;
            target.bind(function.library, function.nid, function.handler);
            taken.emplace(key, module.title);
            report.active.push_back({function.library, function.nid, name, module.title, overrides, {}});
            ++summary.functions;
            if (overrides) ++summary.overrides;
        }
        report.modules.push_back(std::move(summary));
    }
    return report;
}

std::pair<GuestCallArguments, std::uint32_t> guest_call_arguments(std::span<const std::uint32_t> arguments) {
    static_assert(hle_extension::kMaxGuestArguments == kGuestCallMaxArguments);
    if (arguments.size() > kGuestCallMaxArguments)
        throw psprecomp::Error("A call into the game takes at most " + std::to_string(kGuestCallMaxArguments) +
                               " arguments, not " + std::to_string(arguments.size()));
    GuestCallArguments packed{};
    std::copy(arguments.begin(), arguments.end(), packed.begin());
    return {packed, static_cast<std::uint32_t>(arguments.size())};
}

void print_hle_extension_modules(std::ostream &out, std::span<const HleExtensionModule> modules) {
    for (const HleExtensionModule &module : modules) {
        out << "HLE extension: " << module.title;
        if (module.version != nullptr && *module.version != '\0') out << " " << module.version;
        out << " (" << module.license << ")\n";
    }
}

void print_hle_extension_summary(std::ostream &out, const HleExtensionReport &report) {
    for (const HleExtensionModuleSummary &module : report.modules) {
        out << "HLE extensions: " << module.functions << (module.functions == 1u ? " function" : " functions")
            << " (" << module.overrides << " overriding built-ins";
        if (module.skipped != 0u) out << ", " << module.skipped << " not used";
        out << ") from " << module.name << "\n";
    }
    for (const HleExtensionBinding &binding : report.active) {
        if (!binding.wraps.empty())
            out << "[hle-extension] " << binding.library << "::" << binding.name << " is " << binding.module
                << "'s, passing what it does not handle to " << binding.wraps << "\n";
        else if (binding.overrides)
            out << "[hle-extension] " << binding.library << "::" << binding.name << " is " << binding.module
                << "'s, overriding the built-in\n";
    }
    for (const HleExtensionSkip &skip : report.skipped)
        out << "[hle-extension] " << skip.library << "::" << skip.name << " from " << skip.module
            << " not used: " << skip.reason << "\n";
}

void print_hle_extension_imports(std::ostream &out, const HleExtensionReport &report,
                                 std::span<const psprecomp::PspImport> imports) {
    if (report.active.empty()) return;
    std::set<std::string> lines;
    for (const psprecomp::PspImport &import : imports)
        if (const HleExtensionBinding *binding = report.find(import.library, import.nid))
            lines.insert(binding->library + "::" + binding->name + "  " + binding->module +
                         (!binding->wraps.empty() ? " (wraps " + binding->wraps + ")"
                          : binding->overrides ? std::string(" (overrides the built-in)")
                                               : std::string(" (fills in)")));
    out << "Imports served by HLE extensions: " << lines.size() << "\n";
    for (const std::string &line : lines) out << "    " << line << "\n";
}

} // namespace portablekit
