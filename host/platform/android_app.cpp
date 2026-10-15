// The entry point of the Android app. SDLActivity loads libmain.so and calls
// SDL_main on a Java thread whose stack is far too small for chained AOT
// calls, and an app cannot restart itself with a larger one the way the Linux
// executable does. So the game runs on a thread of its own with the 64 MiB
// stack the other platforms give their main thread.

#include "app_paths.hpp"
#include "profile.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <pthread.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

// An app has no terminal: what the program prints goes to <app_name>.log in
// its internal storage, where `adb shell run-as` and the menu's Save the log
// reach it; the one before is kept as <app_name>-previous.log.
void redirect_output() {
    const char *storage = SDL_GetAndroidInternalStoragePath();
    if (storage == nullptr) return;
    const std::string log = std::string(storage) + "/" + portablekit::game().app_name + ".log";
    // The previous run's log survives one start, for reporting a crash.
    std::rename(log.c_str(),
                (std::string(storage) + "/" + portablekit::game().app_name + "-previous.log").c_str());
    if (std::freopen(log.c_str(), "w", stdout) != nullptr) std::setvbuf(stdout, nullptr, _IOLBF, 0);
    if (std::freopen(log.c_str(), "a", stderr) != nullptr) std::setvbuf(stderr, nullptr, _IONBF, 0);
}

// The APK carries, as assets, the fallback font a release ships in fonts/ and
// the launcher's images in launcher/. The font code maps files and the
// launcher reads them, so they are unpacked to the app's storage once and
// found there, as an app's bundled resources are on the desktop.
void unpack_asset(const std::filesystem::path &root, const std::string &asset) {
    std::error_code ec;
    const std::filesystem::path target = root / asset;
    std::filesystem::create_directories(target.parent_path(), ec);
    SDL_IOStream *stream = SDL_IOFromFile(asset.c_str(), "rb");
    if (stream == nullptr) return;
    const Sint64 size = SDL_GetIOSize(stream);
    if (size > 0 && std::filesystem::file_size(target, ec) == static_cast<std::uintmax_t>(size)) {
        SDL_CloseIO(stream);
        return;
    }
    std::size_t length = 0;
    void *data = SDL_LoadFile_IO(stream, &length, true);
    if (data == nullptr) return;
    const std::filesystem::path partial = target.string() + ".partial";
    {
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        out.write(static_cast<const char *>(data), static_cast<std::streamsize>(length));
    }
    SDL_free(data);
    std::filesystem::rename(partial, target, ec);
}

void unpack_bundled_resources() {
    const char *storage = SDL_GetAndroidInternalStoragePath();
    if (storage == nullptr) return;
    const std::filesystem::path root = std::filesystem::path(storage) / "bundled";
    unpack_asset(root, "fonts/NotoSansCJKjp-Regular.otf");
    if (const portablekit::LauncherTheme *theme = portablekit::game().launcher) {
        if (theme->backdrop_file != nullptr) unpack_asset(root, std::string("launcher/") + theme->backdrop_file);
        if (theme->logo_file != nullptr) unpack_asset(root, std::string("launcher/") + theme->logo_file);
    }
    portablekit::set_bundled_resource_directory(root);
}

// An app has no shell to set the <prefix>_* variables that switch on traces
// and fallbacks in: env.txt in the app's internal storage holds them, one
// NAME=value a line, read at start. Only `adb shell run-as` on a debuggable
// build reaches that folder, so players never meet it.
void load_environment_file() {
    const char *storage = SDL_GetAndroidInternalStoragePath();
    if (storage == nullptr) return;
    std::ifstream in(std::filesystem::path(storage) / "env.txt");
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t equals = line.find('=');
        if (line.empty() || line[0] == '#' || equals == std::string::npos || equals == 0u) continue;
        const std::string name = line.substr(0, equals);
        setenv(name.c_str(), line.c_str() + equals + 1u, 1);
        std::printf("[env] %s from env.txt\n", name.c_str());
    }
}

} // namespace

int main(int argc, char *argv[]) {
    redirect_output();
    load_environment_file();
    // Back opens the in-game menu instead of leaving the app.
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    unpack_bundled_resources();
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
