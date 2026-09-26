// ThreadManForUser and Kernel_Library: threads, time, semaphores, event flags,
// mutexes, callbacks, VTimers and interrupt masking.
#include "hle_common.hpp"

#include "kernel/fixed_pool.hpp"
#include "kernel/message_pipe.hpp"

#include "psprecomp/common.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <optional>

namespace portablekit {
namespace {

constexpr std::uint32_t kEventFlagWaitMulti = 0x200u;
constexpr std::uint32_t kMutexAttrRecursive = 0x200u;

std::int32_t as_signed(std::uint32_t value) { return static_cast<std::int32_t>(value); }
std::uint32_t as_unsigned(std::int32_t value) { return static_cast<std::uint32_t>(value); }

void register_threads(HleRegistrar &hle) {
    hle.add("ThreadManForUser", "sceKernelCreateThread", [](Runtime &rt, AllegrexContext &ctx) {
        const std::string name = read_cstring(rt.memory(), arg(ctx, 0), 32u);
        kernel().finish(ctx, as_unsigned(kernel().create_thread(name, arg(ctx, 1), arg(ctx, 2), arg(ctx, 3),
                                                                arg(ctx, 4), ctx.gpr[28])));
    });
    hle.add("ThreadManForUser", "sceKernelStartThread", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, as_unsigned(kernel().start_thread(ctx, as_signed(arg(ctx, 0)), arg(ctx, 1), arg(ctx, 2))));
    });
    hle.add("ThreadManForUser", "sceKernelExitThread", [](Runtime &, AllegrexContext &ctx) {
        kernel().exit_current_thread(ctx, as_signed(arg(ctx, 0)), false);
    });
    hle.add("ThreadManForUser", "sceKernelExitDeleteThread", [](Runtime &, AllegrexContext &ctx) {
        kernel().exit_current_thread(ctx, as_signed(arg(ctx, 0)), true);
    });
    hle.add("ThreadManForUser", "sceKernelDeleteThread", [](Runtime &, AllegrexContext &ctx) {
        const SceUID uid = as_signed(arg(ctx, 0));
        kernel().finish(ctx, uid == kernel().current_uid() ? error::kNotDormant
                                                           : as_unsigned(kernel().delete_thread(uid)));
    });
    hle.add("ThreadManForUser", "sceKernelTerminateThread", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, as_unsigned(kernel().terminate_thread(ctx, as_signed(arg(ctx, 0)), false)));
    });
    hle.add("ThreadManForUser", "sceKernelTerminateDeleteThread", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, as_unsigned(kernel().terminate_thread(ctx, as_signed(arg(ctx, 0)), true)));
    });
    const auto get_thread_id = [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, as_unsigned(kernel().current_uid()));
    };
    hle.add("ThreadManForUser", "sceKernelGetThreadId", get_thread_id);
    hle.add("Kernel_Library", "sceKernelGetThreadId", get_thread_id);
    hle.add("ThreadManForUser", "sceKernelGetThreadCurrentPriority", [](Runtime &, AllegrexContext &ctx) {
        const Thread *thread = kernel().current_thread();
        kernel().finish(ctx, thread != nullptr ? thread->priority : 0u);
    });
    hle.add("ThreadManForUser", "sceKernelChangeThreadPriority", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, as_unsigned(kernel().change_priority(ctx, as_signed(arg(ctx, 0)), arg(ctx, 1))));
    });
    hle.add("ThreadManForUser", "sceKernelChangeCurrentThreadAttr", [](Runtime &, AllegrexContext &ctx) {
        if (Thread *thread = kernel().current_thread())
            thread->attributes = (thread->attributes & ~arg(ctx, 0)) | arg(ctx, 1);
        kernel().finish(ctx, 0u);
    });
    hle.add("ThreadManForUser", "sceKernelGetThreadExitStatus", [](Runtime &, AllegrexContext &ctx) {
        const Thread *thread = kernel().find_thread(as_signed(arg(ctx, 0)));
        if (thread == nullptr) kernel().finish(ctx, error::kUnknownThid);
        else if (thread->status != ThreadStatus::Dormant) kernel().finish(ctx, error::kNotDormant);
        else kernel().finish(ctx, as_unsigned(thread->exit_status));
    });

    // Waits until the thread has ended and answers its exit status. A thread
    // that deletes itself as it ends is gone by the time the waiter looks, and
    // its status with it; that answers 0. The timeout is in microseconds at a1.
    const auto wait_thread_end = [](Runtime &rt, AllegrexContext &ctx) {
        const SceUID uid = as_signed(arg(ctx, 0));
        const Thread *thread = kernel().find_thread(uid);
        if (thread == nullptr) {
            kernel().finish(ctx, error::kUnknownThid);
            return;
        }
        if (uid == kernel().current_uid()) {
            kernel().finish(ctx, error::kIllegalThid);
            return;
        }
        const std::uint32_t timeout_address = arg(ctx, 1);
        std::optional<std::uint64_t> timeout;
        if (timeout_address != 0u) timeout = rt.memory().load32(timeout_address);
        auto &memory = rt.memory();
        kernel().wait_host(ctx, timeout, [uid, timeout_address, &memory](bool timed_out) -> std::optional<std::uint32_t> {
            const Thread *waited = kernel().find_thread(uid);
            if (waited == nullptr) return 0u;
            if (waited->status == ThreadStatus::Dormant || waited->status == ThreadStatus::Dead)
                return as_unsigned(waited->exit_status);
            if (timed_out) {
                if (timeout_address != 0u) memory.store32(timeout_address, 0u);
                return error::kWaitTimeout;
            }
            return std::nullopt;
        });
    };
    hle.add("ThreadManForUser", "sceKernelWaitThreadEnd", wait_thread_end);
    hle.add("ThreadManForUser", "sceKernelWaitThreadEndCB", wait_thread_end);

    // The *CB variants run the thread's notified callbacks before waiting.
    const auto sleep_cb = [](Runtime &, AllegrexContext &ctx) {
        Thread *thread = kernel().current_thread();
        (void)kernel().deliver_callbacks();
        if (thread != nullptr && thread->wakeup_count != 0u) {
            --thread->wakeup_count;
            kernel().finish(ctx, 0u);
            return;
        }
        WaitState wait{};
        wait.type = WaitType::Sleep;
        kernel().block(ctx, wait, 0u);
    };
    const auto sleep = [](Runtime &, AllegrexContext &ctx) {
        Thread *thread = kernel().current_thread();
        if (thread != nullptr && thread->wakeup_count != 0u) {
            --thread->wakeup_count;
            kernel().finish(ctx, 0u);
            return;
        }
        WaitState wait{};
        wait.type = WaitType::Sleep;
        kernel().block(ctx, wait, 0u);
    };
    hle.add("ThreadManForUser", "sceKernelSleepThread", sleep);
    hle.add("ThreadManForUser", "sceKernelSleepThreadCB", sleep_cb);
    // sceKernelReleaseWaitThread(uid): a waiting thread stops waiting, its
    // call answering SCE_KERNEL_ERROR_RELEASE_WAIT. Phantasy Star Portable 2
    // Infinity (NPJH50332) calls it.
    hle.add("ThreadManForUser", "sceKernelReleaseWaitThread", [](Runtime &, AllegrexContext &ctx) {
        constexpr std::uint32_t kReleaseWait = 0x800201AAu;
        constexpr std::uint32_t kNotWait = 0x800201A6u;
        Thread *thread = kernel().find_thread(as_signed(arg(ctx, 0)));
        if (thread == nullptr) {
            kernel().finish(ctx, error::kUnknownThid);
            return;
        }
        if (thread->status != ThreadStatus::Waiting) {
            kernel().finish(ctx, kNotWait);
            return;
        }
        if (trace_sync()) log_sync("ReleaseWaitThread " + std::to_string(thread->uid) + " " + thread->name);
        kernel().wake(*thread, kReleaseWait);
        kernel().finish(ctx, 0u);
    });
    hle.add("ThreadManForUser", "sceKernelWakeupThread", [](Runtime &, AllegrexContext &ctx) {
        Thread *thread = kernel().find_thread(as_signed(arg(ctx, 0)));
        if (thread == nullptr) {
            kernel().finish(ctx, error::kUnknownThid);
            return;
        }
        if (trace_sync()) log_sync("WakeupThread " + std::to_string(thread->uid) + " " + thread->name);
        if (thread->status == ThreadStatus::Waiting && thread->wait.type == WaitType::Sleep)
            kernel().wake(*thread, 0u);
        else
            ++thread->wakeup_count;
        kernel().finish(ctx, 0u);
    });

    const auto delay = [](Runtime &, AllegrexContext &ctx) { kernel().delay_current(ctx, arg(ctx, 0)); };
    hle.add("ThreadManForUser", "sceKernelDelayThread", delay);
    // Another thread only: it keeps its state and simply is not run until
    // sceKernelResumeThread.
    hle.add("ThreadManForUser", "sceKernelSuspendThread", [](Runtime &, AllegrexContext &ctx) {
        const SceUID uid = as_signed(arg(ctx, 0));
        Thread *thread = kernel().find_thread(uid);
        if (uid == 0 || uid == kernel().current_uid()) {
            kernel().finish(ctx, error::kIllegalThid);
            return;
        }
        if (thread == nullptr) {
            kernel().finish(ctx, error::kUnknownThid);
            return;
        }
        if (thread->status == ThreadStatus::Dormant || thread->status == ThreadStatus::Dead) {
            kernel().finish(ctx, error::kDormant);
            return;
        }
        if (thread->suspended) {
            kernel().finish(ctx, 0x800201A3u);  // SCE_KERNEL_ERROR_SUSPEND
            return;
        }
        thread->suspended = true;
        if (trace_sync()) log_sync("SuspendThread " + std::to_string(uid) + " " + thread->name);
        kernel().finish(ctx, 0u);
    });
    hle.add("ThreadManForUser", "sceKernelResumeThread", [](Runtime &, AllegrexContext &ctx) {
        Thread *thread = kernel().find_thread(as_signed(arg(ctx, 0)));
        if (thread == nullptr) {
            kernel().finish(ctx, error::kUnknownThid);
            return;
        }
        if (!thread->suspended) {
            kernel().finish(ctx, 0x800201A5u);  // SCE_KERNEL_ERROR_NOT_SUSPEND
            return;
        }
        thread->suspended = false;
        if (trace_sync()) log_sync("ResumeThread " + std::to_string(thread->uid) + " " + thread->name);
        kernel().finish(ctx, 0u);  // which runs it now if it outranks this thread
    });
    // sceKernelReferThreadStatus(uid, SceKernelThreadInfo *info), uid 0 the
    // calling thread. The info's layout is pspthreadman.h's: size, name[32],
    // attr, status, entry, stack, stackSize, gpReg, initPriority,
    // currentPriority, waitType, waitId, wakeupCount, exitStatus, runClocks
    // (8 bytes), three counters; written up to the size the game gives.
    // God of War (UCES00842) polls it with DelayThread until a loader
    // thread is done.
    hle.add("ThreadManForUser", "sceKernelReferThreadStatus", [](Runtime &rt, AllegrexContext &ctx) {
        const SceUID uid = as_signed(arg(ctx, 0));
        const Thread *thread = kernel().find_thread(uid == 0 ? kernel().current_uid() : uid);
        if (thread == nullptr) {
            kernel().finish(ctx, error::kUnknownThid);
            return;
        }
        auto &memory = rt.memory();
        const std::uint32_t info = arg(ctx, 1);
        const std::uint32_t size = info != 0u ? memory.load32(info) : 0u;
        std::uint32_t status = 0u;
        switch (thread->status) {
        case ThreadStatus::Running: status = 1u; break;
        case ThreadStatus::Ready: status = 2u; break;
        case ThreadStatus::Waiting: status = 4u; break;
        case ThreadStatus::Dormant: status = 16u; break;
        case ThreadStatus::Dead: status = 32u; break;
        }
        if (thread->uid == kernel().current_uid()) status = 1u;
        if (thread->suspended) status |= 8u;
        std::uint32_t wait_type = 0u;
        if (thread->status == ThreadStatus::Waiting) {
            switch (thread->wait.type) {
            case WaitType::Sleep: wait_type = 1u; break;
            case WaitType::Delay: wait_type = 2u; break;
            case WaitType::Semaphore: wait_type = 3u; break;
            case WaitType::EventFlag: wait_type = 4u; break;
            case WaitType::Mailbox: wait_type = 5u; break;
            case WaitType::ThreadEnd: wait_type = 9u; break;
            case WaitType::Mutex: wait_type = 12u; break;
            default: wait_type = 0u; break;
            }
        }
        std::array<std::uint8_t, 0x68> bytes{};
        const auto put = [&bytes](std::size_t at, std::uint32_t value) {
            for (std::size_t i = 0; i < 4u; ++i) bytes[at + i] = static_cast<std::uint8_t>(value >> (8u * i));
        };
        put(0u, size);
        for (std::size_t i = 0; i < 31u && i < thread->name.size(); ++i) bytes[4u + i] = static_cast<std::uint8_t>(thread->name[i]);
        put(36u, thread->attributes);
        put(40u, status);
        put(44u, thread->entry);
        put(48u, thread->stack_bottom);
        put(52u, thread->stack_size);
        put(56u, thread->gp);
        put(60u, thread->initial_priority);
        put(64u, thread->priority);
        put(68u, wait_type);
        put(72u, thread->status == ThreadStatus::Waiting ? as_unsigned(thread->wait.object) : 0u);
        put(76u, thread->wakeup_count);
        put(80u, as_unsigned(thread->exit_status));
        for (std::uint32_t i = 0; i < std::min<std::uint32_t>(size, static_cast<std::uint32_t>(bytes.size())); ++i)
            memory.store8(info + i, bytes[i]);
        kernel().finish(ctx, 0u);
    });
    // Runs the thread's notified callbacks now: 1 when there were any.
    hle.add("ThreadManForUser", "sceKernelCheckCallback", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, kernel().deliver_callbacks() ? 1u : 0u);
    });
    hle.add("ThreadManForUser", "sceKernelDelayThreadCB", [](Runtime &, AllegrexContext &ctx) {
        (void)kernel().deliver_callbacks();
        kernel().delay_current(ctx, arg(ctx, 0));
    });

    hle.add("ThreadManForUser", "sceKernelSuspendDispatchThread", [](Runtime &, AllegrexContext &ctx) {
        const bool was_enabled = kernel().dispatch_enabled();
        kernel().set_dispatch_enabled(false);
        kernel().finish(ctx, was_enabled ? 1u : 0u);
    });
    hle.add("ThreadManForUser", "sceKernelResumeDispatchThread", [](Runtime &, AllegrexContext &ctx) {
        kernel().set_dispatch_enabled(arg(ctx, 0) != 0u);
        kernel().finish(ctx, 0u);
    });
}

void register_time(HleRegistrar &hle) {
    hle.add("ThreadManForUser", "sceKernelGetSystemTime", [](Runtime &rt, AllegrexContext &ctx) {
        store64(rt.memory(), arg(ctx, 0), kernel().now_us());
        kernel().finish(ctx, 0u);
    });
    hle.add("ThreadManForUser", "sceKernelGetSystemTimeWide", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish64(ctx, kernel().now_us());
    });
    hle.add("ThreadManForUser", "sceKernelGetSystemTimeLow", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, static_cast<std::uint32_t>(kernel().now_us()));
    });
    // (SceKernelSysClock *clock, seconds *, microseconds *): the clock in
    // memory, split.
    hle.add("ThreadManForUser", "sceKernelSysClock2USec", [](Runtime &rt, AllegrexContext &ctx) {
        auto &memory = rt.memory();
        const std::uint32_t clock_address = arg(ctx, 0);
        const std::uint64_t clock = static_cast<std::uint64_t>(memory.load32(clock_address)) |
                                    (static_cast<std::uint64_t>(memory.load32(clock_address + 4u)) << 32u);
        if (arg(ctx, 1) != 0u) memory.store32(arg(ctx, 1), static_cast<std::uint32_t>(clock / 1'000'000u));
        if (arg(ctx, 2) != 0u) memory.store32(arg(ctx, 2), static_cast<std::uint32_t>(clock % 1'000'000u));
        kernel().finish(ctx, 0u);
    });
    // The system clock counts microseconds, so the conversion is the value.
    hle.add("ThreadManForUser", "sceKernelUSec2SysClockWide", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish64(ctx, arg(ctx, 0));
    });
    hle.add("ThreadManForUser", "sceKernelSysClock2USecWide", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint64_t clock = arg64(ctx, 0);
        if (arg(ctx, 2) != 0u) rt.memory().store32(arg(ctx, 2), static_cast<std::uint32_t>(clock / 1'000'000u));
        if (arg(ctx, 3) != 0u) rt.memory().store32(arg(ctx, 3), static_cast<std::uint32_t>(clock % 1'000'000u));
        kernel().finish(ctx, 0u);
    });
}

void register_semaphores(HleRegistrar &hle) {
    hle.add("ThreadManForUser", "sceKernelCreateSema", [](Runtime &rt, AllegrexContext &ctx) {
        const auto initial = as_signed(arg(ctx, 2));
        const auto maximum = as_signed(arg(ctx, 3));
        if (maximum <= 0 || initial < 0 || initial > maximum) {
            kernel().finish(ctx, error::kIllegalCount);
            return;
        }
        const SceUID uid = kernel().allocate_uid();
        kernel().semaphores[uid] = Semaphore{read_cstring(rt.memory(), arg(ctx, 0), 32u), arg(ctx, 1), initial, maximum, {}};
        kernel().finish(ctx, as_unsigned(uid));
    });
    hle.add("ThreadManForUser", "sceKernelDeleteSema", [](Runtime &, AllegrexContext &ctx) {
        auto found = kernel().semaphores.find(as_signed(arg(ctx, 0)));
        if (found == kernel().semaphores.end()) {
            kernel().finish(ctx, error::kUnknownSemid);
            return;
        }
        kernel().cancel_waiters(found->second.waiters, error::kWaitDelete);
        kernel().semaphores.erase(found);
        kernel().finish(ctx, 0u);
    });
    hle.add("ThreadManForUser", "sceKernelSignalSema", [](Runtime &, AllegrexContext &ctx) {
        const SceUID uid = as_signed(arg(ctx, 0));
        auto found = kernel().semaphores.find(uid);
        if (found == kernel().semaphores.end()) {
            kernel().finish(ctx, error::kUnknownSemid);
            return;
        }
        const auto signal = as_signed(arg(ctx, 1));
        if (found->second.count + signal - static_cast<std::int32_t>(found->second.waiters.size()) > found->second.max_count) {
            kernel().finish(ctx, error::kSemaOverflow);
            return;
        }
        found->second.count += signal;
        kernel().release_semaphore_waiters(uid);
        kernel().finish(ctx, 0u);
    });
    const auto wait_sema = [](Runtime &, AllegrexContext &ctx) {
        const SceUID uid = as_signed(arg(ctx, 0));
        auto found = kernel().semaphores.find(uid);
        const auto wanted = as_signed(arg(ctx, 1));
        if (found == kernel().semaphores.end()) {
            kernel().finish(ctx, error::kUnknownSemid);
            return;
        }
        if (wanted <= 0 || wanted > found->second.max_count) {
            kernel().finish(ctx, error::kIllegalCount);
            return;
        }
        Semaphore &sema = found->second;
        if (sema.waiters.empty() && sema.count >= wanted) {
            sema.count -= wanted;
            kernel().finish(ctx, 0u);
            return;
        }
        sema.waiters.push_back(kernel().current_uid());
        WaitState wait{};
        wait.type = WaitType::Semaphore;
        wait.object = uid;
        wait.value = static_cast<std::uint32_t>(wanted);
        wait.timeout_address = arg(ctx, 2);
        kernel().block(ctx, wait, 0u);
    };
    hle.add("ThreadManForUser", "sceKernelWaitSema", wait_sema);
    // As the other *CB waits: the thread's notified callbacks run first.
    hle.add("ThreadManForUser", "sceKernelWaitSemaCB", [wait_sema](Runtime &rt, AllegrexContext &ctx) {
        (void)kernel().deliver_callbacks();
        wait_sema(rt, ctx);
    });
    hle.add("ThreadManForUser", "sceKernelPollSema", [](Runtime &, AllegrexContext &ctx) {
        auto found = kernel().semaphores.find(as_signed(arg(ctx, 0)));
        const auto wanted = as_signed(arg(ctx, 1));
        if (found == kernel().semaphores.end()) kernel().finish(ctx, error::kUnknownSemid);
        else if (wanted <= 0) kernel().finish(ctx, error::kIllegalCount);
        else if (found->second.waiters.empty() && found->second.count >= wanted) {
            found->second.count -= wanted;
            kernel().finish(ctx, 0u);
        } else {
            kernel().finish(ctx, error::kSemaZero);
        }
    });
}

void register_event_flags(HleRegistrar &hle) {
    hle.add("ThreadManForUser", "sceKernelCreateEventFlag", [](Runtime &rt, AllegrexContext &ctx) {
        const SceUID uid = kernel().allocate_uid();
        kernel().event_flags[uid] = EventFlag{read_cstring(rt.memory(), arg(ctx, 0), 32u), arg(ctx, 1), arg(ctx, 2), {}, arg(ctx, 2)};
        if (trace_sync()) log_sync("CreateEventFlag " + std::to_string(uid) + " " + kernel().event_flags[uid].name);
        kernel().finish(ctx, as_unsigned(uid));
    });
    // sceKernelReferEventFlagStatus(uid, SceKernelEventFlagInfo *info): size,
    // name[32], attr, initPattern, currentPattern, numWaitThreads, as in
    // pspthreadman.h; written up to the size the game gives. God of War
    // (UCES00842) and Patapon (UCES00995) read the current pattern.
    hle.add("ThreadManForUser", "sceKernelReferEventFlagStatus", [](Runtime &rt, AllegrexContext &ctx) {
        const auto found = kernel().event_flags.find(as_signed(arg(ctx, 0)));
        if (found == kernel().event_flags.end()) {
            kernel().finish(ctx, error::kUnknownEvfid);
            return;
        }
        const EventFlag &flag = found->second;
        auto &memory = rt.memory();
        const std::uint32_t info = arg(ctx, 1);
        const std::uint32_t size = info != 0u ? memory.load32(info) : 0u;
        std::array<std::uint8_t, 52> bytes{};
        const auto put = [&bytes](std::size_t at, std::uint32_t value) {
            for (std::size_t i = 0; i < 4u; ++i) bytes[at + i] = static_cast<std::uint8_t>(value >> (8u * i));
        };
        put(0u, size);
        for (std::size_t i = 0; i < 31u && i < flag.name.size(); ++i) bytes[4u + i] = static_cast<std::uint8_t>(flag.name[i]);
        put(36u, flag.attributes);
        put(40u, flag.initial_pattern);
        put(44u, flag.pattern);
        put(48u, static_cast<std::uint32_t>(flag.waiters.size()));
        for (std::uint32_t i = 0; i < std::min<std::uint32_t>(size, static_cast<std::uint32_t>(bytes.size())); ++i)
            memory.store8(info + i, bytes[i]);
        kernel().finish(ctx, 0u);
    });
    hle.add("ThreadManForUser", "sceKernelDeleteEventFlag", [](Runtime &, AllegrexContext &ctx) {
        auto found = kernel().event_flags.find(as_signed(arg(ctx, 0)));
        if (found == kernel().event_flags.end()) {
            kernel().finish(ctx, error::kUnknownEvfid);
            return;
        }
        kernel().cancel_waiters(found->second.waiters, error::kWaitDelete);
        kernel().event_flags.erase(found);
        kernel().finish(ctx, 0u);
    });
    hle.add("ThreadManForUser", "sceKernelSetEventFlag", [](Runtime &, AllegrexContext &ctx) {
        const SceUID uid = as_signed(arg(ctx, 0));
        auto found = kernel().event_flags.find(uid);
        if (found == kernel().event_flags.end()) {
            kernel().finish(ctx, error::kUnknownEvfid);
            return;
        }
        found->second.pattern |= arg(ctx, 1);
        if (trace_sync())
            log_sync("SetEventFlag " + std::to_string(uid) + " " + found->second.name + " bits=" +
                     psprecomp::hex32(arg(ctx, 1)) + " -> " + psprecomp::hex32(found->second.pattern));
        kernel().release_event_flag_waiters(uid);
        kernel().finish(ctx, 0u);
    });
    hle.add("ThreadManForUser", "sceKernelClearEventFlag", [](Runtime &, AllegrexContext &ctx) {
        auto found = kernel().event_flags.find(as_signed(arg(ctx, 0)));
        if (found == kernel().event_flags.end()) {
            kernel().finish(ctx, error::kUnknownEvfid);
            return;
        }
        found->second.pattern &= arg(ctx, 1);
        kernel().finish(ctx, 0u);
    });

    // Shared validation and immediate-match logic for Wait/Poll. Returns true
    // when the call was completed without blocking.
    const auto try_match = [](Runtime &rt, AllegrexContext &ctx, bool poll) -> bool {
        const SceUID uid = as_signed(arg(ctx, 0));
        const std::uint32_t bits = arg(ctx, 1);
        const std::uint32_t mode = arg(ctx, 2);
        const std::uint32_t out = arg(ctx, 3);
        auto found = kernel().event_flags.find(uid);
        if (found == kernel().event_flags.end()) {
            kernel().finish(ctx, error::kUnknownEvfid);
            return true;
        }
        if ((mode & ~0x31u) != 0u || (mode & 0x30u) == 0x30u) {
            kernel().finish(ctx, error::kIllegalMode);
            return true;
        }
        if (bits == 0u) {
            kernel().finish(ctx, error::kEvfIllegalPattern);
            return true;
        }
        EventFlag &flag = found->second;
        if (Kernel::event_flag_matches(flag.pattern, bits, mode)) {
            if (out != 0u) rt.memory().store32(out, flag.pattern);
            if ((mode & 0x10u) != 0u) flag.pattern = 0u;
            else if ((mode & 0x20u) != 0u) flag.pattern &= ~bits;
            kernel().finish(ctx, 0u);
            return true;
        }
        if (poll) {
            if (out != 0u) rt.memory().store32(out, flag.pattern);
            // Once per flag and bits: a game polling in a loop would
            // otherwise fill the log.
            if (trace_sync())
                log_once("poll-evf:" + std::to_string(uid) + ":" + psprecomp::hex32(bits),
                         "[sync] PollEventFlag " + std::to_string(uid) + " " + flag.name + " bits=" +
                             psprecomp::hex32(bits) + " pattern=" + psprecomp::hex32(flag.pattern) + " (not met)");
            kernel().finish(ctx, error::kEvfCond);
            return true;
        }
        if ((flag.attributes & kEventFlagWaitMulti) == 0u && !flag.waiters.empty()) {
            kernel().finish(ctx, error::kEvfMulti);
            return true;
        }
        return false;
    };
    hle.add("ThreadManForUser", "sceKernelPollEventFlag", [try_match](Runtime &rt, AllegrexContext &ctx) {
        (void)try_match(rt, ctx, true);
    });
    const auto wait_event_flag = [try_match](Runtime &rt, AllegrexContext &ctx) {
        if (try_match(rt, ctx, false)) return;
        const SceUID uid = as_signed(arg(ctx, 0));
        if (trace_sync())
            log_sync("WaitEventFlag " + std::to_string(uid) + " " + kernel().event_flags[uid].name + " bits=" +
                     psprecomp::hex32(arg(ctx, 1)) + " pattern=" + psprecomp::hex32(kernel().event_flags[uid].pattern));
        kernel().event_flags[uid].waiters.push_back(kernel().current_uid());
        WaitState wait{};
        wait.type = WaitType::EventFlag;
        wait.object = uid;
        wait.value = arg(ctx, 1);
        wait.mode = arg(ctx, 2);
        wait.out_address = arg(ctx, 3);
        wait.timeout_address = arg(ctx, 4);
        kernel().block(ctx, wait, 0u);
    };
    hle.add("ThreadManForUser", "sceKernelWaitEventFlag", wait_event_flag);
    hle.add("ThreadManForUser", "sceKernelWaitEventFlagCB", [wait_event_flag](Runtime &rt, AllegrexContext &ctx) {
        (void)kernel().deliver_callbacks();
        wait_event_flag(rt, ctx);
    });
}

void register_mutexes(HleRegistrar &hle) {
    hle.add("ThreadManForUser", "sceKernelCreateMutex", [](Runtime &rt, AllegrexContext &ctx) {
        const SceUID uid = kernel().allocate_uid();
        Mutex mutex{read_cstring(rt.memory(), arg(ctx, 0), 32u), arg(ctx, 1), 0, as_signed(arg(ctx, 2)), {}};
        if (mutex.lock_count > 0) mutex.owner = kernel().current_uid();
        kernel().mutexes[uid] = std::move(mutex);
        kernel().finish(ctx, as_unsigned(uid));
    });
    hle.add("ThreadManForUser", "sceKernelDeleteMutex", [](Runtime &, AllegrexContext &ctx) {
        auto found = kernel().mutexes.find(as_signed(arg(ctx, 0)));
        if (found == kernel().mutexes.end()) {
            kernel().finish(ctx, error::kMutexNotFound);
            return;
        }
        kernel().cancel_waiters(found->second.waiters, error::kWaitDelete);
        kernel().mutexes.erase(found);
        kernel().finish(ctx, 0u);
    });
    hle.add("ThreadManForUser", "sceKernelLockMutex", [](Runtime &, AllegrexContext &ctx) {
        const SceUID uid = as_signed(arg(ctx, 0));
        const auto count = as_signed(arg(ctx, 1));
        auto found = kernel().mutexes.find(uid);
        if (found == kernel().mutexes.end()) {
            kernel().finish(ctx, error::kMutexNotFound);
            return;
        }
        if (count <= 0) {
            kernel().finish(ctx, error::kIllegalCount);
            return;
        }
        Mutex &mutex = found->second;
        if (mutex.lock_count == 0) {
            mutex.owner = kernel().current_uid();
            mutex.lock_count = count;
            kernel().finish(ctx, 0u);
            return;
        }
        if (mutex.owner == kernel().current_uid()) {
            if ((mutex.attributes & kMutexAttrRecursive) == 0u) {
                kernel().finish(ctx, error::kMutexRecursiveNotAllowed);
                return;
            }
            mutex.lock_count += count;
            kernel().finish(ctx, 0u);
            return;
        }
        mutex.waiters.push_back(kernel().current_uid());
        WaitState wait{};
        wait.type = WaitType::Mutex;
        wait.object = uid;
        wait.value = static_cast<std::uint32_t>(count);
        wait.timeout_address = arg(ctx, 2);
        kernel().block(ctx, wait, 0u);
    });
    // The same as sceKernelLockMutex, but a mutex another thread holds is
    // SCE_KERNEL_ERROR_MUTEX_LOCKED instead of a wait.
    hle.add("ThreadManForUser", "sceKernelTryLockMutex", [](Runtime &, AllegrexContext &ctx) {
        const SceUID uid = as_signed(arg(ctx, 0));
        const auto count = as_signed(arg(ctx, 1));
        auto found = kernel().mutexes.find(uid);
        if (found == kernel().mutexes.end()) {
            kernel().finish(ctx, error::kMutexNotFound);
            return;
        }
        if (count <= 0) {
            kernel().finish(ctx, error::kIllegalCount);
            return;
        }
        Mutex &mutex = found->second;
        if (mutex.lock_count == 0) {
            mutex.owner = kernel().current_uid();
            mutex.lock_count = count;
            kernel().finish(ctx, 0u);
        } else if (mutex.owner == kernel().current_uid()) {
            if ((mutex.attributes & kMutexAttrRecursive) == 0u) {
                kernel().finish(ctx, error::kMutexRecursiveNotAllowed);
                return;
            }
            mutex.lock_count += count;
            kernel().finish(ctx, 0u);
        } else {
            kernel().finish(ctx, error::kMutexLocked);
        }
    });
    hle.add("ThreadManForUser", "sceKernelUnlockMutex", [](Runtime &, AllegrexContext &ctx) {
        const SceUID uid = as_signed(arg(ctx, 0));
        const auto count = as_signed(arg(ctx, 1));
        auto found = kernel().mutexes.find(uid);
        if (found == kernel().mutexes.end()) {
            kernel().finish(ctx, error::kMutexNotFound);
            return;
        }
        Mutex &mutex = found->second;
        if (count <= 0) kernel().finish(ctx, error::kIllegalCount);
        else if (mutex.lock_count == 0 || mutex.owner != kernel().current_uid()) kernel().finish(ctx, error::kMutexUnlocked);
        else if (count > mutex.lock_count) kernel().finish(ctx, error::kMutexUnlockUnderflow);
        else {
            mutex.lock_count -= count;
            if (mutex.lock_count == 0) {
                mutex.owner = 0;
                kernel().release_mutex_waiters(uid);
            }
            kernel().finish(ctx, 0u);
        }
    });
}

void register_callbacks_and_timers(HleRegistrar &hle) {
    hle.add("ThreadManForUser", "sceKernelCreateCallback", [](Runtime &rt, AllegrexContext &ctx) {
        const SceUID uid = kernel().allocate_uid();
        kernel().callbacks[uid] = Callback{read_cstring(rt.memory(), arg(ctx, 0), 32u), arg(ctx, 1), arg(ctx, 2),
                                           kernel().current_uid()};
        kernel().finish(ctx, as_unsigned(uid));
    });
    hle.add("ThreadManForUser", "sceKernelDeleteCallback", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, kernel().callbacks.erase(as_signed(arg(ctx, 0))) != 0u ? 0u : error::kUnknownCbid);
    });

    hle.add("ThreadManForUser", "sceKernelCreateVTimer", [](Runtime &rt, AllegrexContext &ctx) {
        const SceUID uid = kernel().allocate_uid();
        VTimer timer{};
        timer.name = read_cstring(rt.memory(), arg(ctx, 0), 32u);
        kernel().vtimers[uid] = timer;
        kernel().finish(ctx, as_unsigned(uid));
    });
    hle.add("ThreadManForUser", "sceKernelStartVTimer", [](Runtime &, AllegrexContext &ctx) {
        auto found = kernel().vtimers.find(as_signed(arg(ctx, 0)));
        if (found == kernel().vtimers.end()) {
            kernel().finish(ctx, error::kUnknownVtid);
            return;
        }
        if (found->second.active) {
            kernel().finish(ctx, 1u);
            return;
        }
        found->second.active = true;
        found->second.base_us = kernel().now_us();
        kernel().finish(ctx, 0u);
    });
    hle.add("ThreadManForUser", "sceKernelSetVTimerHandlerWide", [](Runtime &, AllegrexContext &ctx) {
        auto found = kernel().vtimers.find(as_signed(arg(ctx, 0)));
        if (found == kernel().vtimers.end()) {
            kernel().finish(ctx, error::kUnknownVtid);
            return;
        }
        found->second.schedule_us = arg64(ctx, 2);
        found->second.handler = arg(ctx, 4);
        found->second.common = arg(ctx, 5);
        kernel().finish(ctx, 0u);
    });
}


// Lightweight mutexes.
//
// A lightweight mutex lives in a work area the game owns, so that an
// uncontended lock can be taken in user space without entering the kernel.
// This game imports all of the calls, so every lock comes through here and the
// work area is bookkeeping rather than the source of truth: the mutex itself
// is an ordinary kernel mutex, found by the work area's address.
//
// The fields are still written where the PSP's SceLwMutexWorkarea documents
// them, for any code that reads them without calling. That layout is taken
// from the public description of the structure and has not been checked
// against what this game does with it.
namespace lwmutex {

constexpr std::uint32_t kLockLevel = 0u;
constexpr std::uint32_t kLockThread = 4u;
constexpr std::uint32_t kAttributes = 8u;
constexpr std::uint32_t kWaitingThreads = 12u;
constexpr std::uint32_t kUid = 16u;

std::map<std::uint32_t, SceUID> &by_work_area() {
    static std::map<std::uint32_t, SceUID> table;
    return table;
}

void write_state(psprecomp::GuestMemory &memory, std::uint32_t work_area, const Mutex &mutex) {
    memory.store32(work_area + kLockLevel, as_unsigned(mutex.lock_count));
    memory.store32(work_area + kLockThread, as_unsigned(mutex.owner));
    memory.store32(work_area + kWaitingThreads, static_cast<std::uint32_t>(mutex.waiters.size()));
}

// The kernel mutex behind a work area, or null when the game never created one.
Mutex *find(std::uint32_t work_area, SceUID &uid) {
    const auto entry = by_work_area().find(work_area);
    if (entry == by_work_area().end()) return nullptr;
    uid = entry->second;
    const auto found = kernel().mutexes.find(uid);
    return found != kernel().mutexes.end() ? &found->second : nullptr;
}

} // namespace lwmutex

// Runs a function on a stack of its own and returns what it returned.
//
// A game calls this when it is about to recurse deeper than its thread's stack
// allows: the kernel takes a block of the size asked for, moves sp into it,
// calls entry(argument), then puts sp back. Stubbing it out returns without
// running entry at all, which for this game meant module_start finished
// without ever creating the game's main thread.
// Mailboxes: a queue a thread can post a message pointer to and another can
// wait on. The message itself belongs to the guest and is never touched here.
void register_mailboxes(HleRegistrar &hle) {
    // Ordering messages by the priority byte in the guest's own message header
    // would mean knowing that header's layout. FIFO is what this does, and it
    // says so rather than pretending.
    constexpr std::uint32_t kMbxAttrMessagePriority = 0x400u;
    hle.add("ThreadManForUser", "sceKernelCreateMbx", [](Runtime &rt, AllegrexContext &ctx) {
        const SceUID uid = kernel().allocate_uid();
        Mailbox mailbox;
        mailbox.name = read_cstring(rt.memory(), arg(ctx, 0), 32u);
        mailbox.attributes = arg(ctx, 1);
        if ((mailbox.attributes & kMbxAttrMessagePriority) != 0u)
            log_once("mbx-priority", "[kernel] mailbox \"" + mailbox.name +
                                         "\" asks for messages in priority order; they are delivered in the order "
                                         "they were sent");
        kernel().mailboxes[uid] = std::move(mailbox);
        kernel().finish(ctx, as_unsigned(uid));
    });
    hle.add("ThreadManForUser", "sceKernelDeleteMbx", [](Runtime &, AllegrexContext &ctx) {
        auto found = kernel().mailboxes.find(as_signed(arg(ctx, 0)));
        if (found == kernel().mailboxes.end()) {
            kernel().finish(ctx, error::kUnknownMbxid);
            return;
        }
        kernel().cancel_waiters(found->second.waiters, error::kWaitDelete);
        kernel().mailboxes.erase(found);
        kernel().finish(ctx, 0u);
    });
    hle.add("ThreadManForUser", "sceKernelSendMbx", [](Runtime &, AllegrexContext &ctx) {
        const SceUID uid = as_signed(arg(ctx, 0));
        auto found = kernel().mailboxes.find(uid);
        if (found == kernel().mailboxes.end()) {
            kernel().finish(ctx, error::kUnknownMbxid);
            return;
        }
        found->second.messages.push_back(arg(ctx, 1));
        kernel().finish(ctx, 0u);
        kernel().release_mailbox_waiters(uid);
    });
    hle.add("ThreadManForUser", "sceKernelPollMbx", [](Runtime &rt, AllegrexContext &ctx) {
        auto found = kernel().mailboxes.find(as_signed(arg(ctx, 0)));
        if (found == kernel().mailboxes.end()) {
            kernel().finish(ctx, error::kUnknownMbxid);
            return;
        }
        Mailbox &mailbox = found->second;
        if (mailbox.messages.empty()) {
            kernel().finish(ctx, error::kMbxNoMessage);
            return;
        }
        if (arg(ctx, 1) != 0u) rt.memory().store32(arg(ctx, 1), mailbox.messages.front());
        mailbox.messages.pop_front();
        kernel().finish(ctx, 0u);
    });
    const auto receive = [](Runtime &rt, AllegrexContext &ctx) {
        const SceUID uid = as_signed(arg(ctx, 0));
        auto found = kernel().mailboxes.find(uid);
        if (found == kernel().mailboxes.end()) {
            kernel().finish(ctx, error::kUnknownMbxid);
            return;
        }
        Mailbox &mailbox = found->second;
        // A thread already queued has waited longer, so it goes first.
        if (mailbox.waiters.empty() && !mailbox.messages.empty()) {
            if (arg(ctx, 1) != 0u) rt.memory().store32(arg(ctx, 1), mailbox.messages.front());
            mailbox.messages.pop_front();
            kernel().finish(ctx, 0u);
            return;
        }
        mailbox.waiters.push_back(kernel().current_uid());
        WaitState wait{};
        wait.type = WaitType::Mailbox;
        wait.object = uid;
        wait.out_address = arg(ctx, 1);
        wait.timeout_address = arg(ctx, 2);
        kernel().block(ctx, wait, 0u);
    };
    hle.try_add("ThreadManForUser", "sceKernelReceiveMbx", receive);
    hle.add("ThreadManForUser", "sceKernelReceiveMbxCB", receive);
}

void register_stack_extension(HleRegistrar &hle) {
    hle.add("ThreadManForUser", "sceKernelExtendThreadStack", [](Runtime &, AllegrexContext &ctx) {
        const std::uint32_t size = arg(ctx, 0);
        const std::uint32_t entry = arg(ctx, 1);
        const std::uint32_t argument = arg(ctx, 2);
        if (entry == 0u) {
            kernel().finish(ctx, error::kIllegalEntry);
            return;
        }
        const std::int32_t block = kernel().allocate_block("ExtendStack", 1u, (size + 255u) & ~std::uint32_t{255u}, 0u);
        const MemoryBlock *memory_block = block >= 0 ? kernel().find_block(block) : nullptr;
        const std::uint32_t saved_sp = ctx.gpr[29];
        if (memory_block != nullptr) {
            // The stack grows down from the top of the block, aligned as the
            // ABI wants it.
            ctx.set_gpr(29, (memory_block->address + memory_block->size) & ~std::uint32_t{15u});
        }
        kernel().call_guest(ctx, entry, {argument, 0u, 0u, 0u},
                            [saved_sp, block](AllegrexContext &returned, std::uint32_t result) {
                                returned.set_gpr(29, saved_sp);
                                if (block >= 0) (void)kernel().free_block(block);
                                kernel().finish(returned, result);
                            });
    });
}

void register_lightweight_mutexes(HleRegistrar &hle) {
    hle.add("ThreadManForUser", "sceKernelCreateLwMutex", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t work_area = arg(ctx, 0);
        const SceUID uid = kernel().allocate_uid();
        Mutex mutex{read_cstring(rt.memory(), arg(ctx, 1), 32u), arg(ctx, 2), 0, as_signed(arg(ctx, 3)), {}};
        if (mutex.lock_count > 0) mutex.owner = kernel().current_uid();
        kernel().mutexes[uid] = std::move(mutex);
        lwmutex::by_work_area()[work_area] = uid;
        rt.memory().store32(work_area + lwmutex::kAttributes, arg(ctx, 2));
        rt.memory().store32(work_area + lwmutex::kUid, as_unsigned(uid));
        lwmutex::write_state(rt.memory(), work_area, kernel().mutexes[uid]);
        kernel().finish(ctx, 0u);
    });
    hle.add("ThreadManForUser", "sceKernelDeleteLwMutex", [](Runtime &, AllegrexContext &ctx) {
        const std::uint32_t work_area = arg(ctx, 0);
        SceUID uid = 0;
        Mutex *mutex = lwmutex::find(work_area, uid);
        if (mutex == nullptr) {
            kernel().finish(ctx, error::kMutexNotFound);
            return;
        }
        kernel().cancel_waiters(mutex->waiters, error::kWaitDelete);
        kernel().mutexes.erase(uid);
        lwmutex::by_work_area().erase(work_area);
        kernel().finish(ctx, 0u);
    });

    // Locking and unlocking are exported by Kernel_Library, because on a PSP
    // the uncontended case never leaves user space.
    const auto lock = [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t work_area = arg(ctx, 0);
        const std::int32_t count = as_signed(arg(ctx, 1));
        SceUID uid = 0;
        Mutex *mutex = lwmutex::find(work_area, uid);
        if (mutex == nullptr) {
            kernel().finish(ctx, error::kMutexNotFound);
            return;
        }
        if (count <= 0) {
            kernel().finish(ctx, error::kIllegalCount);
            return;
        }
        if (mutex->lock_count == 0) {
            mutex->owner = kernel().current_uid();
            mutex->lock_count = count;
            lwmutex::write_state(rt.memory(), work_area, *mutex);
            kernel().finish(ctx, 0u);
            return;
        }
        if (mutex->owner == kernel().current_uid()) {
            if ((mutex->attributes & kMutexAttrRecursive) == 0u) {
                kernel().finish(ctx, error::kMutexRecursiveNotAllowed);
                return;
            }
            mutex->lock_count += count;
            lwmutex::write_state(rt.memory(), work_area, *mutex);
            kernel().finish(ctx, 0u);
            return;
        }
        mutex->waiters.push_back(kernel().current_uid());
        lwmutex::write_state(rt.memory(), work_area, *mutex);
        WaitState wait{};
        wait.type = WaitType::Mutex;
        wait.object = uid;
        wait.value = static_cast<std::uint32_t>(count);
        wait.timeout_address = arg(ctx, 2);
        kernel().block(ctx, wait, 0u);
    };
    hle.add("Kernel_Library", "sceKernelLockLwMutex", lock);
    hle.add("Kernel_Library", "sceKernelLockLwMutexCB", lock);
    hle.add("Kernel_Library", "sceKernelUnlockLwMutex", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t work_area = arg(ctx, 0);
        const std::int32_t count = as_signed(arg(ctx, 1));
        SceUID uid = 0;
        Mutex *mutex = lwmutex::find(work_area, uid);
        if (mutex == nullptr) {
            kernel().finish(ctx, error::kMutexNotFound);
            return;
        }
        if (count <= 0) {
            kernel().finish(ctx, error::kIllegalCount);
            return;
        }
        if (mutex->lock_count < count) {
            kernel().finish(ctx, error::kMutexUnlockUnderflow);
            return;
        }
        mutex->lock_count -= count;
        if (mutex->lock_count == 0) {
            mutex->owner = 0;
            kernel().release_mutex_waiters(uid);
        }
        lwmutex::write_state(rt.memory(), work_area, *mutex);
        kernel().finish(ctx, 0u);
    });
}

void register_kernel_library(HleRegistrar &hle) {
    // Bytes left on the current thread's stack, which a game checks before it
    // recurses. A stub returning 0 says the stack is exhausted, and the game
    // believes it.
    // sceKernelGetThreadStackFreeSize(uid), 0 the calling thread: the space
    // between the stack's bottom and where the thread's stack pointer is.
    // A PSP measures the part never written (its fill pattern); this is the
    // part not in use now, never less. Patapon and Chinatown Wars import it.
    hle.add("ThreadManForUser", "sceKernelGetThreadStackFreeSize", [](Runtime &, AllegrexContext &ctx) {
        log_once("thread-stack-free", "[kernel] sceKernelGetThreadStackFreeSize (UNVERIFIED: no game traced yet)");
        const SceUID uid = as_signed(arg(ctx, 0));
        const Thread *thread = kernel().find_thread(uid == 0 ? kernel().current_uid() : uid);
        if (thread == nullptr) {
            kernel().finish(ctx, error::kUnknownThid);
            return;
        }
        const std::uint32_t sp = thread->uid == kernel().current_uid() ? ctx.gpr[29] : thread->context.gpr[29];
        kernel().finish(ctx, sp > thread->stack_bottom ? sp - thread->stack_bottom : 0u);
    });
    hle.add("ThreadManForUser", "sceKernelCheckThreadStack", [](Runtime &, AllegrexContext &ctx) {
        const Thread *thread = kernel().current_thread();
        const std::uint32_t sp = ctx.gpr[29];
        kernel().finish(ctx, thread != nullptr && sp > thread->stack_bottom ? sp - thread->stack_bottom : 0u);
    });
    hle.add("Kernel_Library", "sceKernelCheckThreadStack", [](Runtime &, AllegrexContext &ctx) {
        const Thread *thread = kernel().current_thread();
        if (thread == nullptr) {
            kernel().finish(ctx, 0u);
            return;
        }
        const std::uint32_t sp = ctx.gpr[29];
        const std::uint32_t free_bytes = sp > thread->stack_bottom ? sp - thread->stack_bottom : 0u;
        kernel().finish(ctx, free_bytes);
    });
    // The intrinsic memcpy: returning without copying corrupts whatever the
    // game expected to be copied.
    hle.add("Kernel_Library", "sceKernelMemcpy", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t destination = arg(ctx, 0);
        const std::uint32_t source = arg(ctx, 1);
        const std::uint32_t size = arg(ctx, 2);
        for (std::uint32_t i = 0; i < size; ++i) rt.memory().store8(destination + i, rt.memory().load8(source + i));
        kernel().finish(ctx, destination);
    });

    hle.add("Kernel_Library", "sceKernelCpuSuspendIntr", [](Runtime &, AllegrexContext &ctx) {
        const bool was_enabled = kernel().interrupts_enabled();
        kernel().set_interrupts_enabled(false);
        kernel().finish(ctx, was_enabled ? 1u : 0u);
    });
    const auto resume_interrupts = [](Runtime &, AllegrexContext &ctx) {
        kernel().set_interrupts_enabled(arg(ctx, 0) != 0u);
        kernel().finish(ctx, 0u);
    };
    hle.add("Kernel_Library", "sceKernelCpuResumeIntr", resume_interrupts);
    // sceKernelIsCpuIntrEnable() -> 1 while interrupts are enabled. God of
    // War (UCES00842) asks it at start-up and, given 0, stops in a loop that
    // never ends.
    hle.add("Kernel_Library", "sceKernelIsCpuIntrEnable", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, kernel().interrupts_enabled() ? 1u : 0u);
    });
    // With a pipeline sync on a PSP, which has nothing to wait for here.
    hle.add("Kernel_Library", "sceKernelCpuResumeIntrWithSync", resume_interrupts);
    hle.add("Kernel_Library", "sceKernelMemset", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t address = arg(ctx, 0);
        const auto value = static_cast<std::uint8_t>(arg(ctx, 1));
        const std::uint32_t size = arg(ctx, 2);
        for (std::uint32_t i = 0; i < size; ++i) rt.memory().store8(address + i, value);
        kernel().finish(ctx, address);
    });
}


// Variable-size memory pools. The pool is a block of partition memory; what
// is allocated inside it is kept here, on the host, so the guest's memory
// holds only what the game writes into its allocations.
//
// A PSP keeps its own management records inside the pool, so it reports
// less free space than it was given. That overhead is not documented where
// this project can use it, so this pool charges its own: every allocation is
// rounded up to 8 bytes and costs 8 more, and the free size it reports is the
// largest allocation that would succeed. A game that allocates exactly what
// sceKernelReferVplStatus reported therefore gets it.
struct Vpl {
    std::string name;
    std::uint32_t attributes{};
    SceUID block{};
    std::uint32_t address{};
    std::uint32_t size{};
    std::map<std::uint32_t, std::uint32_t> used; // start -> bytes, including the overhead
    std::uint32_t waiting{};                     // threads blocked in sceKernelAllocateVpl
    std::uint64_t cancel_generation{};           // bumped by sceKernelCancelVpl
};

constexpr std::uint32_t kVplOverhead = 8u;
constexpr std::uint32_t kVplAttrHighMemory = 0x4000u;
constexpr std::uint32_t kUnknownVplid = 0x8002019Cu;
constexpr std::uint32_t kUnknownFplid = 0x8002019Du;
constexpr std::uint32_t kIllegalAttr = 0x80020191u;
constexpr std::uint32_t kWaitCancel = 0x800201A9u;
constexpr std::uint32_t kIllegalMemsize = 0x800201B7u;

std::map<SceUID, Vpl> &vpls() {
    static std::map<SceUID, Vpl> pools;
    return pools;
}

// The largest free gap, in bytes.
std::uint32_t vpl_largest_gap(const Vpl &pool) {
    std::uint32_t largest = 0u;
    std::uint32_t cursor = pool.address;
    for (const auto &[start, bytes] : pool.used) {
        largest = std::max(largest, start - cursor);
        cursor = start + bytes;
    }
    return std::max(largest, pool.address + pool.size - cursor);
}

std::uint32_t vpl_free_size(const Vpl &pool) {
    const std::uint32_t gap = vpl_largest_gap(pool);
    return gap > kVplOverhead ? gap - kVplOverhead : 0u;
}

// First fit. Returns the address the game gets, past the overhead.
std::optional<std::uint32_t> vpl_allocate(Vpl &pool, std::uint32_t size) {
    const std::uint64_t needed = ((static_cast<std::uint64_t>(size) + 7u) & ~std::uint64_t{7u}) + kVplOverhead;
    std::uint32_t cursor = pool.address;
    const auto take = [&](std::uint32_t start) {
        pool.used.emplace(start, static_cast<std::uint32_t>(needed));
        return start + kVplOverhead;
    };
    for (const auto &[start, bytes] : pool.used) {
        if (start - cursor >= needed) return take(cursor);
        cursor = start + bytes;
    }
    if (pool.address + pool.size - cursor >= needed) return take(cursor);
    return std::nullopt;
}

void register_vpls(HleRegistrar &hle) {
    hle.add("ThreadManForUser", "sceKernelCreateVpl", [](Runtime &rt, AllegrexContext &ctx) {
        Vpl pool;
        pool.name = read_cstring(rt.memory(), arg(ctx, 0), 32u);
        pool.attributes = arg(ctx, 2);
        const std::uint32_t size = arg(ctx, 3);
        if (size == 0u || size > 0x7FFFFFFFu) {
            kernel().finish(ctx, error::kIllegalArgument);
            return;
        }
        const std::int32_t block = kernel().allocate_block(
            "vpl:" + pool.name, (pool.attributes & kVplAttrHighMemory) != 0u ? 1u : 0u, size, 0u);
        if (block < 0) {
            kernel().finish(ctx, as_unsigned(block));
            return;
        }
        pool.block = block;
        pool.address = kernel().find_block(block)->address;
        pool.size = size;
        const SceUID uid = kernel().allocate_uid();
        if (trace_sync())
            log_sync("[kernel] vpl " + pool.name + " size=" + psprecomp::hex32(size) + " at " +
                     psprecomp::hex32(pool.address) + " uid=" + std::to_string(uid));
        vpls().emplace(uid, std::move(pool));
        kernel().finish(ctx, as_unsigned(uid));
    });
    hle.add("ThreadManForUser", "sceKernelDeleteVpl", [](Runtime &, AllegrexContext &ctx) {
        const auto found = vpls().find(as_signed(arg(ctx, 0)));
        if (found == vpls().end()) {
            kernel().finish(ctx, kUnknownVplid);
            return;
        }
        (void)kernel().free_block(found->second.block);
        vpls().erase(found);
        kernel().finish(ctx, 0u);
    });
    // Blocking allocation. A waiter polls until the pool has room, the pool is
    // deleted, or the timeout (microseconds, at a3, rewritten with what is
    // left, as every other timed wait does) runs out.
    const auto allocate = [](Runtime &rt, AllegrexContext &ctx) {
        const SceUID uid = as_signed(arg(ctx, 0));
        const auto found = vpls().find(uid);
        if (found == vpls().end()) {
            kernel().finish(ctx, kUnknownVplid);
            return;
        }
        const std::uint32_t size = arg(ctx, 1);
        if (size == 0u || size > found->second.size) {
            kernel().finish(ctx, error::kIllegalArgument);
            return;
        }
        const std::uint32_t out = arg(ctx, 2);
        const std::uint32_t timeout_address = arg(ctx, 3);
        std::optional<std::uint64_t> timeout;
        if (timeout_address != 0u) timeout = rt.memory().load32(timeout_address);
        auto &memory = rt.memory();
        // Room now: no wait at all.
        if (const auto address = vpl_allocate(found->second, size)) {
            if (out != 0u) memory.store32(out, *address);
            kernel().finish(ctx, 0u);
            return;
        }
        ++found->second.waiting;
        const std::uint64_t generation = found->second.cancel_generation;
        kernel().wait_host(ctx, timeout, [uid, size, out, timeout_address, generation, &memory](bool timed_out) -> std::optional<std::uint32_t> {
            const auto pool = vpls().find(uid);
            if (pool == vpls().end()) return error::kWaitDelete;
            if (pool->second.cancel_generation != generation) return kWaitCancel;  // counted out by the cancel
            const auto leave = [&](std::uint32_t result) {
                --pool->second.waiting;
                return result;
            };
            if (const auto address = vpl_allocate(pool->second, size)) {
                if (out != 0u) memory.store32(out, *address);
                return leave(0u);
            }
            if (timed_out) {
                if (timeout_address != 0u) memory.store32(timeout_address, 0u);
                return leave(error::kWaitTimeout);
            }
            return std::nullopt;
        });
    };
    hle.add("ThreadManForUser", "sceKernelAllocateVpl", allocate);
    hle.add("ThreadManForUser", "sceKernelAllocateVplCB", allocate);
    hle.add("ThreadManForUser", "sceKernelTryAllocateVpl", [](Runtime &rt, AllegrexContext &ctx) {
        const auto found = vpls().find(as_signed(arg(ctx, 0)));
        if (found == vpls().end()) {
            kernel().finish(ctx, kUnknownVplid);
            return;
        }
        const std::uint32_t size = arg(ctx, 1);
        if (size == 0u || size > found->second.size) {
            kernel().finish(ctx, error::kIllegalArgument);
            return;
        }
        const auto address = vpl_allocate(found->second, size);
        if (!address) {
            kernel().finish(ctx, error::kNoMemory);
            return;
        }
        if (arg(ctx, 2) != 0u) rt.memory().store32(arg(ctx, 2), *address);
        kernel().finish(ctx, 0u);
    });
    hle.add("ThreadManForUser", "sceKernelFreeVpl", [](Runtime &, AllegrexContext &ctx) {
        const auto found = vpls().find(as_signed(arg(ctx, 0)));
        if (found == vpls().end()) {
            kernel().finish(ctx, kUnknownVplid);
            return;
        }
        const auto used = found->second.used.find(arg(ctx, 1) - kVplOverhead);
        if (used == found->second.used.end()) {
            kernel().finish(ctx, error::kIllegalMemblock);
            return;
        }
        found->second.used.erase(used);
        kernel().finish(ctx, 0u);
    });
    // Releases every waiting thread with SCE_KERNEL_ERROR_WAIT_CANCEL and
    // stores how many there were.
    hle.add("ThreadManForUser", "sceKernelCancelVpl", [](Runtime &rt, AllegrexContext &ctx) {
        const auto found = vpls().find(as_signed(arg(ctx, 0)));
        if (found == vpls().end()) {
            kernel().finish(ctx, kUnknownVplid);
            return;
        }
        const std::uint32_t released = found->second.waiting;
        found->second.waiting = 0u;
        ++found->second.cancel_generation;
        if (arg(ctx, 1) != 0u) rt.memory().store32(arg(ctx, 1), released);
        kernel().finish(ctx, 0u);
    });
    // SceKernelVplInfo: size, name[32], attr, poolSize, freeSize, numWaitThreads.
    hle.add("ThreadManForUser", "sceKernelReferVplStatus", [](Runtime &rt, AllegrexContext &ctx) {
        const auto found = vpls().find(as_signed(arg(ctx, 0)));
        if (found == vpls().end()) {
            kernel().finish(ctx, kUnknownVplid);
            return;
        }
        const Vpl &pool = found->second;
        const std::uint32_t info = arg(ctx, 1);
        auto &memory = rt.memory();
        if (memory.load32(info) != 0u) {
            for (std::uint32_t i = 0; i < 32u; ++i)
                memory.store8(info + 4u + i, i < pool.name.size() ? static_cast<std::uint8_t>(pool.name[i]) : 0u);
            memory.store32(info + 36u, pool.attributes);
            memory.store32(info + 40u, pool.size);
            memory.store32(info + 44u, vpl_free_size(pool));
            memory.store32(info + 48u, pool.waiting);
        }
        kernel().finish(ctx, 0u);
    });
}

// Fixed-size memory pools: `count` blocks of one size in a block of partition
// memory (kernel/fixed_pool.hpp keeps the books). The calls and the layout of
// SceKernelFplInfo follow the SDK's pspthreadman.h; the option block's
// alignment word (at +4, when its size says it is there) and the attributes
// are what games pass, logged with <prefix>_TRACE_SYNC.
struct Fpl {
    std::string name;
    std::uint32_t attributes{};
    SceUID block{};
    kernel_pools::FixedPool pool;
};

// 0x100: waiting threads are served by priority rather than in order (served
// in order here, see below); 0x4000: from the top of the partition.
constexpr std::uint32_t kFplAttrPriority = 0x100u;
constexpr std::uint32_t kFplAttrHighMemory = 0x4000u;
constexpr std::uint32_t kFplKnownAttributes = kFplAttrPriority | kFplAttrHighMemory;

std::map<SceUID, Fpl> &fpls() {
    static std::map<SceUID, Fpl> pools;
    return pools;
}

void register_fpls(HleRegistrar &hle) {
    // sceKernelCreateFpl(name, partition, attr, block size, blocks, option)
    hle.add("ThreadManForUser", "sceKernelCreateFpl", [](Runtime &rt, AllegrexContext &ctx) {
        auto &memory = rt.memory();
        Fpl fpl;
        fpl.name = arg(ctx, 0) != 0u ? read_cstring(memory, arg(ctx, 0), 32u) : std::string();
        const std::uint32_t partition = arg(ctx, 1);
        fpl.attributes = arg(ctx, 2);
        const std::uint32_t block_size = arg(ctx, 3);
        const std::uint32_t count = ctx.gpr[8];   // t0: the fifth argument
        const std::uint32_t option = ctx.gpr[9];  // t1
        std::uint32_t alignment = 0u;
        if (option != 0u && memory.load32(option) >= 8u) alignment = memory.load32(option + 4u);
        if (trace_sync())
            log_sync("[kernel] CreateFpl \"" + fpl.name + "\" partition=" + std::to_string(partition) + " attr=" +
                     psprecomp::hex32(fpl.attributes) + " block=" + psprecomp::hex32(block_size) + " count=" +
                     std::to_string(count) + " option=" + psprecomp::hex32(option) + " align=" +
                     std::to_string(alignment));
        if ((fpl.attributes & ~kFplKnownAttributes) != 0u) {
            kernel().finish(ctx, kIllegalAttr);
            return;
        }
        const auto layout = kernel_pools::fixed_pool_layout(block_size, count, alignment);
        if (!layout) {
            kernel().finish(ctx, block_size == 0u || count == 0u ? kIllegalMemsize : error::kIllegalArgument);
            return;
        }
        if ((fpl.attributes & kFplAttrPriority) != 0u)
            log_once("fpl-priority", "[kernel] an Fpl asks for waiters by priority; they are served in order");
        const bool high = (fpl.attributes & kFplAttrHighMemory) != 0u;
        // Aligned block types (3 low, 4 high) when the pool asks for more than
        // the partition's own alignment.
        const std::int32_t block =
            layout->alignment > 4u
                ? kernel().allocate_block("fpl:" + fpl.name, high ? 4u : 3u, layout->bytes, layout->alignment)
                : kernel().allocate_block("fpl:" + fpl.name, high ? 1u : 0u, layout->bytes, 0u);
        if (block < 0) {
            kernel().finish(ctx, error::kNoMemory);
            return;
        }
        fpl.block = block;
        fpl.pool = kernel_pools::FixedPool(kernel().find_block(block)->address, block_size, count, *layout);
        const SceUID uid = kernel().allocate_uid();
        if (trace_sync())
            log_sync("[kernel] fpl " + fpl.name + " uid=" + std::to_string(uid) + " at " +
                     psprecomp::hex32(fpl.pool.address()) + " bytes=" + psprecomp::hex32(layout->bytes));
        fpls().emplace(uid, std::move(fpl));
        kernel().finish(ctx, as_unsigned(uid));
    });
    // Threads waiting on it wake with SCE_KERNEL_ERROR_WAIT_DELETE.
    hle.add("ThreadManForUser", "sceKernelDeleteFpl", [](Runtime &, AllegrexContext &ctx) {
        const auto found = fpls().find(as_signed(arg(ctx, 0)));
        if (found == fpls().end()) {
            kernel().finish(ctx, kUnknownFplid);
            return;
        }
        (void)kernel().free_block(found->second.block);
        fpls().erase(found);
        kernel().finish(ctx, 0u);
    });
    // sceKernelAllocateFpl(uid, void **block, unsigned *timeout). A thread that
    // finds no free block waits, first come first served, until one is freed,
    // the pool is deleted or cancelled, or the timeout (microseconds, rewritten
    // with what is left: 0 when it runs out) passes.
    const auto allocate = [](Runtime &rt, AllegrexContext &ctx) {
        const SceUID uid = as_signed(arg(ctx, 0));
        const auto found = fpls().find(uid);
        if (found == fpls().end()) {
            kernel().finish(ctx, kUnknownFplid);
            return;
        }
        auto &memory = rt.memory();
        const std::uint32_t out = arg(ctx, 1);
        const std::uint32_t timeout_address = arg(ctx, 2);
        kernel_pools::FixedPool &pool = found->second.pool;
        if (pool.waiting() == 0u) {
            if (const auto block = pool.allocate()) {
                if (out != 0u) memory.store32(out, *block);
                if (trace_sync())
                    log_sync("[kernel] AllocateFpl " + std::to_string(uid) + " -> " + psprecomp::hex32(*block));
                kernel().finish(ctx, 0u);
                return;
            }
        }
        std::optional<std::uint64_t> timeout;
        if (timeout_address != 0u) timeout = memory.load32(timeout_address);
        const std::uint64_t ticket = pool.enqueue();
        const std::uint64_t generation = pool.cancel_generation();
        if (trace_sync()) log_sync("[kernel] AllocateFpl " + std::to_string(uid) + " waits");
        kernel().wait_host(ctx, timeout, [uid, out, timeout_address, ticket, generation, &memory](bool timed_out) -> std::optional<std::uint32_t> {
            const auto fpl = fpls().find(uid);
            if (fpl == fpls().end()) return error::kWaitDelete;
            kernel_pools::FixedPool &waited = fpl->second.pool;
            if (waited.cancel_generation() != generation) return kWaitCancel;
            if (waited.first_in_line(ticket)) {
                if (const auto block = waited.allocate()) {
                    waited.leave(ticket);
                    if (out != 0u) memory.store32(out, *block);
                    return 0u;
                }
            }
            if (timed_out) {
                waited.leave(ticket);
                if (timeout_address != 0u) memory.store32(timeout_address, 0u);
                return error::kWaitTimeout;
            }
            return std::nullopt;
        });
    };
    hle.add("ThreadManForUser", "sceKernelAllocateFpl", allocate);
    hle.add("ThreadManForUser", "sceKernelAllocateFplCB", allocate);
    // No free block, or threads already waiting for one: SCE_KERNEL_ERROR_NO_MEMORY.
    hle.add("ThreadManForUser", "sceKernelTryAllocateFpl", [](Runtime &rt, AllegrexContext &ctx) {
        const auto found = fpls().find(as_signed(arg(ctx, 0)));
        if (found == fpls().end()) {
            kernel().finish(ctx, kUnknownFplid);
            return;
        }
        kernel_pools::FixedPool &pool = found->second.pool;
        const auto block = pool.waiting() == 0u ? pool.allocate() : std::nullopt;
        if (!block) {
            kernel().finish(ctx, error::kNoMemory);
            return;
        }
        if (arg(ctx, 1) != 0u) rt.memory().store32(arg(ctx, 1), *block);
        kernel().finish(ctx, 0u);
    });
    hle.add("ThreadManForUser", "sceKernelFreeFpl", [](Runtime &, AllegrexContext &ctx) {
        const auto found = fpls().find(as_signed(arg(ctx, 0)));
        if (found == fpls().end()) {
            kernel().finish(ctx, kUnknownFplid);
            return;
        }
        if (!found->second.pool.free(arg(ctx, 1))) {
            kernel().finish(ctx, error::kIllegalMemblock);
            return;
        }
        kernel().finish(ctx, 0u);
    });
    hle.add("ThreadManForUser", "sceKernelCancelFpl", [](Runtime &rt, AllegrexContext &ctx) {
        const auto found = fpls().find(as_signed(arg(ctx, 0)));
        if (found == fpls().end()) {
            kernel().finish(ctx, kUnknownFplid);
            return;
        }
        const std::uint32_t released = found->second.pool.cancel_waiters();
        if (arg(ctx, 1) != 0u) rt.memory().store32(arg(ctx, 1), released);
        kernel().finish(ctx, 0u);
    });
    // SceKernelFplInfo: size, name[32], attr, blockSize, numBlocks, freeBlocks,
    // numWaitThreads. Written only when the game says it has room (size).
    hle.add("ThreadManForUser", "sceKernelReferFplStatus", [](Runtime &rt, AllegrexContext &ctx) {
        const auto found = fpls().find(as_signed(arg(ctx, 0)));
        if (found == fpls().end()) {
            kernel().finish(ctx, kUnknownFplid);
            return;
        }
        const Fpl &fpl = found->second;
        const std::uint32_t info = arg(ctx, 1);
        auto &memory = rt.memory();
        const std::uint32_t size = memory.load32(info);
        const auto put = [&](std::uint32_t offset, std::uint32_t value) {
            if (offset + 4u <= size) memory.store32(info + offset, value);
        };
        for (std::uint32_t i = 0; i < 32u && 4u + i < size; ++i)
            memory.store8(info + 4u + i, i < fpl.name.size() ? static_cast<std::uint8_t>(fpl.name[i]) : 0u);
        put(36u, fpl.attributes);
        put(40u, fpl.pool.block_size());
        put(44u, fpl.pool.count());
        put(48u, fpl.pool.free_count());
        put(52u, fpl.pool.waiting());
        kernel().finish(ctx, 0u);
    });
}


// Message pipes: a byte stream between threads, through a buffer of partition
// memory (kernel/message_pipe.hpp keeps the books; the bytes themselves are
// kept on the host). The calls follow the SDK's pspthreadman.h and uOFW's
// names for the arguments it leaves unnamed: send and receive take (uid,
// message, size, wait mode, int *result, unsigned *timeout), wait mode 0 for
// all of `size` and 1 for as much as there is, and *result gets the byte
// count. Patapon (UCES00995) passes messages to a worker thread this way when
// a new game starts.
constexpr std::uint32_t kUnknownMppid = 0x8002019Eu;
constexpr std::uint32_t kMppFull = 0x800201B3u;
constexpr std::uint32_t kMppEmpty = 0x800201B4u;
constexpr std::uint32_t kMppAttrHighMemory = 0x4000u;

struct MsgPipe {
    std::string name;
    std::uint32_t attributes{};
    SceUID block{-1};
    kernel_pipes::MessagePipe pipe;
};

std::map<SceUID, MsgPipe> &msg_pipes() {
    static std::map<SceUID, MsgPipe> pipes;
    return pipes;
}

std::vector<std::uint8_t> read_bytes(const psprecomp::GuestMemory &memory, std::uint32_t address, std::uint32_t size) {
    std::vector<std::uint8_t> bytes(size);
    if (size != 0u && memory.contains(address, size)) memory.copy_out(address, bytes);
    return bytes;
}

void write_bytes(psprecomp::GuestMemory &memory, std::uint32_t address, const std::vector<std::uint8_t> &bytes) {
    for (std::size_t i = 0; i < bytes.size(); ++i) memory.store8(address + static_cast<std::uint32_t>(i), bytes[i]);
}

void register_msg_pipes(HleRegistrar &hle) {
    // sceKernelCreateMsgPipe(name, partition, attr, buffer size, option)
    hle.add("ThreadManForUser", "sceKernelCreateMsgPipe", [](Runtime &rt, AllegrexContext &ctx) {
        MsgPipe pipe;
        pipe.name = read_cstring(rt.memory(), arg(ctx, 0), 32u);
        pipe.attributes = arg(ctx, 2);
        const std::uint32_t size = arg(ctx, 3);
        if (size != 0u) {
            const std::uint32_t type = (pipe.attributes & kMppAttrHighMemory) != 0u ? 1u : 0u;
            pipe.block = kernel().allocate_block("MsgPipe", type, size, 0u);
            if (pipe.block < 0) {
                kernel().finish(ctx, error::kNoMemory);
                return;
            }
        }
        pipe.pipe = kernel_pipes::MessagePipe(size);
        if (trace_sync())
            log_sync("[kernel] CreateMsgPipe \"" + pipe.name + "\" size=" + std::to_string(size) +
                     " attr=" + psprecomp::hex32(pipe.attributes));
        const SceUID uid = kernel().allocate_uid();
        msg_pipes()[uid] = std::move(pipe);
        kernel().finish(ctx, as_unsigned(uid));
    });
    hle.add("ThreadManForUser", "sceKernelDeleteMsgPipe", [](Runtime &, AllegrexContext &ctx) {
        const auto found = msg_pipes().find(as_signed(arg(ctx, 0)));
        if (found == msg_pipes().end()) {
            kernel().finish(ctx, kUnknownMppid);
            return;
        }
        if (found->second.block >= 0) (void)kernel().free_block(found->second.block);
        msg_pipes().erase(found);  // waiters see it gone and end with WAIT_DELETE
        kernel().finish(ctx, 0u);
    });

    const auto send = [](bool may_wait) {
        return [may_wait](Runtime &rt, AllegrexContext &ctx) {
            const SceUID uid = as_signed(arg(ctx, 0));
            const auto found = msg_pipes().find(uid);
            if (found == msg_pipes().end()) {
                kernel().finish(ctx, kUnknownMppid);
                return;
            }
            auto &memory = rt.memory();
            const std::uint32_t size = arg(ctx, 2);
            const bool whole = arg(ctx, 3) == 0u;
            const std::uint32_t result_address = ctx.gpr[8];   // t0
            const std::uint32_t timeout_address = ctx.gpr[9];  // t1
            std::vector<std::uint8_t> bytes = read_bytes(memory, arg(ctx, 1), size);
            kernel_pipes::MessagePipe &pipe = found->second.pipe;
            if (!may_wait) {
                const auto sent = pipe.try_send(bytes, whole);
                if (result_address != 0u && sent) memory.store32(result_address, *sent);
                kernel().finish(ctx, sent ? 0u : kMppFull);
                return;
            }
            const auto ticket = pipe.send(std::move(bytes), whole);
            const std::uint64_t generation = pipe.cancel_generation();
            std::optional<std::uint64_t> timeout;
            if (timeout_address != 0u) timeout = memory.load32(timeout_address);
            kernel().wait_host(ctx, timeout, [uid, ticket, generation, result_address, timeout_address, &memory](
                                                 bool timed_out) -> std::optional<std::uint32_t> {
                const auto pipe = msg_pipes().find(uid);
                if (pipe == msg_pipes().end()) return error::kWaitDelete;
                kernel_pipes::MessagePipe &waited = pipe->second.pipe;
                if (waited.cancel_generation() != generation) return kWaitCancel;
                if (const auto sent = waited.sent(ticket)) {
                    if (result_address != 0u) memory.store32(result_address, *sent);
                    return 0u;
                }
                if (!timed_out) return std::nullopt;
                const std::uint32_t sent = waited.withdraw(ticket);
                if (result_address != 0u) memory.store32(result_address, sent);
                if (timeout_address != 0u) memory.store32(timeout_address, 0u);
                return error::kWaitTimeout;
            });
        };
    };
    hle.add("ThreadManForUser", "sceKernelSendMsgPipe", send(true));
    hle.add("ThreadManForUser", "sceKernelSendMsgPipeCB", send(true));
    hle.add("ThreadManForUser", "sceKernelTrySendMsgPipe", send(false));

    const auto receive = [](bool may_wait) {
        return [may_wait](Runtime &rt, AllegrexContext &ctx) {
            const SceUID uid = as_signed(arg(ctx, 0));
            const auto found = msg_pipes().find(uid);
            if (found == msg_pipes().end()) {
                kernel().finish(ctx, kUnknownMppid);
                return;
            }
            auto &memory = rt.memory();
            const std::uint32_t address = arg(ctx, 1);
            const std::uint32_t size = arg(ctx, 2);
            const bool whole = arg(ctx, 3) == 0u;
            const std::uint32_t result_address = ctx.gpr[8];   // t0
            const std::uint32_t timeout_address = ctx.gpr[9];  // t1
            kernel_pipes::MessagePipe &pipe = found->second.pipe;
            if (!may_wait) {
                const auto got = pipe.try_receive(size, whole);
                if (got) write_bytes(memory, address, *got);
                if (result_address != 0u && got) memory.store32(result_address, static_cast<std::uint32_t>(got->size()));
                kernel().finish(ctx, got ? 0u : kMppEmpty);
                return;
            }
            const auto ticket = pipe.receive(size, whole);
            const std::uint64_t generation = pipe.cancel_generation();
            std::optional<std::uint64_t> timeout;
            if (timeout_address != 0u) timeout = memory.load32(timeout_address);
            kernel().wait_host(ctx, timeout, [uid, ticket, generation, address, result_address, timeout_address,
                                              &memory](bool timed_out) -> std::optional<std::uint32_t> {
                const auto pipe = msg_pipes().find(uid);
                if (pipe == msg_pipes().end()) return error::kWaitDelete;
                kernel_pipes::MessagePipe &waited = pipe->second.pipe;
                if (waited.cancel_generation() != generation) return kWaitCancel;
                if (const auto got = waited.received(ticket)) {
                    write_bytes(memory, address, *got);
                    if (result_address != 0u) memory.store32(result_address, static_cast<std::uint32_t>(got->size()));
                    return 0u;
                }
                if (!timed_out) return std::nullopt;
                (void)waited.withdraw(ticket);
                if (result_address != 0u) memory.store32(result_address, 0u);
                if (timeout_address != 0u) memory.store32(timeout_address, 0u);
                return error::kWaitTimeout;
            });
        };
    };
    hle.add("ThreadManForUser", "sceKernelReceiveMsgPipe", receive(true));
    hle.add("ThreadManForUser", "sceKernelReceiveMsgPipeCB", receive(true));
    hle.add("ThreadManForUser", "sceKernelTryReceiveMsgPipe", receive(false));

    // sceKernelCancelMsgPipe(uid, int *senders, int *receivers): every waiter
    // ends with WAIT_CANCEL, and the pipe is emptied.
    hle.add("ThreadManForUser", "sceKernelCancelMsgPipe", [](Runtime &rt, AllegrexContext &ctx) {
        const auto found = msg_pipes().find(as_signed(arg(ctx, 0)));
        if (found == msg_pipes().end()) {
            kernel().finish(ctx, kUnknownMppid);
            return;
        }
        log_once("mpp-cancel", "[kernel] sceKernelCancelMsgPipe (UNVERIFIED: no game traced yet)");
        kernel_pipes::MessagePipe &pipe = found->second.pipe;
        const std::uint32_t senders = pipe.waiting_senders();
        const std::uint32_t receivers = pipe.waiting_receivers();
        (void)pipe.cancel();
        pipe.clear();
        auto &memory = rt.memory();
        if (arg(ctx, 1) != 0u) memory.store32(arg(ctx, 1), senders);
        if (arg(ctx, 2) != 0u) memory.store32(arg(ctx, 2), receivers);
        kernel().finish(ctx, 0u);
    });
    // SceKernelMppInfo: size, name[32], attr, bufSize, freeSize,
    // numSendWaitThreads, numReceiveWaitThreads.
    hle.add("ThreadManForUser", "sceKernelReferMsgPipeStatus", [](Runtime &rt, AllegrexContext &ctx) {
        const auto found = msg_pipes().find(as_signed(arg(ctx, 0)));
        if (found == msg_pipes().end()) {
            kernel().finish(ctx, kUnknownMppid);
            return;
        }
        log_once("mpp-refer", "[kernel] sceKernelReferMsgPipeStatus (UNVERIFIED: no game traced yet)");
        const MsgPipe &pipe = found->second;
        auto &memory = rt.memory();
        const std::uint32_t info = arg(ctx, 1);
        if (info != 0u && memory.load32(info) >= 56u) {
            for (std::uint32_t i = 0; i < 32u; ++i)
                memory.store8(info + 4u + i, i < pipe.name.size() ? static_cast<std::uint8_t>(pipe.name[i]) : 0u);
            memory.store32(info + 36u, pipe.attributes);
            memory.store32(info + 40u, pipe.pipe.capacity());
            memory.store32(info + 44u, pipe.pipe.free_space());
            memory.store32(info + 48u, pipe.pipe.waiting_senders());
            memory.store32(info + 52u, pipe.pipe.waiting_receivers());
        }
        kernel().finish(ctx, 0u);
    });
}
} // namespace

void register_threadman(HleRegistrar &hle) {
    register_threads(hle);
    register_time(hle);
    register_semaphores(hle);
    register_event_flags(hle);
    register_mutexes(hle);
    register_lightweight_mutexes(hle);
    register_stack_extension(hle);
    register_mailboxes(hle);
    register_vpls(hle);
    register_fpls(hle);
    register_msg_pipes(hle);
    register_callbacks_and_timers(hle);
    register_kernel_library(hle);
}

} // namespace portablekit
