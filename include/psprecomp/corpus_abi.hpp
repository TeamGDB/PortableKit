#pragma once

// Everything recompiled code sees of the runtime that runs it: the "corpus
// ABI". Generated units include this header and nothing else from the
// framework, reach the runtime only through CorpusRuntime (the base of
// psprecomp::Runtime) and the table of functions it points to, and so
// reference no symbol of the program that loads them.
//
// That makes a corpus independent of how the runtime is built. A library
// compiled on a player's machine keeps working with a newer build of the
// program as long as this header, AllegrexContext and the recompiler's output
// are unchanged, and it can be compiled by a different compiler than the
// program was (the types below are plain data and function pointers, laid out
// the same way by every x86_64 and arm64 compiler).
//
// Change this file with care: every change is a new ABI, and every corpus
// already compiled, by a port's build or on a player's machine, has to be
// compiled again. Bump kCorpusAbiVersion when the layout changes.

#include "psprecomp/allegrex_context.hpp"

#include <bit>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace psprecomp {

inline constexpr std::uint32_t kCorpusAbiVersion = 1u;

#if defined(_MSC_VER)
#define PSPRECOMP_CORPUS_FORCEINLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define PSPRECOMP_CORPUS_FORCEINLINE inline __attribute__((always_inline))
#else
#define PSPRECOMP_CORPUS_FORCEINLINE inline
#endif

class CorpusRuntime;
class AotFastView;

using CorpusFunction = void (*)(CorpusRuntime &, AllegrexContext &);
using CorpusEntryFunction = void (*)(CorpusRuntime &, AllegrexContext &, std::uint16_t, AotFastView &);

// Identifies the PSP execution context that entered a host import wrapper.
// A kernel HLE call may schedule a different thread while the wrapper is still
// on the native stack.  Checking only ctx.pc is insufficient because the new
// thread can legitimately be waiting at the same import stub.  The switch
// generation makes return-address normalization conditional on still owning
// the original PSP thread context.
struct RuntimeExecutionContextToken {
    std::int32_t thread_uid{-1};
    std::uint64_t switch_generation{};
};

// Guest memory accesses the inline fast path below does not handle: EDRAM,
// out-of-range and write-watched addresses. `memory` is the GuestMemory the
// view was made from.
struct AotSlowPaths {
    std::uint8_t (*load8)(const void *memory, std::uint32_t address);
    std::uint16_t (*load16)(const void *memory, std::uint32_t address);
    std::uint32_t (*load32)(const void *memory, std::uint32_t address);
    void (*store8)(void *memory, std::uint32_t address, std::uint8_t value);
    void (*store16)(void *memory, std::uint32_t address, std::uint16_t value);
    void (*store32)(void *memory, std::uint32_t address, std::uint32_t value);
};

// Cached AOT memory view. Generated units contain hundreds to thousands of
// guest loads/stores each. Calling the inline GuestMemory accessors still asks
// the optimizer to rediscover the RAM pointer, three limits and the immutable
// write-watch flag at every static site. Materialize those values once when a
// unit is entered, then keep them as ordinary locals the optimizer can retain
// in registers across the unit's basic blocks.
//
// The RAM never moves after construction and the write-watch flag is fixed
// by the constructor, so a view stays valid across nested AOT/HLE calls.
class AotFastView {
public:
    AotFastView() = default;
    AotFastView(void *owner, const AotSlowPaths *slow, std::uint8_t *ram_data, std::uint32_t limit8,
                std::uint32_t limit16, std::uint32_t limit32, bool write_watch) noexcept
        : owner_(owner), slow_(slow), ram_data_(ram_data), ram_limit8_(limit8), ram_limit16_(limit16),
          ram_limit32_(limit32), write_watch_enabled_(write_watch) {}

    [[nodiscard]] PSPRECOMP_CORPUS_FORCEINLINE std::uint8_t aot_load8(std::uint32_t address) const {
        const std::uint32_t offset = ram_offset_of_fast(address);
        if (offset <= ram_limit8_) return ram_data_[offset];
        return slow_->load8(owner_, address);
    }
    [[nodiscard]] PSPRECOMP_CORPUS_FORCEINLINE std::uint16_t aot_load16(std::uint32_t address) const {
        const std::uint32_t offset = ram_offset_of_fast(address);
        if (offset <= ram_limit16_) return read_le16(ram_data_ + offset);
        return slow_->load16(owner_, address);
    }
    [[nodiscard]] PSPRECOMP_CORPUS_FORCEINLINE std::uint32_t aot_load32(std::uint32_t address) const {
        const std::uint32_t offset = ram_offset_of_fast(address);
        if (offset <= ram_limit32_) return read_le32(ram_data_ + offset);
        return slow_->load32(owner_, address);
    }
    PSPRECOMP_CORPUS_FORCEINLINE void aot_store8(std::uint32_t address, std::uint8_t value) const {
        const std::uint32_t offset = ram_offset_of_fast(address);
#if defined(PSPRECOMP_AOT_ASSUME_NO_WRITE_WATCH)
        if (offset <= ram_limit8_) {
#else
        if (!write_watch_enabled_ && offset <= ram_limit8_) {
#endif
            ram_data_[offset] = value;
            return;
        }
        slow_->store8(owner_, address, value);
    }
    PSPRECOMP_CORPUS_FORCEINLINE void aot_store16(std::uint32_t address, std::uint16_t value) const {
        const std::uint32_t offset = ram_offset_of_fast(address);
#if defined(PSPRECOMP_AOT_ASSUME_NO_WRITE_WATCH)
        if (offset <= ram_limit16_) {
#else
        if (!write_watch_enabled_ && offset <= ram_limit16_) {
#endif
            write_le16(ram_data_ + offset, value);
            return;
        }
        slow_->store16(owner_, address, value);
    }
    PSPRECOMP_CORPUS_FORCEINLINE void aot_store32(std::uint32_t address, std::uint32_t value) const {
        const std::uint32_t offset = ram_offset_of_fast(address);
#if defined(PSPRECOMP_AOT_ASSUME_NO_WRITE_WATCH)
        if (offset <= ram_limit32_) {
#else
        if (!write_watch_enabled_ && offset <= ram_limit32_) {
#endif
            write_le32(ram_data_ + offset, value);
            return;
        }
        slow_->store32(owner_, address, value);
    }

    // Guest memory is little-endian, so on a little-endian host these are the
    // bytes in one access.
    [[nodiscard]] PSPRECOMP_CORPUS_FORCEINLINE static std::uint16_t read_le16(const std::uint8_t *source) noexcept {
        std::uint16_t value{};
        std::memcpy(&value, source, sizeof(value));
        if constexpr (std::endian::native == std::endian::big)
            value = static_cast<std::uint16_t>((value >> 8u) | (value << 8u));
        return value;
    }
    [[nodiscard]] PSPRECOMP_CORPUS_FORCEINLINE static std::uint32_t read_le32(const std::uint8_t *source) noexcept {
        std::uint32_t value{};
        std::memcpy(&value, source, sizeof(value));
        if constexpr (std::endian::native == std::endian::big)
            value = ((value >> 24u) & 0x000000FFu) | ((value >> 8u) & 0x0000FF00u) |
                    ((value << 8u) & 0x00FF0000u) | ((value << 24u) & 0xFF000000u);
        return value;
    }
    PSPRECOMP_CORPUS_FORCEINLINE static void write_le16(std::uint8_t *destination, std::uint16_t value) noexcept {
        if constexpr (std::endian::native == std::endian::big)
            value = static_cast<std::uint16_t>((value >> 8u) | (value << 8u));
        std::memcpy(destination, &value, sizeof(value));
    }
    PSPRECOMP_CORPUS_FORCEINLINE static void write_le32(std::uint8_t *destination, std::uint32_t value) noexcept {
        if constexpr (std::endian::native == std::endian::big)
            value = ((value >> 24u) & 0x000000FFu) | ((value >> 8u) & 0x0000FF00u) |
                    ((value << 8u) & 0x00FF0000u) | ((value << 24u) & 0xFF000000u);
        std::memcpy(destination, &value, sizeof(value));
    }

private:
    // Canonicalize and rebase in one step. An address below the RAM base wraps
    // to a value far above any RAM size, so one unsigned compare rejects it
    // along with every out-of-range access.
    [[nodiscard]] PSPRECOMP_CORPUS_FORCEINLINE static constexpr std::uint32_t ram_offset_of_fast(
        std::uint32_t address) noexcept {
        return (address & 0x1FFFFFFFu) - 0x08000000u;
    }
    void *owner_{};
    const AotSlowPaths *slow_{};
    std::uint8_t *ram_data_{};
    std::uint32_t ram_limit8_{};
    std::uint32_t ram_limit16_{};
    std::uint32_t ram_limit32_{};
    bool write_watch_enabled_{};
};

// What the whole process shares: diagnostics and the scheduler's cadence,
// configured by the host before guest code runs.
struct CorpusProcessState {
    // Diagnostics flip this once for the process and the direct chain falls
    // back to the fully instrumented lookup. A plain byte keeps the common
    // branch one predictable load.
    bool chain_observers_active{};
    // How many dispatches a thread runs before the scheduler's safe point;
    // 0 when no starvation hook is installed.
    std::uint64_t starvation_interval{};
};

// The runtime's out-of-line services, one table per process. Every entry is
// called with the CorpusRuntime the generated code was given.
struct CorpusHostApi {
    std::uint32_t abi_version;
    // Cross-unit chaining (Runtime::invoke_chained_call and friends).
    bool (*chained_call)(CorpusRuntime &, AllegrexContext &, AotFastView *shared_view);
    bool (*chained_unit)(CorpusRuntime &, AllegrexContext &, std::uint32_t unit_index, AotFastView *shared_view);
    bool (*starvation_boundary)(CorpusRuntime &, AllegrexContext &);
    // Stopping, imports, instructions the recompiler lowers to a call.
    void (*unsupported)(CorpusRuntime &, std::uint32_t pc, std::uint32_t instruction, const char *reason);
    void (*arithmetic_overflow)(CorpusRuntime &, std::uint32_t pc, std::uint32_t instruction);
    bool (*stopped)(const CorpusRuntime &);
    void (*invoke_import)(CorpusRuntime &, std::uint32_t slot, const char *library, std::uint32_t nid,
                          AllegrexContext &);
    RuntimeExecutionContextToken (*capture_context)();
    bool (*context_matches)(RuntimeExecutionContextToken token);
    void (*execute_extra_instruction)(CorpusRuntime &, AllegrexContext &, std::uint32_t pc, std::uint32_t word);
    // Unaligned word loads and stores.
    std::uint32_t (*load_word_left)(CorpusRuntime &, std::uint32_t address, std::uint32_t existing);
    std::uint32_t (*load_word_right)(CorpusRuntime &, std::uint32_t address, std::uint32_t existing);
    void (*store_word_left)(CorpusRuntime &, std::uint32_t address, std::uint32_t value);
    void (*store_word_right)(CorpusRuntime &, std::uint32_t address, std::uint32_t value);
    // Registration, called once when the corpus is installed.
    void (*register_function)(CorpusRuntime &, std::uint32_t address, CorpusFunction function, const char *name);
    void (*register_unit)(CorpusRuntime &, std::uint32_t unit_index, std::uint32_t unit_address,
                          std::uint32_t unit_span, CorpusFunction function, CorpusEntryFunction entry_function);
};

// The part of psprecomp::Runtime that recompiled code sees. Runtime derives
// from it; generated functions take a CorpusRuntime & and never learn more.
// Plain data only, so its layout is the same under every compiler.
class CorpusRuntime {
public:
    static constexpr std::uint32_t kGeneratedUnitFastCapacity = 512u;

    CorpusRuntime(const CorpusRuntime &) = delete;
    CorpusRuntime &operator=(const CorpusRuntime &) = delete;

    // Generated code writes rt.memory().aot_load32(...) and the like; to it,
    // the runtime's memory is this object. (Runtime::memory() hides this and
    // returns the GuestMemory, for the host.)
    [[nodiscard]] CorpusRuntime &memory() noexcept { return *this; }
    [[nodiscard]] PSPRECOMP_CORPUS_FORCEINLINE AotFastView aot_fast_view() const noexcept { return fast_view_; }
    [[nodiscard]] PSPRECOMP_CORPUS_FORCEINLINE std::uint8_t aot_load8(std::uint32_t address) const {
        return fast_view_.aot_load8(address);
    }
    [[nodiscard]] PSPRECOMP_CORPUS_FORCEINLINE std::uint16_t aot_load16(std::uint32_t address) const {
        return fast_view_.aot_load16(address);
    }
    [[nodiscard]] PSPRECOMP_CORPUS_FORCEINLINE std::uint32_t aot_load32(std::uint32_t address) const {
        return fast_view_.aot_load32(address);
    }
    PSPRECOMP_CORPUS_FORCEINLINE void aot_store8(std::uint32_t address, std::uint8_t value) const {
        fast_view_.aot_store8(address, value);
    }
    PSPRECOMP_CORPUS_FORCEINLINE void aot_store16(std::uint32_t address, std::uint16_t value) const {
        fast_view_.aot_store16(address, value);
    }
    PSPRECOMP_CORPUS_FORCEINLINE void aot_store32(std::uint32_t address, std::uint32_t value) const {
        fast_view_.aot_store32(address, value);
    }

    // Everything below forwards to the host, except the direct chain.
    [[nodiscard]] bool invoke_chained_call(AllegrexContext &ctx, AotFastView *shared_view = nullptr) {
        return api_->chained_call(*this, ctx, shared_view);
    }
    [[nodiscard]] bool invoke_chained_unit(AllegrexContext &ctx, std::uint32_t unit_index,
                                           AotFastView *shared_view = nullptr) {
        return api_->chained_unit(*this, ctx, unit_index, shared_view);
    }
    void unsupported(std::uint32_t pc, std::uint32_t instruction, const char *reason) {
        api_->unsupported(*this, pc, instruction, reason);
    }
    void arithmetic_overflow(std::uint32_t pc, std::uint32_t instruction) {
        api_->arithmetic_overflow(*this, pc, instruction);
    }
    [[nodiscard]] bool stopped() const { return api_->stopped(*this); }
    void invoke_import_cached(std::uint32_t slot, const char *library, std::uint32_t nid, AllegrexContext &ctx) {
        api_->invoke_import(*this, slot, library, nid, ctx);
    }
    [[nodiscard]] RuntimeExecutionContextToken capture_execution_context() const { return api_->capture_context(); }
    [[nodiscard]] bool execution_context_matches(RuntimeExecutionContextToken token) const {
        return api_->context_matches(token);
    }
    void execute_extra_instruction(AllegrexContext &ctx, std::uint32_t pc, std::uint32_t word) {
        api_->execute_extra_instruction(*this, ctx, pc, word);
    }
    [[nodiscard]] std::uint32_t aot_load_word_left(std::uint32_t address, std::uint32_t existing) {
        return api_->load_word_left(*this, address, existing);
    }
    [[nodiscard]] std::uint32_t aot_load_word_right(std::uint32_t address, std::uint32_t existing) {
        return api_->load_word_right(*this, address, existing);
    }
    void aot_store_word_left(std::uint32_t address, std::uint32_t value) {
        api_->store_word_left(*this, address, value);
    }
    void aot_store_word_right(std::uint32_t address, std::uint32_t value) {
        api_->store_word_right(*this, address, value);
    }
    void register_function(std::uint32_t address, CorpusFunction function, const char *name) {
        api_->register_function(*this, address, function, name);
    }
    void register_generated_unit(std::uint32_t unit_index, std::uint32_t unit_address, std::uint32_t unit_span,
                                 CorpusFunction function, CorpusEntryFunction entry_function = nullptr) {
        api_->register_unit(*this, unit_index, unit_address, unit_span, function, entry_function);
    }

    // Bounded cross-unit call chaining, for targets the recompiler knows.
    //
    // The hottest guest routines are five-instruction leaves in a different
    // generated unit than their caller, so a plain `jal` costs two full outer
    // dispatches: one to enter the leaf and one to return. This executes the
    // translated unit inline instead, leaving ctx.pc wherever the callee
    // stopped so the caller can resume locally only when it matches its own
    // return address. It stays safe because a generated unit never runs an
    // import inline, and a scheduler boundary that switches PSP threads raises
    // chain_context_invalidated_, which every live native caller checks. Depth
    // is bounded so guest recursion cannot exhaust the native stack.
    //
    // The target function, its unit and entry and its guest PC are template
    // constants, so the common path is a direct native call. If a host
    // replacement was later registered in that unit, registration poisoned it
    // and this falls back to the exact per-PC path.
    template <auto Function, std::uint32_t UnitIndex, std::uint16_t DirectEntryId = 0u,
              std::uint32_t DirectTargetPc = 0u>
    [[nodiscard]] PSPRECOMP_CORPUS_FORCEINLINE bool invoke_chained_direct(AllegrexContext &ctx,
                                                                         AotFastView *shared_view = nullptr) {
#if defined(PSPRECOMP_AOT_PRODUCTION_FASTPATHS)
        if (UnitIndex >= kGeneratedUnitFastCapacity || !generated_unit_layout_valid_) {
#else
        if (process_->chain_observers_active || UnitIndex >= kGeneratedUnitFastCapacity ||
            !generated_unit_layout_valid_) {
#endif
            if constexpr (DirectTargetPc != 0u) ctx.pc = DirectTargetPc;
            return invoke_chained_unit(ctx, UnitIndex, shared_view);
        }
        if (generated_unit_disabled_[UnitIndex] != 0u) {
            // A unit can be poisoned because it contains PSP import stubs
            // while other entries in the same bucket remain ordinary AOT. Use
            // the exact per-PC chain table for the fixed target.
            if constexpr (DirectTargetPc != 0u) {
                ctx.pc = DirectTargetPc;
                return invoke_chained_call(ctx, shared_view);
            } else {
                return false;
            }
        }
        if (chain_depth_ >= chain_depth_limit_) {
            // Materialize the target PC only on the rare depth-limit unwind so
            // the outer dispatcher enters the exact guest destination.
            if constexpr (DirectTargetPc != 0u) ctx.pc = DirectTargetPc;
            return false;
        }
        struct DepthGuard {
            std::uint32_t &depth;
            explicit DepthGuard(std::uint32_t &value) : depth(value) { ++depth; }
            ~DepthGuard() { --depth; }
        } guard(chain_depth_);
        if constexpr (DirectEntryId != 0u &&
                      std::is_invocable_v<decltype(Function), CorpusRuntime &, AllegrexContext &, std::uint16_t,
                                          AotFastView &>) {
            if (shared_view != nullptr) {
                Function(*this, ctx, DirectEntryId, *shared_view);
            } else {
                AotFastView local_view = fast_view_;
                Function(*this, ctx, DirectEntryId, local_view);
            }
        } else {
            Function(*this, ctx);
        }
#if !defined(PSPRECOMP_AOT_PRODUCTION_FASTPATHS)
        if (track_dispatch_counters_) {
            ++chained_dispatches_;
            ++dispatch_work_count_;
        }
#endif
        // A scheduler boundary in any descendant switched PSP ownership:
        // unwind every still-live native caller.
        if (chain_context_invalidated_) {
            if (process_->starvation_interval != 0u) ++dispatches_since_import_;
            return false;
        }
        const std::uint64_t starvation_interval = process_->starvation_interval;
        if (starvation_interval == 0u) return true;
        if (++dispatches_since_import_ < starvation_interval) return true;
        return api_->starvation_boundary(*this, ctx);
    }

protected:
    CorpusRuntime(const CorpusHostApi *api, const CorpusProcessState *process) noexcept
        : api_(api), process_(process) {}
    ~CorpusRuntime() = default;

    const CorpusHostApi *api_;
    const CorpusProcessState *process_;
    AotFastView fast_view_{};
    // Dense chain state. An overlapping host/import entry poisons its whole
    // unit for the fast path; calls then unwind to exact PC dispatch.
    std::uint8_t generated_unit_disabled_[kGeneratedUnitFastCapacity]{};
    bool generated_unit_layout_valid_{true};
    // Set only when a scheduler safe-point actually changes PSP execution
    // ownership while native AOT frames may still be nested. Cleared at the
    // beginning of each outer dispatch.
    bool chain_context_invalidated_{};
    // High-frequency dispatch counters stay cold unless asked for.
    bool track_dispatch_counters_{};
    std::uint32_t chain_depth_{};
    std::uint32_t chain_depth_limit_{};
    std::uint64_t dispatches_since_import_{};
    std::uint64_t chained_dispatches_{};
    std::uint64_t dispatch_work_count_{};
};

// The corpus's own entry point: registers every function it holds. A port
// links it into its executable; a program that loads corpora at run time (the
// desktop app, overlay libraries) calls it through the library.
void register_generated_functions(CorpusRuntime &runtime);

} // namespace psprecomp
