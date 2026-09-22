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
    host/audio/atrac_decoder.cpp
    host/movie/avc_decoder.cpp
    host/movie/psmf_demuxer.cpp
    host/audio/audio_sink.cpp
    host/audio/sas_core.cpp
    host/camera_probe.cpp
    host/camera/camera_input.cpp
    host/gpu/ge_state.cpp
    host/gpu/texture_decode.cpp
    host/input/bindings.cpp
    host/perf/frame_stats.cpp
    host/perf/perf_overlay.cpp
    host/overlays.cpp
    host/system.cpp
    host/kernel/kernel.cpp
    host/kernel/iso_image.cpp
    host/hle/hle_common.cpp
    host/hle/hle_threadman.cpp
    host/hle/hle_sysmem.cpp
    host/hle/hle_io.cpp
    host/hle/hle_system.cpp
    host/hle/hle_media.cpp
    host/hle/hle_atrac.cpp
    host/hle/hle_mpeg.cpp
    host/fonts/game_font.cpp
    host/hle/hle_font.cpp
    host/hle/hle_utility.cpp
    host/hle/hle_savedata.cpp
    host/hle/hle_adhoc.cpp
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
    host/gpu/vulkan_renderer.cpp
    host/ui/file_browser.cpp
    host/ui/font_menu.cpp
    host/ui/input_script.cpp
    host/ui/layer.cpp
    host/ui/menu.cpp
    host/ui/save_screen.cpp
    host/ui/setup_screens.cpp
    host/ui/text_input.cpp
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
        list(APPEND result "${root}/${source}")
    endforeach()
    set(${out} "${result}" PARENT_SCOPE)
endfunction()

function(portablekit_add_game target)
    cmake_parse_arguments(GAME "" "PROFILE_DIR" "SOURCES" ${ARGN})
    if(NOT GAME_PROFILE_DIR)
        set(GAME_PROFILE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()

    # The AOT corpus is derived from the player's own executable and is never
    # committed. Until the profile's generate step has produced it, link an
    # empty registry instead, so a checkout still builds and says so.
    file(GLOB generated CONFIGURE_DEPENDS "${GAME_PROFILE_DIR}/generated/*.cpp")
    if(generated)
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

    # Renderer: SDL3 for the window and Vulkan for drawing. Both are optional
    # so a port still builds headless on a machine without them.
    option(PORTABLEKIT_RENDERER "Build the Vulkan renderer" ON)
    set(renderer_sources)
    if(PORTABLEKIT_RENDERER)
        find_package(SDL3 QUIET)
        find_package(Vulkan QUIET)
        find_program(PORTABLEKIT_GLSLANG NAMES glslangValidator glslang)
        if(SDL3_FOUND AND Vulkan_FOUND AND PORTABLEKIT_GLSLANG)
            set(shader_inc "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders/ge_shaders.inc")
            add_custom_command(
                OUTPUT "${shader_inc}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders"
                COMMAND python3 "${PORTABLEKIT_ROOT}/tools/embed_shaders.py" "${PORTABLEKIT_GLSLANG}" "${shader_inc}"
                        "kGeVertexShader=${PORTABLEKIT_ROOT}/host/gpu/shaders/ge.vert"
                        "kGeFragmentShader=${PORTABLEKIT_ROOT}/host/gpu/shaders/ge.frag"
                DEPENDS "${PORTABLEKIT_ROOT}/host/gpu/shaders/ge.vert"
                        "${PORTABLEKIT_ROOT}/host/gpu/shaders/ge.frag"
                        "${PORTABLEKIT_ROOT}/tools/embed_shaders.py"
                COMMENT "Compiling GE shaders to SPIR-V")
            add_custom_target(${target}_shaders DEPENDS "${shader_inc}")
            _portablekit_prefix(renderer_sources "${PORTABLEKIT_ROOT}" ${PORTABLEKIT_RENDERER_SOURCES})
            message(STATUS "${target}: Vulkan renderer enabled")
        else()
            message(STATUS "${target}: renderer disabled (SDL3, Vulkan or glslang not found)")
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
    add_library(${target}_generated OBJECT ${generated})
    # The standard is stated per target: a game's own CMakeLists is the top
    # level project and need not set CMAKE_CXX_STANDARD for the framework.
    target_compile_features(${target}_generated PRIVATE cxx_std_20)
    target_include_directories(${target}_generated PRIVATE
        "${PORTABLEKIT_ROOT}/include" "${GAME_PROFILE_DIR}/generated")
    set_target_properties(${target}_generated PROPERTIES JOB_POOL_COMPILE psprecomp_generated)

    add_executable(${target}
        "${PORTABLEKIT_ROOT}/host/main.cpp"
        ${host_sources}
        ${profile_sources}
        ${renderer_sources}
        $<TARGET_OBJECTS:${target}_generated>)
    target_compile_features(${target} PRIVATE cxx_std_20)
    add_dependencies(${target} ${target}_version)
    if(PORTABLEKIT_RENDERER)
        add_dependencies(${target} ${target}_shaders)
        # The sink opens its own SDL audio subsystem, so it only needs SDL to exist.
        target_compile_definitions(${target} PRIVATE
            PORTABLEKIT_HAS_RENDERER=1 PORTABLEKIT_HAS_SDL_AUDIO=1 PORTABLEKIT_HAS_SDL=1)
        target_include_directories(${target} PRIVATE
            "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders"
            "${PORTABLEKIT_ROOT}/third_party/imgui")
        target_link_libraries(${target} PRIVATE SDL3::SDL3 Vulkan::Vulkan)
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
    elseif(APPLE)
        target_link_options(${target} PRIVATE "LINKER:-stack_size,0x4000000")
    endif()
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        JOB_POOL_COMPILE psprecomp_generated)

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
    # Headers only: linking psprecomp_core would give the module its own copy
    # of the runtime state the host already owns.
    target_include_directories(${target} PRIVATE
        "${source_dir}" "${PORTABLEKIT_ROOT}/host" "${PORTABLEKIT_ROOT}/include")
    if(WIN32 OR APPLE)
        # Both linkers want the host binary while linking the module: MSVC for
        # its import library, ld64 as the bundle loader. ELF leaves the host
        # symbols undefined and resolves them when the module is loaded.
        target_link_libraries(${target} PRIVATE ${host_target})
    else()
        add_dependencies(${target} ${host_target})
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
endfunction()
