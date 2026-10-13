// The entry point of the Android app. SDLActivity loads libmain.so and calls
// SDL_main on a Java thread whose stack is far too small for chained AOT
// calls, and an app cannot restart itself with a larger one the way the Linux
// executable does. So the game runs on a thread of its own with the 64 MiB
// stack the other platforms give their main thread.

#include "app_paths.hpp"
#include "platform/android_jni.hpp"
#include "profile.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <pthread.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

int portablekit_main(int argc, char **argv);

namespace {

constexpr std::size_t kGameStackBytes = 64u * 1024u * 1024u;

struct Launch {
    int argc;
    char **argv;
    int result;
};

void *run_game(void *argument) {
    auto *launch = static_cast<Launch *>(argument);
    launch->result = portablekit_main(launch->argc, launch->argv);
    return nullptr;
}

// An app has no terminal: write output to a user-selected folder when available.
void redirect_output() {
    // Prefer the folder selected through Android's document picker. The Java
    // bridge retains the SAF permission across process death.
    const int picked = portablekit::android::open_crash_log(portablekit::game().project_name);
    if (picked >= 0) {
        const int out_ok = ::dup2(picked, STDOUT_FILENO);
        const int err_ok = ::dup2(picked, STDERR_FILENO);
        ::close(picked);
        if (out_ok >= 0 && err_ok >= 0) {
            std::setvbuf(stdout, nullptr, _IOLBF, 0);
            std::setvbuf(stderr, nullptr, _IONBF, 0);
            std::fprintf(stderr, "\\n=== %s startup ===\\n", portablekit::game().project_name);
            return;
        }
    }
    // If the picker was cancelled or unavailable, keep the private fallback.
    const char *storage = SDL_GetAndroidInternalStoragePath();
    if (storage == nullptr) return;
    const std::string log = std::string(storage) + "/" + portablekit::game().project_name + ".log";
    std::rename(log.c_str(),
                (std::string(storage) + "/" + portablekit::game().project_name + "-previous.log").c_str());
    if (std::freopen(log.c_str(), "w", stdout) != nullptr) std::setvbuf(stdout, nullptr, _IOLBF, 0);
    if (std::freopen(log.c_str(), "a", stderr) != nullptr) std::setvbuf(stderr, nullptr, _IONBF, 0);
}

// The APK carries the fallback font a release ships in fonts/, as an asset.
// The font code maps files, so it is unpacked to the app's storage once and
// found there.
constexpr const char *kBundledFonts[] = {"NotoSansCJKjp-Regular.otf"};

void unpack_bundled_fonts() {
    const char *storage = SDL_GetAndroidInternalStoragePath();
    if (storage == nullptr) return;
    const std::filesystem::path root = std::filesystem::path(storage) / "bundled";
    std::error_code ec;
    std::filesystem::create_directories(root / "fonts", ec);
    for (const char *name : kBundledFonts) {
        const std::filesystem::path target = root / "fonts" / name;
        const std::string asset = std::string("fonts/") + name;
        SDL_IOStream *stream = SDL_IOFromFile(asset.c_str(), "rb");
        if (stream == nullptr) continue;
        const Sint64 size = SDL_GetIOSize(stream);
        if (size > 0 && std::filesystem::file_size(target, ec) == static_cast<std::uintmax_t>(size)) {
            SDL_CloseIO(stream);
            continue;
        }
        std::size_t length = 0;
        void *data = SDL_LoadFile_IO(stream, &length, true);
        if (data == nullptr) continue;
        const std::filesystem::path partial = target.string() + ".partial";
        {
            std::ofstream out(partial, std::ios::binary | std::ios::trunc);
            out.write(static_cast<const char *>(data), static_cast<std::streamsize>(length));
        }
        SDL_free(data);
        std::filesystem::rename(partial, target, ec);
    }
    portablekit::set_bundled_resource_directory(root);
}

} // namespace

int main(int argc, char *argv[]) {
    redirect_output();
    // Back opens the in-game menu instead of leaving the app.
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    unpack_bundled_fonts();
    Launch launch{argc, argv, 1};
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, kGameStackBytes);
    pthread_t thread;
    if (pthread_create(&thread, &attributes, run_game, &launch) != 0) {
        std::fprintf(stderr, "%s: cannot start the game thread with a %zu MiB stack\n", portablekit::game().project_name,
                     kGameStackBytes >> 20);
        pthread_attr_destroy(&attributes);
        return 1;
    }
    pthread_attr_destroy(&attributes);
    pthread_join(thread, nullptr);
    std::fflush(stdout);
    return launch.result;
}
