# portablekit_add_game(<target> ...) builds one port: the framework's system
# layer, the game's own host code, and the AOT corpus generated from the
# player's executable.
#
#   portablekit_add_game(TenkawaNative
#       PROFILE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
#       SOURCES host/tenkawa_profile.cpp)
#
# PROFILE_DIR is where the profile keeps generated/ (the AOT corpus, never
# committed), overlays/ (one directory per recompiled code overlay) and its own
# host sources. SOURCES are those host sources, relative to PROFILE_DIR; one of
# them must define portablekit::game().
#
# The target is an executable in <build>/bin. Everything the old single-game
# build did — the renderer, the shaders, the version header, FFmpeg, the NID
# table, the overlay modules, the 64 MiB stack — happens here, so a profile's
# CMakeLists.txt is a few lines.

include_guard(GLOBAL)

set(PORTABLEKIT_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." CACHE INTERNAL "PortableKit source root")

# The system layer. Backend independent and always built; only the device the
# audio sink opens and the window the renderer creates depend on SDL.
set(PORTABLEKIT_HOST_SOURCES
    host/profile.cpp
    host/app_paths.cpp
    host/corpus_library.cpp
    host/audio/atrac_decoder.cpp
    host/movie/avc_decoder.cpp
    host/movie/psmf_demuxer.cpp
    host/movie/psmf_header.cpp
    host/audio/audio_sink.cpp
    host/audio/sas_core.cpp
    host/camera_probe.cpp
    host/camera/camera_input.cpp
    host/gpu/ge_state.cpp
    host/gpu/texture_decode.cpp
    host/input/bindings.cpp
    host/input/touch_controls.cpp
    host/gpu/texture_pack.cpp
    host/gpu/texture_pack_import.cpp
    host/perf/frame_stats.cpp
    host/perf/perf_overlay.cpp
    host/overlays.cpp
    host/system.cpp
    host/kernel/kernel.cpp
    host/kernel/fixed_pool.cpp
    host/kernel/message_pipe.cpp
    host/kernel/rtc_time.cpp
    host/kernel/load_trace.cpp
    host/kernel/fast_loading.cpp
    host/kernel/load_detector.cpp
    host/kernel/iso_image.cpp
    host/hle/hle_common.cpp
    host/hle/hle_threadman.cpp
    host/hle/hle_sysmem.cpp
    host/hle/hle_io.cpp
    host/hle/hle_system.cpp
    host/hle/hle_media.cpp
    host/hle/hle_atrac.cpp
    host/hle/hle_mpeg.cpp
    host/hle/hle_psmfplayer.cpp
    host/hle/hle_psmf.cpp
    host/hle/hle_net_offline.cpp
    host/fonts/game_font.cpp
    host/hle/hle_font.cpp
    host/hle/hle_utility.cpp
    host/hle/hle_savedata.cpp
    host/hle/hle_adhoc.cpp
    host/hle/hle_extensions.cpp
    host/hle/hle_extension_host.cpp
    host/adhoc/client.cpp
    host/adhoc/discovery.cpp
    host/adhoc/host.cpp
    host/adhoc/server.cpp
    host/save_data/aes128.cpp
    host/save_data/param_sfo.cpp
    host/save_data/savedata_crypto.cpp
    host/save_data/savedata_store.cpp
    host/save_data/save_transfer.cpp
    host/install/executable_preparation.cpp
    host/install/front_end.cpp
    host/install/installer.cpp
    host/install/installer_dialogs.cpp
    host/install/user_data.cpp
    host/settings/settings.cpp
    third_party/tiny_aes/aes.c
    CACHE INTERNAL "PortableKit system-layer sources, relative to PORTABLEKIT_ROOT")

set(PORTABLEKIT_RENDERER_SOURCES
    host/gpu/frame_interpolation.cpp
    host/gpu/frame_pacing.cpp
    host/gpu/replacement_textures.cpp
    host/gpu/vulkan_renderer.cpp
    host/ui/file_browser.cpp
    host/ui/font_menu.cpp
    host/ui/input_script.cpp
    host/ui/layer.cpp
    host/ui/menu.cpp
    host/ui/save_screen.cpp
    host/ui/setup_screens.cpp
    host/ui/text_input.cpp
    host/ui/touch_overlay.cpp
    host/ui/texture_pack_screen.cpp
    host/ui/widgets.cpp
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_sdl3.cpp
    third_party/imgui/backends/imgui_impl_vulkan.cpp
    CACHE INTERNAL "PortableKit renderer and interface sources")

function(_portablekit_prefix out root)
    set(result)
    foreach(source IN LISTS ARGN)
        if(IS_ABSOLUTE "${source}")
            list(APPEND result "${source}")
        else()
            list(APPEND result "${root}/${source}")
        endif()
    endforeach()
    set(${out} "${result}" PARENT_SCOPE)
endfunction()

# The framework's build settings, for a target made in the port's directory
# (see the end of the framework's CMakeLists.txt). A setting the port's own
# directory already has is left as the port set it.
function(_portablekit_inherit_settings target)
    get_property(launcher GLOBAL PROPERTY PORTABLEKIT_CXX_COMPILER_LAUNCHER)
    if(launcher AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
        set_property(TARGET ${target} PROPERTY CXX_COMPILER_LAUNCHER "${launcher}")
    endif()
    get_property(definitions GLOBAL PROPERTY PORTABLEKIT_COMPILE_DEFINITIONS)
    if(definitions)
        target_compile_definitions(${target} PRIVATE ${definitions})
    endif()
    get_property(options GLOBAL PROPERTY PORTABLEKIT_COMPILE_OPTIONS)
    if(options)
        target_compile_options(${target} PRIVATE ${options})
    endif()
    get_property(ipo GLOBAL PROPERTY PORTABLEKIT_IPO)
    if(ipo)
        set_target_properties(${target} PROPERTIES
            INTERPROCEDURAL_OPTIMIZATION_RELEASE ON
            INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON
            INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL ON)
    endif()
endfunction()

# portablekit_add_hle_extension(<name> LICENSE <licence> SOURCES ...
#                               [TITLE <title>] [VERSION <version>]
#                               [INCLUDE_DIRECTORIES ...] [LINK_LIBRARIES ...]
#                               [COMPILE_DEFINITIONS ...])
# declares an HLE extension module (docs/HLE_EXTENSIONS.md): a static library,
# built from SOURCES against include/portablekit/hle_extension.hpp, that
# defines PORTABLEKIT_HLE_EXTENSION(<name>). LICENSE is the licence the module
# states (an SPDX identifier such as "MIT" where there is one), and it, TITLE
# and VERSION are printed at startup and by --version. Every program portablekit_add_game()
# makes afterwards links it and calls that entry at startup. <name> is a C
# identifier and unique in the build. A module's directory is usually handed
# to the build in PORTABLEKIT_HLE_EXTENSION_DIRS, and its CMakeLists.txt is
# this one call; a port may also call it itself before portablekit_add_game().
function(portablekit_add_hle_extension name)
    cmake_parse_arguments(EXT "" "LICENSE;TITLE;VERSION"
        "SOURCES;INCLUDE_DIRECTORIES;LINK_LIBRARIES;COMPILE_DEFINITIONS" ${ARGN})
    if(NOT name MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR "portablekit_add_hle_extension: '${name}' is not a C identifier")
    endif()
    if(NOT EXT_SOURCES)
        message(FATAL_ERROR "portablekit_add_hle_extension(${name}): no SOURCES")
    endif()
    if(NOT EXT_LICENSE)
        message(FATAL_ERROR "portablekit_add_hle_extension(${name}): no LICENSE")
    endif()
    if(NOT EXT_TITLE)
        set(EXT_TITLE "${name}")
    endif()
    foreach(field IN ITEMS LICENSE TITLE VERSION)
        string(FIND "${EXT_${field}}" "\"" quote)
        string(FIND "${EXT_${field}}" "\\" backslash)
        string(FIND "${EXT_${field}}" "\n" newline)
        if(NOT quote EQUAL -1 OR NOT backslash EQUAL -1 OR NOT newline EQUAL -1)
            message(FATAL_ERROR "portablekit_add_hle_extension(${name}): ${field} may not contain quotes, backslashes or new lines")
        endif()
    endforeach()
    get_property(known GLOBAL PROPERTY PORTABLEKIT_HLE_EXTENSIONS)
    if(name IN_LIST known)
        message(FATAL_ERROR "portablekit_add_hle_extension: a module named '${name}' was already added")
    endif()
    set(target "portablekit_hle_extension_${name}")
    add_library(${target} STATIC ${EXT_SOURCES})
    target_compile_features(${target} PRIVATE cxx_std_20)
    _portablekit_inherit_settings(${target})
    # Headers only, as for an overlay module: the runtime's objects belong to
    # the program the module is linked into. host/ is there for a module that
    # reaches past the stable interface, at its own risk.
    target_include_directories(${target} PRIVATE
        "${PORTABLEKIT_ROOT}/include" "${PORTABLEKIT_ROOT}/host" ${EXT_INCLUDE_DIRECTORIES})
    if(EXT_COMPILE_DEFINITIONS)
        target_compile_definitions(${target} PRIVATE ${EXT_COMPILE_DEFINITIONS})
    endif()
    if(EXT_LINK_LIBRARIES)
        target_link_libraries(${target} PRIVATE ${EXT_LINK_LIBRARIES})
    endif()
    set_property(GLOBAL APPEND PROPERTY PORTABLEKIT_HLE_EXTENSIONS "${name}")
    set_property(GLOBAL PROPERTY PORTABLEKIT_HLE_EXTENSION_${name}_INFO
        "\"${name}\", \"${EXT_TITLE}\", \"${EXT_VERSION}\", \"${EXT_LICENSE}\"")
    message(STATUS "HLE extension module: ${EXT_TITLE} ${EXT_VERSION} (${EXT_LICENSE})")
endfunction()

# Links every HLE extension module added so far into <target> and generates
# the list host/hle/hle_extensions.hpp's linked_hle_extensions() returns.
function(_portablekit_link_hle_extensions target)
    get_property(modules GLOBAL PROPERTY PORTABLEKIT_HLE_EXTENSIONS)
    set(declarations "")
    set(entries "")
    foreach(module IN LISTS modules)
        string(APPEND declarations "void portablekit_hle_extension_${module}(::portablekit::hle_extension::Registry &);\n")
        get_property(info GLOBAL PROPERTY PORTABLEKIT_HLE_EXTENSION_${module}_INFO)
        string(APPEND entries "        {${info}, &portablekit_hle_extension_${module}},\n")
        target_link_libraries(${target} PRIVATE "portablekit_hle_extension_${module}")
    endforeach()
    if(modules)
        set(body "    static const HleExtensionModule modules[] = {\n${entries}    };\n    return modules;")
    else()
        set(body "    return {};")
    endif()
    set(list_source "${CMAKE_CURRENT_BINARY_DIR}/generated_hle_extensions/${target}_hle_extensions.cpp")
    file(CONFIGURE OUTPUT "${list_source}" CONTENT
"// Generated by cmake/PortableKit.cmake: the HLE extension modules linked into ${target}.
#include \"hle/hle_extensions.hpp\"

${declarations}
namespace portablekit {

std::span<const HleExtensionModule> linked_hle_extensions() {
${body}
}

} // namespace portablekit
")
    target_sources(${target} PRIVATE "${list_source}")
endfunction()

function(portablekit_add_game target)
    # Options a program that wraps the port uses (the desktop app,
    # apps/portablekit); a port passes none of them.
    #   EXTERNAL_KEYS   do not link crypto_keys_builtin.cpp: the program
    #                   defines portablekit::crypto_keys() itself.
    #   NO_CORPUS       link no generated code and no stub: the program
    #                   defines psprecomp::register_generated_functions().
    #   HOST_MAIN_NAME  compile host/main.cpp's main() under this name, so the
    #                   program's own main() can call it.
    cmake_parse_arguments(GAME "EXTERNAL_KEYS;NO_CORPUS" "PROFILE_DIR;HOST_MAIN_NAME" "SOURCES" ${ARGN})
    if(NOT GAME_PROFILE_DIR)
        set(GAME_PROFILE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()

    # The AOT corpus is derived from the player's own executable and is never
    # committed. Until the profile's generate step has produced it, link an
    # empty registry instead, so a checkout still builds and says so.
    file(GLOB generated CONFIGURE_DEPENDS "${GAME_PROFILE_DIR}/generated/*.cpp")
    if(GAME_NO_CORPUS)
        set(generated)
    elseif(generated)
        list(LENGTH generated generated_count)
        message(STATUS "${target}: ${generated_count} generated AOT units")
        if(MSVC)
            if(PSPRECOMP_GENERATED_OPT_LEVEL STREQUAL "0")
                set(generated_options "/Od;/bigobj")
            else()
                set(generated_options "/O${PSPRECOMP_GENERATED_OPT_LEVEL};/Ob0;/bigobj")
            endif()
        else()
            set(generated_options "-O${PSPRECOMP_GENERATED_OPT_LEVEL};-g0")
        endif()
        set_source_files_properties(${generated} PROPERTIES COMPILE_OPTIONS "${generated_options}")
    else()
        message(STATUS "${target}: no generated AOT units; run the profile's generate step")
        set(generated "${PORTABLEKIT_ROOT}/host/generated_stub.cpp")
    endif()
    if(NOT GAME_EXTERNAL_KEYS)
        list(APPEND GAME_SOURCES_ABSOLUTE "${PORTABLEKIT_ROOT}/host/crypto_keys_builtin.cpp")
    endif()
    if(GAME_HOST_MAIN_NAME)
        set_property(SOURCE "${PORTABLEKIT_ROOT}/host/main.cpp" APPEND PROPERTY COMPILE_DEFINITIONS
            PORTABLEKIT_HOST_MAIN_NAME=${GAME_HOST_MAIN_NAME})
    endif()

    # Renderer: SDL3 for the window and Vulkan for drawing. Both are optional
    # so a port still builds headless on a machine without them.
    option(PORTABLEKIT_RENDERER "Build the Vulkan renderer" ON)
    set(renderer_sources)
    if(PORTABLEKIT_RENDERER)
        find_package(SDL3 QUIET)
        find_package(Vulkan QUIET)
        find_program(PORTABLEKIT_GLSLANG NAMES glslangValidator glslang)
        # The interpreter by what CMake finds rather than by the name python3:
        # a Windows install from python.org has no python3.exe, and the name
        # then reaches the Microsoft Store's placeholder instead.
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if(SDL3_FOUND AND Vulkan_FOUND AND PORTABLEKIT_GLSLANG AND Python3_Interpreter_FOUND)
            set(shader_inc "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders/ge_shaders.inc")
            set(shaders
                "kGeVertexShader=${PORTABLEKIT_ROOT}/host/gpu/shaders/ge.vert"
                "kGeRawVertexShader=GE_RAW_VERTICES@${PORTABLEKIT_ROOT}/host/gpu/shaders/ge.vert"
                "kGeCheckVertexShader=GE_RAW_VERTICES,GE_CHECK_DECODE@${PORTABLEKIT_ROOT}/host/gpu/shaders/ge.vert"
                "kGeFragmentShader=${PORTABLEKIT_ROOT}/host/gpu/shaders/ge.frag")
            if(ANDROID)
                # Pre-rotation of the finished frame for a display turned sideways.
                list(APPEND shaders
                    "kRotateVertexShader=${PORTABLEKIT_ROOT}/host/gpu/shaders/rotate.vert"
                    "kRotateFragmentShader=${PORTABLEKIT_ROOT}/host/gpu/shaders/rotate.frag")
            endif()
            set(shader_sources ${shaders})
            list(TRANSFORM shader_sources REPLACE "^[A-Za-z]+=([A-Z_,]+@)?" "")
            list(REMOVE_DUPLICATES shader_sources)
            add_custom_command(
                OUTPUT "${shader_inc}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders"
                COMMAND "${Python3_EXECUTABLE}" "${PORTABLEKIT_ROOT}/tools/embed_shaders.py" "${PORTABLEKIT_GLSLANG}" "${shader_inc}"
                        ${shaders}
                DEPENDS ${shader_sources}
                        "${PORTABLEKIT_ROOT}/tools/embed_shaders.py"
                COMMENT "Compiling GE shaders to SPIR-V")
            add_custom_target(${target}_shaders DEPENDS "${shader_inc}")
            _portablekit_prefix(renderer_sources "${PORTABLEKIT_ROOT}" ${PORTABLEKIT_RENDERER_SOURCES})
            message(STATUS "${target}: Vulkan renderer enabled")
        else()
            message(STATUS "${target}: renderer disabled (SDL3, Vulkan, glslang or Python 3 not found)")
            set(PORTABLEKIT_RENDERER OFF)
        endif()
    endif()

    # The build the in-game menu shows: `git describe` of the profile checkout,
    # refreshed on every build but rewritten only when it changes, so only the
    # menu recompiles after a commit.
    set(version_header "${CMAKE_CURRENT_BINARY_DIR}/generated_version/portablekit_version.hpp")
    add_custom_target(${target}_version
        COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${GAME_PROFILE_DIR} -DOUTPUT=${version_header}
                -P "${PORTABLEKIT_ROOT}/tools/write_version.cmake"
        BYPRODUCTS "${version_header}"
        COMMENT "Checking the build version")

    # FFmpeg decodes the game's music and its movies.
    include("${PORTABLEKIT_ROOT}/cmake/FFmpeg.cmake")

    _portablekit_prefix(host_sources "${PORTABLEKIT_ROOT}" ${PORTABLEKIT_HOST_SOURCES})
    _portablekit_prefix(profile_sources "${GAME_PROFILE_DIR}" ${GAME_SOURCES})
    # tiny-AES-c is C; the project only enables C++, which compiles it unchanged.
    set_source_files_properties("${PORTABLEKIT_ROOT}/third_party/tiny_aes/aes.c" PROPERTIES LANGUAGE CXX)

    # The recompiled game code gets a target of its own. It needs only the
    # psprecomp headers, so definitions and include paths the host adds do not
    # change its compile commands and rebuild all of it. Its objects still link
    # straight into the executable, which exports their symbols to the overlays.
    if(generated)
    add_library(${target}_generated OBJECT ${generated})
    # The standard is stated per target: a game's own CMakeLists is the top
    # level project and need not set CMAKE_CXX_STANDARD for the framework.
    target_compile_features(${target}_generated PRIVATE cxx_std_20)
    target_include_directories(${target}_generated PRIVATE
        "${PORTABLEKIT_ROOT}/include" "${GAME_PROFILE_DIR}/generated")
    set_target_properties(${target}_generated PROPERTIES JOB_POOL_COMPILE psprecomp_generated)
    _portablekit_inherit_settings(${target}_generated)
    set(generated_objects $<TARGET_OBJECTS:${target}_generated>)
    else()
        set(generated_objects)
    endif()

    # An Android app is a shared library, libmain.so, that SDL's Java activity
    # loads and calls; the command-line executable still builds for Android
    # without this and runs from adb shell.
    option(PORTABLEKIT_ANDROID_APP "Build the Android app's libmain.so instead of an executable" OFF)
    if(PORTABLEKIT_ANDROID_APP AND NOT (ANDROID AND PORTABLEKIT_RENDERER))
        message(FATAL_ERROR "PORTABLEKIT_ANDROID_APP needs the Android NDK toolchain and the renderer (SDL3 and Vulkan)")
    endif()
    set(program_sources
        "${PORTABLEKIT_ROOT}/host/main.cpp"
        ${host_sources}
        ${profile_sources}
        ${GAME_SOURCES_ABSOLUTE}
        ${renderer_sources}
        ${generated_objects})
    if(PORTABLEKIT_ANDROID_APP)
        add_library(${target} SHARED ${program_sources} "${PORTABLEKIT_ROOT}/host/platform/android_app.cpp"
            "${PORTABLEKIT_ROOT}/host/platform/android_jni.cpp"
            "${PORTABLEKIT_ROOT}/host/platform/android_documents.cpp"
            "${PORTABLEKIT_ROOT}/host/platform/android_performance.cpp")
        target_compile_definitions(${target} PRIVATE PORTABLEKIT_ANDROID_APP=1)
        set_target_properties(${target} PROPERTIES
            OUTPUT_NAME main
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    else()
        add_executable(${target} ${program_sources})
    endif()
    target_compile_features(${target} PRIVATE cxx_std_20)
    _portablekit_inherit_settings(${target})
    add_dependencies(${target} ${target}_version)
    # What a build is called besides its version (PORTABLEKIT_BUILD_LABEL), in
    # a header of its own, so that only a change of the label rebuilds what
    # shows it.
    set(PORTABLEKIT_BUILD_LABEL "" CACHE STRING
        "A name for this build, shown beside the version in window titles, menus and --version")
    string(FIND "${PORTABLEKIT_BUILD_LABEL}" "\"" label_quote)
    string(FIND "${PORTABLEKIT_BUILD_LABEL}" "\\" label_backslash)
    string(FIND "${PORTABLEKIT_BUILD_LABEL}" "\n" label_newline)
    if(NOT label_quote EQUAL -1 OR NOT label_backslash EQUAL -1 OR NOT label_newline EQUAL -1)
        message(FATAL_ERROR "PORTABLEKIT_BUILD_LABEL may not contain quotes, backslashes or new lines")
    endif()
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated_version/portablekit_build_label.hpp" CONTENT
"#pragma once

// Generated by cmake/PortableKit.cmake from PORTABLEKIT_BUILD_LABEL.
namespace portablekit {
// Empty when the build was given no label.
inline constexpr const char *kBuildLabel = \"${PORTABLEKIT_BUILD_LABEL}\";
}
")
    if(PORTABLEKIT_BUILD_LABEL)
        message(STATUS "${target}: build label \"${PORTABLEKIT_BUILD_LABEL}\"")
    endif()
    if(PORTABLEKIT_RENDERER)
        add_dependencies(${target} ${target}_shaders)
        # The sink opens its own SDL audio subsystem, so it only needs SDL to exist.
        target_compile_definitions(${target} PRIVATE
            PORTABLEKIT_HAS_RENDERER=1 PORTABLEKIT_HAS_SDL_AUDIO=1 PORTABLEKIT_HAS_SDL=1)
        target_include_directories(${target} PRIVATE
            "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders"
            "${PORTABLEKIT_ROOT}/third_party/imgui")
        target_link_libraries(${target} PRIVATE SDL3::SDL3 Vulkan::Vulkan)
        # Windows finds a DLL next to the executable or on PATH, and SDL3's
        # development package puts it in neither: put it next to the
        # executable, as the FFmpeg DLLs already are.
        if(WIN32 AND TARGET SDL3::SDL3-shared)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "$<TARGET_FILE:SDL3::SDL3-shared>" "$<TARGET_FILE_DIR:${target}>")
        endif()
    endif()
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated_version")
    # The host only: the generated code never sees FFmpeg.
    portablekit_use_ffmpeg(${target})
    target_include_directories(${target} PRIVATE
        "${PORTABLEKIT_ROOT}/host"
        "${PORTABLEKIT_ROOT}/third_party"
        "${GAME_PROFILE_DIR}/host"
        "${GAME_PROFILE_DIR}/generated")
    target_link_libraries(${target} PRIVATE psprecomp_core ${CMAKE_DL_LIBS})
    _portablekit_link_hle_extensions(${target})
    if(WIN32)
        # The ad hoc client's and server's sockets, and the list of interfaces.
        target_link_libraries(${target} PRIVATE ws2_32 iphlpapi)
    endif()
    find_package(Threads REQUIRED)
    target_link_libraries(${target} PRIVATE Threads::Threads)
    # The overlay libraries resolve psprecomp and runtime symbols against the
    # executable that loads them, so it has to export them.
    set_target_properties(${target} PROPERTIES ENABLE_EXPORTS TRUE WINDOWS_EXPORT_ALL_SYMBOLS TRUE)

    # The NID table is compiled in, so the program reads nothing from the
    # checkout it was built in. A profile may add its own rows in
    # <profile>/config/nids.csv.
    set(nids_inc "${CMAKE_CURRENT_BINARY_DIR}/generated_nids/nid_table.inc")
    set(nids_csv "${PORTABLEKIT_ROOT}/configs/nids.csv")
    if(EXISTS "${GAME_PROFILE_DIR}/config/nids.csv")
        list(APPEND nids_csv "${GAME_PROFILE_DIR}/config/nids.csv")
    endif()
    string(REPLACE ";" "|" nids_input "${nids_csv}")
    add_custom_command(
        OUTPUT "${nids_inc}"
        COMMAND ${CMAKE_COMMAND} "-DINPUT=${nids_input}" -DOUTPUT=${nids_inc}
                -P "${PORTABLEKIT_ROOT}/tools/embed_nids.cmake"
        DEPENDS ${nids_csv} "${PORTABLEKIT_ROOT}/tools/embed_nids.cmake"
        COMMENT "Embedding the NID table")
    target_sources(${target} PRIVATE "${nids_inc}")
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated_nids")

    # A release build runs from wherever it is unpacked: it has no fallback to
    # this checkout's game directory, and on Linux it finds the libraries it
    # ships in lib/ next to the executable and nothing else.
    option(PORTABLEKIT_RELEASE "Build for distribution: no checkout paths, libraries from lib/" OFF)
    if(NOT PORTABLEKIT_RELEASE)
        # <profile>/game, a developer fallback after the per-user data
        # directory. Only main.cpp reads it; on every generated unit's command
        # line it would rebuild all of them when the checkout moves.
        set_property(SOURCE "${PORTABLEKIT_ROOT}/host/main.cpp" APPEND PROPERTY COMPILE_DEFINITIONS
            PORTABLEKIT_DEFAULT_GAME_DIR="${GAME_PROFILE_DIR}/game")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set_target_properties(${target} PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "$ORIGIN/lib")
        # The C++ runtime is linked in statically, so a build made against an
        # old glibc runs wherever that glibc does, and its symbols stay
        # private: the executable exports its own symbols to the overlay
        # libraries, and a second, exported libstdc++ would clash with the
        # system's one that the graphics driver loads.
        set(PORTABLEKIT_RELEASE_LINK_OPTIONS
            -static-libstdc++ "LINKER:--exclude-libs,libstdc++.a:libsupc++.a" PARENT_SCOPE)
        target_link_options(${target} PRIVATE
            -static-libstdc++ "LINKER:--exclude-libs,libstdc++.a:libsupc++.a")
    endif()

    # Chained AOT calls nest native frames; give the guest-executing main
    # thread the same 64 MiB stack on every platform.
    if(MSVC)
        target_link_options(${target} PRIVATE /STACK:67108864)
    elseif(MINGW)
        # MinGW's default is MSVC's 1 MiB reserve too; generated code at -O0
        # overflowed it at once (Chinatown Wars, 0xC00000FD).
        target_link_options(${target} PRIVATE "LINKER:--stack,67108864")
    elseif(APPLE)
        target_link_options(${target} PRIVATE "LINKER:-stack_size,0x4000000")
    endif()
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        JOB_POOL_COMPILE psprecomp_generated)

    # Other editions of the game (GameProfile::variants): each one's corpus,
    # generated into <profile>/generated-variants/<key>/, becomes a library
    # of its own in bin/corpora/, which host/main.cpp loads when that
    # edition's executable is the one running. Such a library needs nothing
    # from the executable (psprecomp/corpus_abi.hpp).
    file(GLOB variant_dirs LIST_DIRECTORIES true "${GAME_PROFILE_DIR}/generated-variants/*")
    foreach(variant_dir IN LISTS variant_dirs)
        if(NOT IS_DIRECTORY "${variant_dir}")
            continue()
        endif()
        get_filename_component(variant_key "${variant_dir}" NAME)
        file(GLOB variant_sources CONFIGURE_DEPENDS "${variant_dir}/*.cpp")
        if(NOT variant_sources)
            continue()
        endif()
        set(variant_target "${target}_corpus_${variant_key}")
        string(MAKE_C_IDENTIFIER "${variant_target}" variant_target)
        set(variant_entry "${CMAKE_CURRENT_BINARY_DIR}/corpora/${variant_key}_module.cpp")
        configure_file("${PORTABLEKIT_ROOT}/host/corpus_module.cpp.in" "${variant_entry}" COPYONLY)
        add_library(${variant_target} MODULE ${variant_sources} "${variant_entry}")
        target_compile_features(${variant_target} PRIVATE cxx_std_20)
        _portablekit_inherit_settings(${variant_target})
        target_include_directories(${variant_target} PRIVATE "${PORTABLEKIT_ROOT}/include" "${variant_dir}")
        if(generated_options)
            target_compile_options(${variant_target} PRIVATE ${generated_options})
        elseif(MSVC)
            target_compile_options(${variant_target} PRIVATE /O${PSPRECOMP_GENERATED_OPT_LEVEL} /bigobj)
        else()
            target_compile_options(${variant_target} PRIVATE -O${PSPRECOMP_GENERATED_OPT_LEVEL} -g0)
        endif()
        set_target_properties(${variant_target} PROPERTIES
            JOB_POOL_COMPILE psprecomp_generated
            CXX_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN ON
            PREFIX ""
            SUFFIX "${CMAKE_SHARED_LIBRARY_SUFFIX}"
            OUTPUT_NAME "${variant_key}"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/corpora")
        add_dependencies(${target} ${variant_target})
        message(STATUS "${target}: corpus of edition ${variant_key}")
    endforeach()

    # Recompiled overlay corpora, produced by tools/add_overlay.py. Each
    # directory under <profile>/overlays/ holds one corpus and becomes one
    # shared module the host loads at run time, so a new overlay never relinks
    # the executable.
    set(PORTABLEKIT_OVERLAY_DIR "${CMAKE_BINARY_DIR}/bin/overlays")
    file(GLOB overlay_metadata CONFIGURE_DEPENDS "${GAME_PROFILE_DIR}/overlays/*/meta.txt")
    foreach(meta IN LISTS overlay_metadata)
        _portablekit_add_overlay("${target}" "${meta}" "${PORTABLEKIT_OVERLAY_DIR}")
    endforeach()
    list(LENGTH overlay_metadata overlay_count)
    if(overlay_count GREATER 0)
        message(STATUS "${target}: ${overlay_count} overlay librar(ies) in ${PORTABLEKIT_OVERLAY_DIR}")
    endif()
endfunction()

function(_portablekit_add_overlay host_target meta_path output_dir)
    get_filename_component(source_dir "${meta_path}" DIRECTORY)
    get_filename_component(PORTABLEKIT_OVERLAY_PREFIX "${source_dir}" NAME)
    file(STRINGS "${meta_path}" meta_lines REGEX "^(base|size|code_size|hash|name)=")
    foreach(line IN LISTS meta_lines)
        string(REGEX MATCH "^([a-z_]+)=(.+)$" matched "${line}")
        if(NOT matched)
            continue()
        endif()
        string(TOUPPER "${CMAKE_MATCH_1}" key)
        set(PORTABLEKIT_OVERLAY_${key} "${CMAKE_MATCH_2}")
    endforeach()
    if(NOT PORTABLEKIT_OVERLAY_BASE OR NOT PORTABLEKIT_OVERLAY_SIZE OR NOT PORTABLEKIT_OVERLAY_CODE_SIZE
       OR NOT PORTABLEKIT_OVERLAY_HASH OR NOT PORTABLEKIT_OVERLAY_NAME)
        message(WARNING "portablekit: incomplete overlay metadata in ${meta_path}")
        return()
    endif()

    file(GLOB sources CONFIGURE_DEPENDS "${source_dir}/*.cpp")
    if(NOT sources)
        message(WARNING "portablekit: no sources for overlay ${PORTABLEKIT_OVERLAY_PREFIX}")
        return()
    endif()
    set(entry_point "${CMAKE_CURRENT_BINARY_DIR}/overlays/${PORTABLEKIT_OVERLAY_PREFIX}_module.cpp")
    configure_file("${PORTABLEKIT_ROOT}/host/overlay_module.cpp.in" "${entry_point}" @ONLY)

    set(target "overlay_${PORTABLEKIT_OVERLAY_PREFIX}")
    add_library(${target} MODULE ${sources} "${entry_point}")
    target_compile_features(${target} PRIVATE cxx_std_20)
    _portablekit_inherit_settings(${target})
    # Headers only: linking psprecomp_core would give the module its own copy
    # of the runtime state the host already owns.
    target_include_directories(${target} PRIVATE
        "${source_dir}" "${PORTABLEKIT_ROOT}/host" "${PORTABLEKIT_ROOT}/include")
    if(WIN32 OR APPLE OR PORTABLEKIT_ANDROID_APP)
        # Both linkers want the host binary while linking the module: MSVC for
        # its import library, ld64 as the bundle loader. ELF leaves the host
        # symbols undefined and resolves them when the module is loaded. In an
        # Android app the host is libmain.so, which the app loads privately,
        # so the module names it as a dependency to find its symbols.
        target_link_libraries(${target} PRIVATE ${host_target})
    else()
        add_dependencies(${target} ${host_target})
        if(ANDROID)
            # The NDK links every shared library with --no-undefined; these
            # leave the host's symbols to the loader like any other ELF.
            target_link_options(${target} PRIVATE "LINKER:-z,undefs")
        endif()
    endif()
    if(MSVC)
        target_compile_definitions(${target} PRIVATE PSPRECOMP_IMPORT_HOST_SYMBOLS=1)
        if(PSPRECOMP_GENERATED_OPT_LEVEL STREQUAL "0")
            target_compile_options(${target} PRIVATE /Od /bigobj)
        else()
            target_compile_options(${target} PRIVATE /O${PSPRECOMP_GENERATED_OPT_LEVEL} /Ob0 /bigobj)
        endif()
    else()
        target_compile_options(${target} PRIVATE -O${PSPRECOMP_GENERATED_OPT_LEVEL} -g0)
    endif()
    if(PORTABLEKIT_RELEASE_LINK_OPTIONS)
        target_link_options(${target} PRIVATE ${PORTABLEKIT_RELEASE_LINK_OPTIONS})
    endif()
    set_target_properties(${target} PROPERTIES
        JOB_POOL_COMPILE psprecomp_generated
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        PREFIX ""
        SUFFIX "${CMAKE_SHARED_LIBRARY_SUFFIX}"
        OUTPUT_NAME "${PORTABLEKIT_OVERLAY_PREFIX}"
        LIBRARY_OUTPUT_DIRECTORY "${output_dir}")
    if(PORTABLEKIT_ANDROID_APP)
        # An APK only carries libraries named lib*.so.
        set_target_properties(${target} PROPERTIES PREFIX "lib")
    endif()
endfunction()
