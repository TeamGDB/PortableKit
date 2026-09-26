// sceDisplay, sceCtrl, sceGe_user, sceAudio and sceSasCore. These keep guest
// timing and callbacks behaving like hardware (vblank-paced input reads, GE
// list completion callbacks, blocking audio output); the drawing and the
// mixing themselves live under gpu/ and audio/.
#include "../profile.hpp"
#include "hle_common.hpp"
#include "kernel/fast_loading.hpp"
#include "kernel/load_trace.hpp"

#include "overlays.hpp"

#include "audio/audio_sink.hpp"
#include "audio/sas_core.hpp"

#include "psprecomp/common.hpp"

#include "camera_probe.hpp"
#include "camera/camera_input.hpp"
#include "camera/camera_driver.hpp"
#include "input/bindings.hpp"
#include "settings/settings.hpp"
#include "gpu/ge_state.hpp"
#include "perf/frame_stats.hpp"
#if defined(PORTABLEKIT_HAS_RENDERER)
#include "gpu/vulkan_renderer.hpp"
#include "ui/ui.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

namespace portablekit {
namespace {

// <prefix>_TRACE_PACING=N prints the calls that pace a game's frames -- the
// flip, the vblank waits, the controller reads and the vcount -- with the
// emulated time, the vblank count, how far into the vblank period the call
// came and the calling thread, N lines of them (3000 when N is not a
// number), from vblank <prefix>_TRACE_PACING_FROM on (0 by default). It is
// what a question like "how many vblanks does this game's frame take, and
// which call waits them" is answered with.
void trace_pacing(const char *call, std::int64_t value = -1) {
    static const long limit = [] {
        const char *text = portablekit::env("TRACE_PACING");
        if (text == nullptr) return 0L;
        const long n = std::strtol(text, nullptr, 10);
        return n > 1 ? n : 3000L;
    }();
    static const std::uint64_t from = [] {
        const char *text = portablekit::env("TRACE_PACING_FROM");
        return text != nullptr ? std::strtoull(text, nullptr, 10) : 0ull;
    }();
    static long lines = 0;
    if (lines >= limit || kernel().vblank_count() < from) return;
    ++lines;
    const std::uint64_t now = kernel().now_us();
    const Thread *thread = kernel().current_thread();
    std::printf("[pacing] %12.3f ms v%-7llu +%5llu us %-20s %s", static_cast<double>(now) / 1000.0,
                static_cast<unsigned long long>(kernel().vblank_count()),
                static_cast<unsigned long long>(now - kernel().last_vblank_us()),
                thread != nullptr ? thread->name.c_str() : "?", call);
    if (value >= 0) std::printf(" %lld", static_cast<long long>(value));
    std::printf("\n");
    if (lines == limit) std::fflush(stdout);
}

constexpr std::uint32_t kEdramBase = 0x04000000u;
constexpr std::uint32_t kEdramSize = 0x00200000u;
constexpr std::uint32_t kAudioSampleRate = 44'100u;

struct DisplayState {
    std::uint32_t mode{};
    std::uint32_t width{480u};
    std::uint32_t height{272u};
    std::uint32_t framebuffer{};
    std::uint32_t buffer_width{};
    std::uint32_t pixel_format{};
};

struct GeCallback {
    std::uint32_t signal_function{};
    std::uint32_t signal_argument{};
    std::uint32_t finish_function{};
    std::uint32_t finish_argument{};
};

struct GeList {
    std::uint32_t pc{};
    std::uint32_t stall{};
    std::int32_t callback{-1};
    bool done{};
};

struct AudioChannel {
    bool reserved{};
    std::uint32_t samples{};
    std::uint32_t format{};
    // Virtual time at which everything handed to this channel has finished
    // playing, and the host ring position its next frames are mixed at.
    std::uint64_t queued_until_us{};
    std::uint64_t cursor{};
};

struct MediaState {
    DisplayState display;
    std::uint32_t ctrl_cycle{};
    std::uint32_t ctrl_mode{};
    std::map<std::int32_t, GeCallback> ge_callbacks;
    std::int32_t next_ge_callback{};
    std::map<std::uint32_t, GeList> ge_lists;
    std::uint32_t next_ge_list{1u};
    std::array<AudioChannel, 8> audio{};
    // The Output2 channel, reserved through sceAudioOutput2Reserve rather than
    // chosen by the guest. -1 when it has not been reserved.
    std::int32_t output2_channel{-1};
    gpu::GeState ge;
#if defined(PORTABLEKIT_HAS_RENDERER)
    std::unique_ptr<gpu::VulkanRenderer> renderer;
#endif
};

MediaState &media() {
    static MediaState state;
    return state;
}

// Runs a display list: the GE state machine produces draw calls for the
// renderer and raises the guest's signal/finish callbacks in interrupt context.
// A GE block transfer, row by row. A source in a framebuffer the renderer drew
// is written back to guest memory first: the game copies the last frame of a
// hunt this way and textures the quest reward screen's background from it.
void block_transfer(Runtime &rt, const gpu::BlockTransfer &transfer) {
    psprecomp::GuestMemory &memory = rt.memory();
#if defined(PORTABLEKIT_HAS_RENDERER)
    if (media().renderer && media().renderer->available())
        media().renderer->read_back_framebuffer(transfer.source, memory);
#endif
    const std::size_t row_bytes = static_cast<std::size_t>(transfer.width) * transfer.bytes_per_pixel;
    for (std::uint32_t row = 0; row < transfer.height; ++row) {
        const std::uint32_t from =
            transfer.source +
            ((transfer.source_y + row) * transfer.source_stride + transfer.source_x) * transfer.bytes_per_pixel;
        const std::uint32_t to = transfer.destination + ((transfer.destination_y + row) * transfer.destination_stride +
                                                          transfer.destination_x) *
                                                             transfer.bytes_per_pixel;
        const std::uint8_t *source = memory.raw_pointer(from, row_bytes);
        std::uint8_t *destination = memory.raw_pointer(to, row_bytes);
        if (source == nullptr || destination == nullptr) {
            log_once("ge-transfer-range", "[ge] block transfer outside guest memory skipped");
            return;
        }
        std::memmove(destination, source, row_bytes);
    }
#if defined(PORTABLEKIT_HAS_RENDERER)
    // Textures already looked up in this list may have changed.
    if (media().renderer && media().renderer->available()) media().renderer->begin_display_list();
#endif
}

void run_ge_list(Runtime &rt, std::uint32_t id) {
    auto found = media().ge_lists.find(id);
    if (found == media().ge_lists.end()) return;
    const perf::Clock::time_point start = perf::Clock::now();
    GeList &list = found->second;
    const GeCallback *callback = nullptr;
    if (const auto cb = media().ge_callbacks.find(list.callback); cb != media().ge_callbacks.end())
        callback = &cb->second;

    media().ge.set_signal_sink([callback](std::uint32_t signal, std::uint32_t pc) {
        if (callback == nullptr) return;
        const bool finish = (signal & 0x10000u) != 0u;
        const std::uint32_t function = finish ? callback->finish_function : callback->signal_function;
        if (function == 0u) return;
        InterruptCall call{};
        call.function = function;
        call.arguments = {signal & 0xFFFFu, finish ? callback->finish_argument : callback->signal_argument, pc, 0u};
        kernel().queue_interrupt(std::move(call));
    });
#if defined(PORTABLEKIT_HAS_RENDERER)
    if (media().renderer && media().renderer->available()) {
        gpu::VulkanRenderer &renderer = *media().renderer;
        const psprecomp::GuestMemory &memory = rt.memory();
        renderer.begin_display_list();
        media().ge.set_raw_vertices(renderer.gpu_decode(), renderer.check_gpu_decode());
        media().ge.set_draw_sink([&renderer, &memory](const gpu::DrawCall &call) { renderer.submit(call, memory); });
    }
#endif
    media().ge.set_transfer_sink([&rt](const gpu::BlockTransfer &transfer) { block_transfer(rt, transfer); });

    bool finished = false;
    try {
        const perf::SplitScope split(perf::Split::Lists);
        list.pc = media().ge.execute(rt.memory(), list.pc, list.stall, finished);
    } catch (const psprecomp::Error &error) {
        // A malformed list must not take the whole run down: drop it and carry on.
        log_once("ge-list-error", std::string("[ge] display list aborted: ") + error.what());
        finished = true;
    }
    list.done = finished;
    media().ge.set_draw_sink(nullptr);
    media().ge.set_transfer_sink(nullptr);
    perf::add_render_time(perf::Clock::now() - start);
}

#if defined(PORTABLEKIT_HAS_RENDERER)
// The mouse's motion since the previous pump, as degrees for the camera
// layer. Added after the flip, so whoever drives the camera takes it in the
// update this frame leads to.
void feed_mouse(gpu::VulkanRenderer &renderer) {
    const settings::Settings &s = settings::current();
    // A drag on the touch screen: Touch camera speed degrees for the screen's
    // height, slowed while aiming as the mouse is.
    const gpu::MouseMotion drag = renderer.take_touch_motion();
    if (drag.x != 0.0f || drag.y != 0.0f) {
        const float scale = camera::game_camera_degrees_per_second() / std::max(s.camera_speed, 1.0f);
        const float degrees = s.touch_camera_speed * scale;
        camera::add_motion(camera::Source::Touch, drag.x * degrees, drag.y * degrees);
    }
    const gpu::MouseMotion motion = renderer.take_mouse_motion();
    if (motion.x == 0.0f && motion.y == 0.0f) return;
    // While a bow or a bowgun aims, Aim speed's share of Camera speed, as
    // for the stick.
    const float scale = camera::game_camera_degrees_per_second() / std::max(s.camera_speed, 1.0f);
    const input::MouseTurn turn =
        input::mouse_turn(motion.x, motion.y, s.mouse_sensitivity, s.invert_mouse_x, s.invert_mouse_y, scale);
    camera::add_motion(camera::Source::Mouse, turn.yaw, turn.pitch);
    static const bool trace = portablekit::env("TRACE_PAD") != nullptr;
    if (trace)
        std::cout << "[pad] mouse " << motion.x << "," << motion.y << " -> " << turn.yaw << "," << turn.pitch
                  << " degrees" << std::endl;
}
#endif

// The emulated time of the vblank the frame being flipped started from. A
// game at 30 frames a second starts a frame every other vblank, when its
// vblank handler has counted two since the last one; the flip comes when the
// frame's code has run, and on a slower machine that is often after the
// vblank between, so the latest vblank is not the frame's own. Frames are
// kept on a grid of the game's frame (GameProfile::frame_vblanks) from the
// one before: the latest start on that grid not after the latest vblank. A
// flip that comes before a whole step, or two steps late, starts the grid
// again at its latest vblank.
std::uint64_t frame_start_us(std::uint64_t latest_vblank_us) {
    static std::uint64_t previous = 0u;
    static bool known = false;
    static const std::uint64_t kStep = std::max<std::uint64_t>(portablekit::game().frame_vblanks, 1u) * kVBlankPeriodUs;
    std::uint64_t start = latest_vblank_us;
    if (known && latest_vblank_us >= previous + kStep && latest_vblank_us < previous + 3u * kStep)
        start = previous + kStep * ((latest_vblank_us - previous) / kStep);
    previous = start;
    known = true;
    return start;
}

void present_frame(Runtime &rt) {
    load_trace::note_flip();
    // Overlays are swapped between frames; re-check before drawing the next one.
    revalidate_overlays(rt);
#if defined(PORTABLEKIT_HAS_RENDERER)
    if (!media().renderer || !media().renderer->available()) {
        perf::end_frame(kernel().now_us());
        return;
    }
    gpu::VulkanRenderer &renderer = *media().renderer;
    // The guest passes a VRAM offset when the high byte is zero.
    const std::uint32_t address = (media().display.framebuffer & 0xFF000000u) == 0u
                                      ? (media().display.framebuffer | 0x04000000u)
                                      : media().display.framebuffer;
    const perf::Clock::time_point present_start = perf::Clock::now();
    // The GE only draws into VRAM, so a framebuffer in main memory was
    // written by the CPU (the movie player's sceJpegCsc) and has to be shown
    // from memory.
    constexpr std::uint32_t kPixelFormat8888 = 3u;
    const DisplayState &display = media().display;
    if ((address & 0x1F000000u) != kEdramBase && display.pixel_format == kPixelFormat8888) {
        const std::size_t bytes = static_cast<std::size_t>(display.buffer_width) * display.height * 4u;
        renderer.upload_frame(address, rt.memory().raw_pointer(address, bytes), display.width, display.height,
                              display.buffer_width);
    }
    ui::draw_over_game();
    renderer.write_back_frame(rt.memory());
    // A load running fast flips far more often than the display refreshes.
    renderer.set_fast_forward(fast_loading::active());
    // The real time the frame stands for, which frame interpolation spaces
    // its presents by: that of the vblank the game's frame started from.
    const bool presented = renderer.present(address, kernel().real_time_of(frame_start_us(kernel().last_vblank_us())));
    // The frame's camera has been measured by now, so the hunt for the guest
    // variables behind it can compare RAM against it.
    probe::camera_frame(rt, media().ge.view_matrix_source());
    // The game's flip is the camera's frame: the camera update runs once
    // between two flips, however many presents interpolation adds. The stick is
    // already shaped and inverted by the input layer; its rate becomes degrees
    // over the real time since the previous flip.
    {
        static perf::Clock::time_point previous_flip = present_start;
        const float seconds = std::chrono::duration<float>(present_start - previous_flip).count();
        previous_flip = present_start;
        camera::set_rate(camera::Source::Stick, (static_cast<int>(renderer.pad().right_x) - 0x80) / 127.0f,
                         (static_cast<int>(renderer.pad().right_y) - 0x80) / 127.0f);
        camera::game_camera_frame(rt);
        // The view's shape follows the picture's: the game builds its next
        // projection with the aspect ratio of the target it will draw into.
        camera::game_aspect_frame(rt, renderer.game_aspect());
        camera::advance(seconds, camera::game_camera_degrees_per_second());
    }
    perf::add_render_time(perf::Clock::now() - present_start);
    // A frame ends when its image has been handed to the swapchain, or with
    // frame interpolation when the presents after it are scheduled.
    perf::end_frame(kernel().now_us(), presented);

    // Optional frame capture, independent of the window.
    static const char *screenshot_dir = portablekit::env("SCREENSHOT_DIR");
    static const std::uint64_t screenshot_every = [] {
        const char *text = portablekit::env("SCREENSHOT_EVERY");
        return text != nullptr ? std::strtoull(text, nullptr, 10) : 60ull;
    }();
    if (screenshot_dir != nullptr && screenshot_every != 0u &&
        renderer.frames_presented() % screenshot_every == 0u) {
        const std::string path = std::string(screenshot_dir) + "/frame_" +
                                 std::to_string(renderer.frames_presented()) + ".bmp";
        if (renderer.capture_frame(path)) {
            std::cout << "[render] frame " << renderer.frames_presented() << " (" << renderer.draws_submitted()
                      << " draws) -> " << path << "\n";
        } else {
            // A capture that fails silently reads as "the game drew nothing",
            // which is a different problem entirely and sends you looking in
            // the wrong place.
            log_once("capture-failed", "[render] cannot capture the frame to " + path +
                                           "; no further capture is reported");
        }
    }
    const bool window_open = renderer.pump_events();
    feed_mouse(renderer);
    if (!window_open) {
        rt.stop("window closed");
    } else if (ui::take_quit_request()) {
        rt.stop("quit from the menu");
    } else if (!ui::menu_over_game() && ui::menu_requested()) {
        if (ui::menu_pauses()) {
            // The menu pauses the game: guest code and emulated time stand
            // still while it runs in here, and the device stops playing.
            renderer.pause_interpolation();
            audio::AudioSink::instance().set_paused(true);
            const bool keep_playing = ui::run_menu();
            audio::AudioSink::instance().set_paused(false);
            // Resume at normal speed rather than racing to make up the pause,
            // and keep the pause out of the frame statistics.
            kernel().resync_real_time();
            perf::restart_measurement();
            if (!keep_playing) rt.stop("quit from the menu");
        } else {
            // The game keeps running, sound and pacing included; the menu is
            // drawn over each frame and takes all input until it closes.
            ui::open_menu_over_game();
        }
    }
#else
    (void)rt;
    perf::end_frame(kernel().now_us());
#endif
}

// One sample of the pad, as the game reads it.
struct CtrlSample {
    std::uint32_t buttons = 0u;
    std::uint8_t analog_x = 0x80u;
    std::uint8_t analog_y = 0x80u;
    std::uint8_t right_x = 0x80u;
    std::uint8_t right_y = 0x80u;
};

CtrlSample sample_ctrl() {
    CtrlSample sample;
#if defined(PORTABLEKIT_HAS_RENDERER)
    if (media().renderer && media().renderer->available()) {
        // The pad as it is now, not as it was at the last flip (#8).
        // <prefix>_PAD_AT_FLIP keeps the state of the flip, as before.
        static const bool at_flip = portablekit::env("PAD_AT_FLIP") != nullptr;
        if (!at_flip) media().renderer->sample_pad();
        const gpu::PadState pad = media().renderer->pad();
        sample.buttons = pad.buttons;
        fast_loading::note_buttons(sample.buttons != 0u);
        sample.analog_x = pad.analog_x;
        sample.analog_y = pad.analog_y;
        sample.right_x = pad.right_x;
        sample.right_y = pad.right_y;
        // Keep the game's digital commands neutral while the analog
        // camera consumes these axes: otherwise the game's one-shot
        // vertical command fires from the same push and glides the camera
        // against what the port is doing. The physical D-pad stays available.
        if (camera::game_camera_driving()) {
            sample.right_x = 0x80u;
            sample.right_y = 0x80u;
        } else if (camera::game_camera_aim_boost()) {
            // Past the dead zone, any push reaches the game at full
            // length in the same direction, so its aim steps and the
            // driver decides how far. With the stick idle, the mouse's
            // direction stands in for it.
            int dx = static_cast<int>(sample.right_x) - 0x80;
            int dy = static_cast<int>(sample.right_y) - 0x80;
            if (dx == 0 && dy == 0) {
                if (const auto mouse = camera::game_camera_mouse_aim()) {
                    dx = static_cast<int>(std::lround(mouse->x * 127.0f));
                    dy = static_cast<int>(std::lround(mouse->y * 127.0f));
                }
            }
            const float length = std::sqrt(static_cast<float>(dx * dx + dy * dy));
            if (length > 0.0f) {
                sample.right_x = static_cast<std::uint8_t>(std::clamp(0x80 + static_cast<int>(std::lround(dx * 127.0f / length)), 0, 255));
                sample.right_y = static_cast<std::uint8_t>(std::clamp(0x80 + static_cast<int>(std::lround(dy * 127.0f / length)), 0, 255));
            }
        } else if (sample.right_x == 0x80u && sample.right_y == 0x80u &&
                   settings::current().right_stick == settings::RightStick::Camera) {
            // The game's own camera: the mouse switches its turn on while
            // it moves sideways. Not in the D-pad mode, where the same
            // bits move cursors in the game's menus.
            if (const int turn = camera::game_camera_mouse_stock_turn()) sample.right_x = turn > 0 ? 0xFFu : 0x01u;
        }
    }
#endif
    return sample;
}

// Writes `count` SceCtrlData entries of the same sample.
void write_ctrl_buffer(psprecomp::GuestMemory &memory, std::uint32_t address, std::uint32_t count,
                       const CtrlSample &sample) {
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t entry = address + i * 16u;
        memory.store32(entry, static_cast<std::uint32_t>(kernel().now_us()));
        memory.store32(entry + 4u, sample.buttons);
        memory.store8(entry + 8u, sample.analog_x);
        memory.store8(entry + 9u, sample.analog_y);
        // Bytes 10 and 11 are the HD release's second stick, not padding.
        // Leaving them zero reads as a full diagonal deflection and turns
        // the camera every frame; 0x80 is the centre the guest tests for.
        memory.store8(entry + 10u, sample.right_x);
        memory.store8(entry + 11u, sample.right_y);
        for (std::uint32_t j = 12u; j < 16u; ++j) memory.store8(entry + j, 0u);
    }
}

// The latch: which buttons went down and which came up since the game last
// read it. The PSP samples the pad once a vblank and accumulates the edges
// between reads, so a press shorter than the game's frame is still seen.
struct CtrlLatch {
    bool sampling = false;
    std::uint32_t previous = 0u;
    std::uint32_t make = 0u;
    std::uint32_t release = 0u;
    std::uint32_t samples = 0u;
};

CtrlLatch &ctrl_latch() {
    static CtrlLatch latch;
    return latch;
}

void sample_latch() {
    CtrlLatch &latch = ctrl_latch();
    const std::uint32_t now = sample_ctrl().buttons;
    latch.make |= now & ~latch.previous;
    latch.release |= latch.previous & ~now;
    latch.previous = now;
    ++latch.samples;
}

// SceCtrlLatch: make, break, press, release. Reading it starts the next
// accumulation. Returns the number of samples since the last read.
std::uint32_t read_latch(psprecomp::GuestMemory &memory, std::uint32_t address) {
    CtrlLatch &latch = ctrl_latch();
    if (!latch.sampling) {
        // Start sampling the first time a game asks; until then there were
        // no edges to see.
        latch.sampling = true;
        latch.previous = sample_ctrl().buttons;
        kernel().add_vblank_hook(sample_latch);
    }
    if (address != 0u) {
        memory.store32(address, latch.make);
        memory.store32(address + 4u, latch.release);
        memory.store32(address + 8u, latch.previous);
        memory.store32(address + 12u, ~latch.previous);
    }
    const std::uint32_t samples = latch.samples;
    latch.make = 0u;
    latch.release = 0u;
    latch.samples = 0u;
    return samples;
}

bool ctrl_reads_wait() {
    static const bool value = portablekit::env("CTRL_READ_WAITS") != nullptr;
    return value;
}

// Samples the pad has taken since the game last read the buffer: one a
// vblank, at most the 64 the PSP's ring holds.
std::uint32_t &ctrl_unread() {
    static std::uint32_t unread = 0u;
    return unread;
}

// Set while a read waits for the next sample: that sample is the read's.
bool &ctrl_read_waiting() {
    static bool waiting = false;
    return waiting;
}

// Takes up to `count` of the samples not read yet, and returns how many.
// Counting starts at the first read, as sampling does on the PSP once a game
// asks for it; until then a read waits for the next vblank.
std::uint32_t take_ctrl_samples(std::uint32_t count) {
    static bool counting = false;
    if (!counting) {
        counting = true;
        kernel().add_vblank_hook([] {
            if (std::exchange(ctrl_read_waiting(), false)) return;
            ctrl_unread() = std::min<std::uint32_t>(ctrl_unread() + 1u, 64u);
        });
    }
    if (ctrl_reads_wait()) return 0u;
    const std::uint32_t taken = std::min(ctrl_unread(), count);
    ctrl_unread() = 0u;
    return taken;
}

void register_display_ctrl(HleRegistrar &hle) {
    hle.add("sceDisplay", "sceDisplaySetMode", [](Runtime &, AllegrexContext &ctx) {
        media().display.mode = arg(ctx, 0);
        media().display.width = arg(ctx, 1);
        media().display.height = arg(ctx, 2);
        kernel().finish(ctx, 0u);
    });
    // The guest flipping the framebuffer is the end of a frame.
    hle.add("sceDisplay", "sceDisplaySetFrameBuf", [](Runtime &rt, AllegrexContext &ctx) {
        media().display.framebuffer = arg(ctx, 0);
        media().display.buffer_width = arg(ctx, 1);
        media().display.pixel_format = arg(ctx, 2);
        trace_pacing("sceDisplaySetFrameBuf");
        present_frame(rt);
        kernel().finish(ctx, 0u);
    });

    // When pad input cancels the console's idle timer: there is none here.
    hle.add("sceCtrl", "sceCtrlSetIdleCancelThreshold", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, 0u);
    });
    hle.add("sceCtrl", "sceCtrlSetSamplingCycle", [](Runtime &, AllegrexContext &ctx) {
        const std::uint32_t previous = media().ctrl_cycle;
        media().ctrl_cycle = arg(ctx, 0);
        kernel().finish(ctx, previous);
    });
    hle.add("sceCtrl", "sceCtrlSetSamplingMode", [](Runtime &, AllegrexContext &ctx) {
        const std::uint32_t previous = media().ctrl_mode;
        media().ctrl_mode = arg(ctx, 0);
        // The mode is recorded but not acted on: the game subtracts 128 from Lx
        // unconditionally, so reporting the neutral 0x80 is right in both modes.
        if (portablekit::env("TRACE_PAD") != nullptr && media().ctrl_mode != previous)
            std::cout << "[pad] sceCtrlSetSamplingMode " << media().ctrl_mode << "\n";
        kernel().finish(ctx, previous);
    });
    // Reading the controller buffer returns the samples taken since the last
    // read, and waits for the next one only when there are none. The PSP
    // samples the pad once a vblank into a ring the read drains, so a game
    // that reads once a frame, after a vblank has passed, is not held up by
    // it: Purun's main loop reads it straight after the flip, and waiting
    // there cost it a vblank a frame. <prefix>_CTRL_READ_WAITS=1 always
    // waits for the next vblank, as the framework did before.
    hle.add("sceCtrl", "sceCtrlReadBufferPositive", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t count = std::clamp<std::uint32_t>(arg(ctx, 1), 1u, 64u);
        const std::uint32_t unread = take_ctrl_samples(count);
        trace_pacing("sceCtrlReadBufferPositive", unread);
        if (unread != 0u) {
            write_ctrl_buffer(rt.memory(), arg(ctx, 0), unread, sample_ctrl());
            kernel().finish(ctx, unread);
            return;
        }
        write_ctrl_buffer(rt.memory(), arg(ctx, 0), count, sample_ctrl());
        ctrl_read_waiting() = true;
        WaitState wait{};
        wait.type = WaitType::VBlank;
        kernel().block(ctx, wait, count);
    });
    // Peeking returns the latest sample at once.
    hle.add("sceCtrl", "sceCtrlPeekBufferPositive", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t count = std::clamp<std::uint32_t>(arg(ctx, 1), 1u, 64u);
        write_ctrl_buffer(rt.memory(), arg(ctx, 0), count, sample_ctrl());
        kernel().finish(ctx, count);
    });
    hle.add("sceCtrl", "sceCtrlPeekLatch", [](Runtime &rt, AllegrexContext &ctx) {
        kernel().finish(ctx, read_latch(rt.memory(), arg(ctx, 0)));
    });
    // Reading the latch returns the edges gathered since the last read and
    // starts gathering again; unlike reading the buffer it never waits.
    // Waiting here cost Purun, which reads the latch once a frame after the
    // buffer, a second vblank a frame (20 frames a second where its loop
    // allows 60). <prefix>_CTRL_READ_WAITS=1 waits for the next vblank, as
    // the framework did before.
    hle.add("sceCtrl", "sceCtrlReadLatch", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t samples = read_latch(rt.memory(), arg(ctx, 0));
        trace_pacing("sceCtrlReadLatch", samples);
        if (!ctrl_reads_wait()) {
            kernel().finish(ctx, samples);
            return;
        }
        WaitState wait{};
        wait.type = WaitType::VBlank;
        kernel().block(ctx, wait, std::max(samples, 1u));
    });
}

// How long the display's vertical blank lasts at the start of each 16.683 ms
// period: 14 of its 286 lines.
constexpr std::uint64_t kVBlankDurationUs = kVBlankPeriodUs * 14u / 286u;

// Waiting for the display to start its vertical blank, which is how a game
// paces its frame loop. Without these the loop spins: a stub returns at once,
// the game draws again, and nothing else ever gets the processor.
void register_vblank_waits(HleRegistrar &hle) {
    const auto wait_for_vblank = [](Runtime &, AllegrexContext &ctx) {
        trace_pacing("sceDisplayWaitVblankStart");
        WaitState wait{};
        wait.type = WaitType::VBlank;
        kernel().block(ctx, wait, 0u);
    };
    hle.add("sceDisplay", "sceDisplayWaitVblankStart", wait_for_vblank);
    // The CB forms also run the thread's pending callbacks. The kernel runs
    // them at the next scheduling point either way, so they wait the same.
    hle.add("sceDisplay", "sceDisplayWaitVblankStartCB", wait_for_vblank);
    // MultiCB waits for that many vblanks: the wait's object counts them
    // down (Kernel::on_vblank). Tenkawa waits two at a time while it loads a
    // fight.
    hle.add("sceDisplay", "sceDisplayWaitVblankStartMultiCB", [](Runtime &, AllegrexContext &ctx) {
        trace_pacing("sceDisplayWaitVblankStartMultiCB", arg(ctx, 0));
        WaitState wait{};
        wait.type = WaitType::VBlank;
        wait.object = static_cast<SceUID>(std::max<std::uint32_t>(arg(ctx, 0), 1u));
        kernel().block(ctx, wait, 0u);
    });
    hle.add("sceDisplay", "sceDisplayWaitVblank", [](Runtime &, AllegrexContext &ctx) {
        trace_pacing("sceDisplayWaitVblank");
        static const bool returns = portablekit::env("VBLANK_WAIT_RETURNS") != nullptr;
        if (returns || kernel().now_us() - kernel().last_vblank_us() < kVBlankDurationUs) {
            kernel().finish(ctx, 1u);
            return;
        }
        WaitState wait{};
        wait.type = WaitType::VBlank;
        kernel().block(ctx, wait, 0u);
    });
    hle.add("sceDisplay", "sceDisplayGetVcount", [](Runtime &, AllegrexContext &ctx) {
        trace_pacing("sceDisplayGetVcount", static_cast<std::int64_t>(kernel().vblank_count()));
        kernel().finish(ctx, static_cast<std::uint32_t>(kernel().vblank_count()));
    });
}

void register_ge(HleRegistrar &hle) {
    hle.add("sceGe_user", "sceGeEdramGetAddr", [](Runtime &, AllegrexContext &ctx) { kernel().finish(ctx, kEdramBase); });
    hle.add("sceGe_user", "sceGeEdramGetSize", [](Runtime &, AllegrexContext &ctx) { kernel().finish(ctx, kEdramSize); });
    hle.add("sceGe_user", "sceGeEdramSetAddrTranslation", [](Runtime &, AllegrexContext &ctx) { kernel().finish(ctx, 0u); });
    hle.add("sceGe_user", "sceGeSetCallback", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t data = arg(ctx, 0);
        auto &memory = rt.memory();
        const std::int32_t id = media().next_ge_callback++;
        media().ge_callbacks[id] = GeCallback{memory.load32(data), memory.load32(data + 4u), memory.load32(data + 8u),
                                              memory.load32(data + 12u)};
        kernel().finish(ctx, static_cast<std::uint32_t>(id));
    });
    hle.add("sceGe_user", "sceGeUnsetCallback", [](Runtime &, AllegrexContext &ctx) {
        media().ge_callbacks.erase(static_cast<std::int32_t>(arg(ctx, 0)));
        kernel().finish(ctx, 0u);
    });
    const auto enqueue = [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t id = media().next_ge_list++;
        GeList list{};
        list.pc = arg(ctx, 0) & 0x0FFFFFFFu;
        list.stall = arg(ctx, 1) & 0x0FFFFFFFu;
        list.callback = static_cast<std::int32_t>(arg(ctx, 2));
        media().ge_lists[id] = std::move(list);
        perf::count_display_list();
        run_ge_list(rt, id);
        kernel().finish(ctx, id);
    };
    hle.add("sceGe_user", "sceGeListEnQueue", enqueue);
    hle.add("sceGe_user", "sceGeListEnQueueHead", enqueue);
    hle.add("sceGe_user", "sceGeListUpdateStallAddr", [](Runtime &rt, AllegrexContext &ctx) {
        auto found = media().ge_lists.find(arg(ctx, 0));
        if (found == media().ge_lists.end()) {
            kernel().finish(ctx, error::kIllegalArgument);
            return;
        }
        found->second.stall = arg(ctx, 1) & 0x0FFFFFFFu;
        run_ge_list(rt, arg(ctx, 0));
        kernel().finish(ctx, 0u);
    });
    hle.add("sceGe_user", "sceGeListSync", [](Runtime &, AllegrexContext &ctx) {
        auto found = media().ge_lists.find(arg(ctx, 0));
        // 0 = completed, 2 = still stalled (drawing).
        const std::uint32_t state = found == media().ge_lists.end() || found->second.done ? 0u : 2u;
        kernel().finish(ctx, state);
    });
    hle.add("sceGe_user", "sceGeDrawSync", [](Runtime &, AllegrexContext &ctx) {
        std::erase_if(media().ge_lists, [](const auto &item) { return item.second.done; });
        kernel().finish(ctx, 0u);
    });
    hle.add("sceGe_user", "sceGeBreak", [](Runtime &, AllegrexContext &ctx) { kernel().finish(ctx, 0u); });
    hle.add("sceGe_user", "sceGeContinue", [](Runtime &, AllegrexContext &ctx) { kernel().finish(ctx, 0u); });
}

// sceAudioOutput*Blocking returns once the previously submitted buffer has
// drained, which is what paces the guest's audio thread: the channel's queue
// is tracked in virtual time and the thread waits on the kernel clock, never
// on the host. The samples themselves go straight to the sink.
void audio_output(Runtime &rt, AllegrexContext &ctx) {
    const std::uint32_t channel = arg(ctx, 0);
    if (channel >= media().audio.size()) {
        kernel().finish(ctx, 0x80260002u);
        return;
    }
    AudioChannel &state = media().audio[channel];
    const std::uint32_t left = arg(ctx, 1);
    const std::uint32_t right = arg(ctx, 2);
    const std::uint32_t buffer = arg(ctx, 3);
    const std::uint32_t frames = state.samples;
    // Format 0x10 is mono: one sample per frame instead of a stereo pair.
    const bool mono = (state.format & 0x10u) != 0u;
    const std::size_t words = frames * (mono ? 1u : 2u);

    if (buffer != 0u && frames != 0u) {
        static std::vector<std::int16_t> staging;
        staging.resize(frames * 2u);
        if (const std::uint8_t *source = rt.memory().raw_pointer(buffer, words * 2u)) {
            for (std::uint32_t frame = 0; frame < frames; ++frame) {
                const std::size_t index = mono ? frame : frame * 2u;
                const auto sample = static_cast<std::int16_t>(source[index * 2u] | (source[index * 2u + 1u] << 8));
                staging[frame * 2u] = sample;
                staging[frame * 2u + 1u] = mono ? sample
                                                : static_cast<std::int16_t>(source[(index + 1u) * 2u] |
                                                                            (source[(index + 1u) * 2u + 1u] << 8));
            }
            // How loud the buffer is after the channel's volume, worked out
            // as the sink mixes it (0x8000 is full volume): 0 means the sink
            // would add nothing but zeros.
            const std::int32_t gains[2] = {static_cast<std::int32_t>(std::min<std::uint32_t>(left, 0x8000u)),
                                           static_cast<std::int32_t>(std::min<std::uint32_t>(right, 0x8000u))};
            int peak = 0;
            for (std::size_t i = 0; i < frames * 2u; ++i)
                peak = std::max(peak, std::abs((static_cast<std::int32_t>(staging[i]) * gains[i & 1u]) >> 15));
            load_trace::note_audio_peak(peak);
            // While a load runs faster than real time its silence is dropped:
            // played, it would pile up faster than the device plays it. The
            // channel's cursor stays where it was and catches up with the
            // device when sound comes back.
            if (!fast_loading::note_audio(peak))
                audio::AudioSink::instance().mix(state.cursor, staging.data(), frames, left, right);
        } else {
            log_once("audio-buffer", "[audio] output buffer is not a single mapped range; dropping it");
        }
    }

    const std::uint64_t now = kernel().now_us();
    if (state.queued_until_us < now) state.queued_until_us = now;
    const std::uint64_t previous_end = state.queued_until_us;
    state.queued_until_us += static_cast<std::uint64_t>(frames) * 1'000'000u / kAudioSampleRate;
    if (previous_end > now)
        kernel().delay_current(ctx, previous_end - now, frames);
    else
        kernel().finish(ctx, frames);
}

// __sceSasCore renders one grain into a guest buffer; the guest then hands that
// buffer to a sceAudio channel itself, so nothing here reaches the sink.
void sas_render(Runtime &rt, std::uint32_t core, std::uint32_t output, bool mix,
                std::uint32_t left_volume, std::uint32_t right_volume) {
    audio::SasCore &sas = audio::sas_core(core);
    const std::size_t frames = sas.grain();
    static std::vector<std::int16_t> staging;
    staging.resize(frames * 2u);
    sas.render(rt.memory(), staging.data(), frames);

    std::uint8_t *destination = rt.memory().raw_pointer(output, frames * 4u);
    if (destination == nullptr) {
        log_once("sas-output", "[sas] output buffer is not a single mapped range; dropping the grain");
        return;
    }
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::size_t side = 0; side < 2u; ++side) {
            const std::size_t index = frame * 2u + side;
            std::int32_t value = staging[index];
            if (mix) {
                const std::uint32_t gain = side == 0u ? left_volume : right_volume;
                value = (value * static_cast<std::int32_t>(std::min(gain, 0x1000u))) >> 12;
                value += static_cast<std::int16_t>(destination[index * 2u] | (destination[index * 2u + 1u] << 8));
                value = std::clamp(value, -32768, 32767);
            }
            destination[index * 2u] = static_cast<std::uint8_t>(value);
            destination[index * 2u + 1u] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(value) >> 8u);
        }
    }
}

// <prefix>_TRACE_SAS: voices set, keyed on and off, and each change of the
// end flags the game polls.
bool trace_sas() {
    static const bool enabled = portablekit::env("TRACE_SAS") != nullptr;
    return enabled;
}

void register_audio(HleRegistrar &hle) {
    audio::AudioSink::instance().initialize();

    hle.add("sceAudio", "sceAudioChReserve", [](Runtime &, AllegrexContext &ctx) {
        auto channel = static_cast<std::int32_t>(arg(ctx, 0));
        auto &channels = media().audio;
        if (channel < 0) {
            channel = -1;
            for (std::size_t i = 0; i < channels.size(); ++i) {
                if (!channels[i].reserved) {
                    channel = static_cast<std::int32_t>(i);
                    break;
                }
            }
        }
        if (channel < 0 || channel >= static_cast<std::int32_t>(channels.size()) || channels[static_cast<std::size_t>(channel)].reserved) {
            kernel().finish(ctx, 0x80260002u);
            return;
        }
        channels[static_cast<std::size_t>(channel)] = AudioChannel{true, arg(ctx, 1), arg(ctx, 2), 0u, 0u};
        kernel().finish(ctx, static_cast<std::uint32_t>(channel));
    });
    hle.add("sceAudio", "sceAudioChRelease", [](Runtime &, AllegrexContext &ctx) {
        if (arg(ctx, 0) < media().audio.size()) media().audio[arg(ctx, 0)].reserved = false;
        kernel().finish(ctx, 0u);
    });
    hle.add("sceAudio", "sceAudioSetChannelDataLen", [](Runtime &, AllegrexContext &ctx) {
        if (arg(ctx, 0) < media().audio.size()) media().audio[arg(ctx, 0)].samples = arg(ctx, 1);
        kernel().finish(ctx, 0u);
    });
    hle.add("sceAudio", "sceAudioChangeChannelConfig", [](Runtime &, AllegrexContext &ctx) {
        if (arg(ctx, 0) < media().audio.size()) media().audio[arg(ctx, 0)].format = arg(ctx, 1);
        kernel().finish(ctx, 0u);
    });
    hle.add("sceAudio", "sceAudioChangeChannelVolume", [](Runtime &, AllegrexContext &ctx) { kernel().finish(ctx, 0u); });
    // How much of what the channel was given is still to play, in samples.
    hle.add("sceAudio", "sceAudioGetChannelRestLength", [](Runtime &, AllegrexContext &ctx) {
        const std::uint32_t channel = arg(ctx, 0);
        if (channel >= media().audio.size()) {
            kernel().finish(ctx, 0x80260002u);
            return;
        }
        const std::uint64_t now = kernel().now_us();
        const AudioChannel &state = media().audio[channel];
        const std::uint64_t remaining = state.queued_until_us > now ? state.queued_until_us - now : 0u;
        kernel().finish(ctx, static_cast<std::uint32_t>(remaining * kAudioSampleRate / 1'000'000u));
    });
    hle.add("sceAudio", "sceAudioOutputPannedBlocking", audio_output);
    hle.try_add("sceAudio", "sceAudioOutputPanned", audio_output);
    // sceAudioOutputBlocking(channel, volume, buffer): the same output with one
    // volume for both sides.
    hle.add("sceAudio", "sceAudioOutputBlocking", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t channel = arg(ctx, 0);
        const std::uint32_t volume = arg(ctx, 1);
        const std::uint32_t buffer = arg(ctx, 2);
        ctx.set_gpr(4, channel);
        ctx.set_gpr(5, volume);
        ctx.set_gpr(6, volume);
        ctx.set_gpr(7, buffer);
        audio_output(rt, ctx);
    });

    // Output2 is a single stereo channel a game reserves once and then feeds,
    // instead of picking a channel of its own. It is the same output
    // underneath, and the same pacing: the blocking form returns once the
    // previous buffer has drained, which is what stops a game's audio thread
    // from spinning at a priority above everything else.
    hle.add("sceAudio", "sceAudioOutput2Reserve", [](Runtime &, AllegrexContext &ctx) {
        auto &channels = media().audio;
        std::int32_t channel = media().output2_channel;
        if (channel < 0) {
            for (std::size_t i = 0; i < channels.size(); ++i) {
                if (!channels[i].reserved) {
                    channel = static_cast<std::int32_t>(i);
                    break;
                }
            }
        }
        if (channel < 0) {
            kernel().finish(ctx, 0x80260002u);
            return;
        }
        // Output2 is always stereo 16-bit; format 0 is the stereo pair.
        channels[static_cast<std::size_t>(channel)] = AudioChannel{true, arg(ctx, 0), 0u, 0u, 0u};
        media().output2_channel = channel;
        kernel().finish(ctx, 0u);
    });
    hle.add("sceAudio", "sceAudioOutput2Release", [](Runtime &, AllegrexContext &ctx) {
        if (media().output2_channel >= 0) {
            media().audio[static_cast<std::size_t>(media().output2_channel)].reserved = false;
            media().output2_channel = -1;
        }
        kernel().finish(ctx, 0u);
    });
    // sceAudioOutput2OutputBlocking(volume, buffer).
    hle.add("sceAudio", "sceAudioOutput2OutputBlocking", [](Runtime &rt, AllegrexContext &ctx) {
        if (media().output2_channel < 0) {
            kernel().finish(ctx, 0x80260002u);
            return;
        }
        const std::uint32_t volume = arg(ctx, 0);
        const std::uint32_t buffer = arg(ctx, 1);
        ctx.set_gpr(4, static_cast<std::uint32_t>(media().output2_channel));
        ctx.set_gpr(5, volume);
        ctx.set_gpr(6, volume);
        ctx.set_gpr(7, buffer);
        audio_output(rt, ctx);
    });

    hle.add("sceSasCore", "__sceSasInit", [](Runtime &, AllegrexContext &ctx) {
        audio::sas_core(arg(ctx, 0)).init(arg(ctx, 1), arg(ctx, 2), arg(ctx, 3));
        kernel().finish(ctx, 0u);
    });
    hle.add("sceSasCore", "__sceSasSetVoice", [](Runtime &, AllegrexContext &ctx) {
        if (trace_sas())
            std::cerr << "[sas] voice " << arg(ctx, 1) << " vag=" << psprecomp::hex32(arg(ctx, 2)) << " size=" << arg(ctx, 3)
                      << " loop=" << arg(ctx, 4) << "\n";
        audio::sas_core(arg(ctx, 0)).set_voice(arg(ctx, 1), arg(ctx, 2), arg(ctx, 3), arg(ctx, 4) != 0u);
        kernel().finish(ctx, 0u);
    });
    hle.try_add("sceSasCore", "__sceSasSetVoicePCM", [](Runtime &, AllegrexContext &ctx) {
        audio::sas_core(arg(ctx, 0)).set_voice_pcm(arg(ctx, 1), arg(ctx, 2), arg(ctx, 3),
                                                   static_cast<std::int32_t>(arg(ctx, 4)));
        kernel().finish(ctx, 0u);
    });
    // __sceSasSetNoise(core, voice, clock): the voice plays the noise
    // generator at that clock instead of a sample, from its next key on.
    hle.try_add("sceSasCore", "__sceSasSetNoise", [](Runtime &, AllegrexContext &ctx) {
        audio::sas_core(arg(ctx, 0)).set_noise(arg(ctx, 1), arg(ctx, 2));
        if (portablekit::env("TRACE_AUDIO") != nullptr)
            std::cout << "[sas] noise voice " << arg(ctx, 1) << " clock " << arg(ctx, 2) << "\n";
        kernel().finish(ctx, 0u);
    });
    hle.add("sceSasCore", "__sceSasSetPitch", [](Runtime &, AllegrexContext &ctx) {
        audio::sas_core(arg(ctx, 0)).set_pitch(arg(ctx, 1), arg(ctx, 2));
        kernel().finish(ctx, 0u);
    });
    hle.add("sceSasCore", "__sceSasSetVolume", [](Runtime &, AllegrexContext &ctx) {
        audio::sas_core(arg(ctx, 0)).set_volume(arg(ctx, 1), static_cast<std::int32_t>(arg(ctx, 2)),
                                                static_cast<std::int32_t>(arg(ctx, 3)));
        kernel().finish(ctx, 0u);
    });
    hle.add("sceSasCore", "__sceSasSetSimpleADSR", [](Runtime &, AllegrexContext &ctx) {
        audio::sas_core(arg(ctx, 0)).set_simple_adsr(arg(ctx, 1), arg(ctx, 2), arg(ctx, 3));
        kernel().finish(ctx, 0u);
    });
    hle.add("sceSasCore", "__sceSasSetKeyOn", [](Runtime &, AllegrexContext &ctx) {
        if (trace_sas()) std::cerr << "[sas] key on " << arg(ctx, 1) << "\n";
        audio::sas_core(arg(ctx, 0)).key_on(arg(ctx, 1));
        kernel().finish(ctx, 0u);
    });
    hle.add("sceSasCore", "__sceSasSetKeyOff", [](Runtime &, AllegrexContext &ctx) {
        if (trace_sas()) std::cerr << "[sas] key off " << arg(ctx, 1) << "\n";
        audio::sas_core(arg(ctx, 0)).key_off(arg(ctx, 1));
        kernel().finish(ctx, 0u);
    });
    hle.try_add("sceSasCore", "__sceSasSetPause", [](Runtime &, AllegrexContext &ctx) {
        audio::sas_core(arg(ctx, 0)).set_pause(arg(ctx, 1), arg(ctx, 2) != 0u);
        kernel().finish(ctx, 0u);
    });
    // __sceSasSetADSR(core, voice, flag, attack, decay, sustain, release) and
    // __sceSasSetADSRmode with the same arguments: the explicit envelope's
    // rates and curves, the stages `flag` names. __sceSasSetSL(core, voice,
    // level): its sustain level.
    hle.try_add("sceSasCore", "__sceSasSetADSR", [](Runtime &, AllegrexContext &ctx) {
        const std::array<std::int32_t, 4> rates{static_cast<std::int32_t>(arg(ctx, 3)), static_cast<std::int32_t>(arg(ctx, 4)),
                                                static_cast<std::int32_t>(arg(ctx, 5)), static_cast<std::int32_t>(arg(ctx, 6))};
        kernel().finish(ctx, audio::sas_core(arg(ctx, 0)).set_adsr_rates(arg(ctx, 1), arg(ctx, 2), rates));
    });
    hle.try_add("sceSasCore", "__sceSasSetADSRmode", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, audio::sas_core(arg(ctx, 0)).set_adsr_curves(
                                 arg(ctx, 1), arg(ctx, 2), {arg(ctx, 3), arg(ctx, 4), arg(ctx, 5), arg(ctx, 6)}));
    });
    hle.try_add("sceSasCore", "__sceSasSetSL", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, audio::sas_core(arg(ctx, 0)).set_sustain_level(arg(ctx, 1), static_cast<std::int32_t>(arg(ctx, 2))));
    });
    // __sceSasGetAllEnvelopeHeights(core, heights): 32 words, one per voice.
    hle.try_add("sceSasCore", "__sceSasGetAllEnvelopeHeights", [](Runtime &rt, AllegrexContext &ctx) {
        auto &core = audio::sas_core(arg(ctx, 0));
        for (std::uint32_t voice = 0; voice < audio::kSasMaxVoices; ++voice)
            rt.memory().store32(arg(ctx, 1) + voice * 4u, static_cast<std::uint32_t>(core.envelope_height(voice)));
        kernel().finish(ctx, 0u);
    });
    hle.try_add("sceSasCore", "__sceSasGetGrain", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, audio::sas_core(arg(ctx, 0)).grain());
    });
    hle.try_add("sceSasCore", "__sceSasSetGrain", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, audio::sas_core(arg(ctx, 0)).set_grain(arg(ctx, 1)));
    });
    hle.try_add("sceSasCore", "__sceSasSetOutputmode", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, audio::sas_core(arg(ctx, 0)).set_output_mode(arg(ctx, 1)));
    });
    hle.try_add("sceSasCore", "__sceSasGetPauseFlag", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, audio::sas_core(arg(ctx, 0)).pause_flag());
    });
    hle.try_add("sceSasCore", "__sceSasGetEnvelopeHeight", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, static_cast<std::uint32_t>(audio::sas_core(arg(ctx, 0)).envelope_height(arg(ctx, 1))));
    });
    // __sceSasGetAllEnvelopeHeights(core, heights): every voice's envelope
    // height, 32 words. Stubbed, the game read back whatever the buffer held.
    hle.try_add("sceSasCore", "__sceSasGetAllEnvelopeHeights", [](Runtime &rt, AllegrexContext &ctx) {
        const audio::SasCore &sas = audio::sas_core(arg(ctx, 0));
        const std::uint32_t heights = arg(ctx, 1);
        auto &memory = rt.memory();
        if (memory.contains(heights, audio::kSasMaxVoices * 4u))
            for (std::uint32_t voice = 0; voice < audio::kSasMaxVoices; ++voice)
                memory.store32(heights + voice * 4u, static_cast<std::uint32_t>(sas.envelope_height(voice)));
        kernel().finish(ctx, 0u);
    });
    hle.try_add("sceSasCore", "__sceSasGetPauseFlag", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, audio::sas_core(arg(ctx, 0)).pause_flags());
    });
    // Reverb is not modelled, so the sends are accepted and dropped.
    for (const char *name : {"__sceSasRevType", "__sceSasRevParam", "__sceSasRevEVOL", "__sceSasRevVON"})
        hle.add("sceSasCore", name, [](Runtime &, AllegrexContext &ctx) { kernel().finish(ctx, 0u); });
    hle.add("sceSasCore", "__sceSasGetOutputmode", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, audio::sas_core(arg(ctx, 0)).output_mode());
    });
    hle.add("sceSasCore", "__sceSasGetEndFlag", [](Runtime &, AllegrexContext &ctx) {
        const std::uint32_t flags = audio::sas_core(arg(ctx, 0)).end_flag();
        static std::uint32_t last = 0u;
        if (trace_sas() && flags != last) std::cerr << "[sas] end flags " << psprecomp::hex32(flags) << "\n";
        last = flags;
        kernel().finish(ctx, flags);
    });
    hle.add("sceSasCore", "__sceSasCore", [](Runtime &rt, AllegrexContext &ctx) {
        sas_render(rt, arg(ctx, 0), arg(ctx, 1), false, 0u, 0u);
        kernel().finish(ctx, 0u);
    });
    hle.try_add("sceSasCore", "__sceSasCoreWithMix", [](Runtime &rt, AllegrexContext &ctx) {
        sas_render(rt, arg(ctx, 0), arg(ctx, 1), true, arg(ctx, 2), arg(ctx, 3));
        kernel().finish(ctx, 0u);
    });
}

} // namespace

void initialize_renderer() {
#if defined(PORTABLEKIT_HAS_RENDERER)
    (void)ensure_renderer();
#else
    std::cout << "Renderer: not built\n";
#endif
}

void register_media(HleRegistrar &hle) {
    initialize_renderer();
    register_display_ctrl(hle);
    register_vblank_waits(hle);
    register_ge(hle);
    register_audio(hle);
}

#if defined(PORTABLEKIT_HAS_RENDERER)
gpu::VulkanRenderer *active_renderer() {
    return media().renderer && media().renderer->available() ? media().renderer.get() : nullptr;
}

gpu::VulkanRenderer *ensure_renderer() {
    static bool tried = false;
    if (tried) return active_renderer();
    tried = true;
    if (portablekit::env("NO_RENDER") != nullptr) {
        std::cout << "Renderer: disabled by " << portablekit::env_name("NO_RENDER") << "\n";
        return nullptr;
    }
    auto renderer = std::make_unique<gpu::VulkanRenderer>();
    std::string error;
    gpu::RendererConfig config;
    // Tells windows apart when several instances run side by side, e.g. two
    // players testing ad hoc play on one machine.
    if (const char *title = portablekit::env("WINDOW_TITLE"); title != nullptr && *title != '\0')
        config.title = title;
    if (!renderer->initialize(config, error)) {
        std::cerr << "Renderer: unavailable (" << error << "); running headless\n";
        return nullptr;
    }
    media().renderer = std::move(renderer);
    ui::attach(*media().renderer);
    // Frame interpolation presents between flips: while the kernel waits for
    // real time, and while the game's code runs.
    kernel().set_idle_hook([](std::chrono::steady_clock::time_point wake) {
        if (gpu::VulkanRenderer *active = active_renderer()) active->present_until(wake);
    });
    kernel().set_poll_hook([] {
        if (gpu::VulkanRenderer *active = active_renderer()) active->present_due();
    });
    return media().renderer.get();
}
#endif

} // namespace portablekit
