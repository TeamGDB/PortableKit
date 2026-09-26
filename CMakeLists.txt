cmake_minimum_required(VERSION 3.20)
# A game's own repository adds this directory and then calls
# portablekit_add_game(); building this directory on its own gives the
# recompiler, the tools and the framework's tests, which is what a framework
# change is checked with.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(PortableKit VERSION 0.1.0 LANGUAGES CXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/BuildLock.cmake)
include(cmake/PortableKit.cmake)

# A compiler cache turns a lost .ninja_deps, a clean or a regenerated corpus
# into seconds of cache hits instead of a full rebuild of the generated units.
option(PSPRECOMP_CCACHE "Compile through ccache when it is installed" ON)
if(PSPRECOMP_CCACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    find_program(PSPRECOMP_CCACHE_PROGRAM ccache)
    if(PSPRECOMP_CCACHE_PROGRAM)
        set(CMAKE_CXX_COMPILER_LAUNCHER "${PSPRECOMP_CCACHE_PROGRAM}")
        # Hash paths inside the checkout relative to it, so checkouts at other
        # paths share cache entries (ccache 4.8+ reads settings given this way).
        execute_process(COMMAND "${PSPRECOMP_CCACHE_PROGRAM}" --version
            OUTPUT_VARIABLE _psprecomp_ccache_version ERROR_QUIET)
        if(_psprecomp_ccache_version MATCHES "version ([0-9]+\\.[0-9]+)"
           AND CMAKE_MATCH_1 VERSION_GREATER_EQUAL "4.8")
            list(APPEND CMAKE_CXX_COMPILER_LAUNCHER "base_dir=${PROJECT_SOURCE_DIR}")
        endif()
        message(STATUS "Compiling through ${PSPRECOMP_CCACHE_PROGRAM}")
    endif()
endif()

# Each generated AOT unit takes well over a gigabyte to compile. Ninja runs the
# compiles of targets in this pool at most this many at a time, whatever -j
# the build was started with. The default allows one per 4 GiB of memory.
cmake_host_system_information(RESULT _psprecomp_memory_mib QUERY TOTAL_PHYSICAL_MEMORY)
math(EXPR _psprecomp_default_jobs "${_psprecomp_memory_mib} / 4096")
if(_psprecomp_default_jobs LESS 1)
    set(_psprecomp_default_jobs 1)
endif()
set(PSPRECOMP_GENERATED_JOBS "${_psprecomp_default_jobs}" CACHE STRING
    "Maximum concurrent compiles of generated AOT code (Ninja generators only)")
if(NOT PSPRECOMP_GENERATED_JOBS MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "PSPRECOMP_GENERATED_JOBS must be a positive integer")
endif()
set_property(GLOBAL APPEND PROPERTY JOB_POOLS psprecomp_generated=${PSPRECOMP_GENERATED_JOBS})

option(PSPRECOMP_BUILD_TESTS "Build reusable PSPRecomp tests" ON)

set(PSPRECOMP_GENERATED_OPT_LEVEL "2" CACHE STRING
    "Optimization level for generated AOT units (0, 1, 2 or 3)")
set_property(CACHE PSPRECOMP_GENERATED_OPT_LEVEL PROPERTY STRINGS 0 1 2 3)
if(NOT PSPRECOMP_GENERATED_OPT_LEVEL MATCHES "^[0-3]$")
    message(FATAL_ERROR "PSPRECOMP_GENERATED_OPT_LEVEL must be 0, 1, 2 or 3")
endif()

option(PSPRECOMP_LTO "Enable link-time/whole-program optimization" OFF)
option(PSPRECOMP_NATIVE_AVX2 "Use AVX2 code generation for optimized MSVC builds" OFF)
option(PSPRECOMP_HOST_AVX "Use AVX for host-side scalar floor/ceil hot paths on MSVC" ON)
option(PSPRECOMP_AOT_ASSUME_NO_WRITE_WATCH
    "Remove per-store write-watch branch from production AOT fast memory" OFF)
option(PSPRECOMP_AOT_PRODUCTION_FASTPATHS
    "Compile out AOT chain diagnostic/counter branches for production builds" OFF)
set(PSPRECOMP_MSVC_CGTHREADS "0" CACHE STRING
    "MSVC LTCG code-generation threads (0 = compiler default)")
if(NOT PSPRECOMP_MSVC_CGTHREADS MATCHES "^[0-9]+$")
    message(FATAL_ERROR "PSPRECOMP_MSVC_CGTHREADS must be a non-negative integer")
endif()

# Bare /MP lets cl.exe spawn one front-end per logical core, and MSBuild /m
# multiplies that by the number of projects it builds in parallel.  On the VCS
# profile each generated AOT unit costs well over a gigabyte at /Ox /Ob3, so the
# unbounded product exhausts physical memory.  Cap the per-cl.exe degree here and
# let the build scripts pick a memory-aware value.
set(PSPRECOMP_MSVC_MP_JOBS "0" CACHE STRING
    "Max cl.exe front-ends (0 = logical-core default, 1 = disable /MP)")
if(NOT PSPRECOMP_MSVC_MP_JOBS MATCHES "^[0-9]+$")
    message(FATAL_ERROR "PSPRECOMP_MSVC_MP_JOBS must be a non-negative integer")
endif()
if(PSPRECOMP_MSVC_MP_JOBS STREQUAL "0")
    set(PSPRECOMP_MSVC_MP_FLAG "/MP")
elseif(PSPRECOMP_MSVC_MP_JOBS STREQUAL "1")
    # Omitting /MP is materially different from /MP1: the latter still creates
    # and communicates with a child compiler process and can fail with D8040.
    set(PSPRECOMP_MSVC_MP_FLAG "")
else()
    set(PSPRECOMP_MSVC_MP_FLAG "/MP${PSPRECOMP_MSVC_MP_JOBS}")
endif()

if(PSPRECOMP_AOT_ASSUME_NO_WRITE_WATCH)
    add_compile_definitions(PSPRECOMP_AOT_ASSUME_NO_WRITE_WATCH=1)
endif()
if(PSPRECOMP_AOT_PRODUCTION_FASTPATHS)
    add_compile_definitions(PSPRECOMP_AOT_PRODUCTION_FASTPATHS=1)
endif()
if(MSVC AND PSPRECOMP_NATIVE_AVX2)
    add_compile_options($<$<CONFIG:Release>:/arch:AVX2>
                        $<$<CONFIG:RelWithDebInfo>:/arch:AVX2>)
endif()

if(PSPRECOMP_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT PSPRECOMP_LTO_SUPPORTED OUTPUT PSPRECOMP_LTO_REASON)
    if(PSPRECOMP_LTO_SUPPORTED)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL ON)
    else()
        message(WARNING "Whole-program optimization unavailable: ${PSPRECOMP_LTO_REASON}")
    endif()
endif()

# A port calls portablekit_add_game() from its own directory, which is a
# sibling of this one, so none of the directory settings above reach its
# targets: not the compiler cache, not the AOT switches, not whole-program
# optimization. Record them for portablekit_add_game() to apply.
get_directory_property(_portablekit_definitions COMPILE_DEFINITIONS)
get_directory_property(_portablekit_options COMPILE_OPTIONS)
set_property(GLOBAL PROPERTY PORTABLEKIT_CXX_COMPILER_LAUNCHER "${CMAKE_CXX_COMPILER_LAUNCHER}")
set_property(GLOBAL PROPERTY PORTABLEKIT_COMPILE_DEFINITIONS "${_portablekit_definitions}")
set_property(GLOBAL PROPERTY PORTABLEKIT_COMPILE_OPTIONS "${_portablekit_options}")
if(PSPRECOMP_LTO AND PSPRECOMP_LTO_SUPPORTED)
    set_property(GLOBAL PROPERTY PORTABLEKIT_IPO ON)
endif()

# Keep the runtime as an object library so profile executables own its objects.
# On Windows this also lets WINDOWS_EXPORT_ALL_SYMBOLS include runtime symbols
# in the executable's import library for separately linked overlay modules.
add_library(psprecomp_core OBJECT
    "${PORTABLEKIT_ROOT}/src/decoder.cpp"
    "${PORTABLEKIT_ROOT}/src/extra_instructions.cpp"
    "${PORTABLEKIT_ROOT}/src/deflate.cpp"
    "${PORTABLEKIT_ROOT}/src/elf32.cpp"
    "${PORTABLEKIT_ROOT}/src/guest_memory.cpp"
    "${PORTABLEKIT_ROOT}/src/interpreter.cpp"
    "${PORTABLEKIT_ROOT}/src/nid_registry.cpp"
    "${PORTABLEKIT_ROOT}/src/program_analysis.cpp"
    "${PORTABLEKIT_ROOT}/src/runtime.cpp"
    "${PORTABLEKIT_ROOT}/src/sha256.cpp"
)
target_include_directories(psprecomp_core PUBLIC ${PORTABLEKIT_ROOT}/include)
target_compile_features(psprecomp_core PUBLIC cxx_std_20)
target_compile_definitions(psprecomp_core PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)
if(MSVC)
    target_compile_options(psprecomp_core PRIVATE /W4 /permissive- /EHsc ${PSPRECOMP_MSVC_MP_FLAG}
        $<$<CONFIG:Release>:/Ob3> $<$<CONFIG:RelWithDebInfo>:/Ob3>)
else()
    target_compile_options(psprecomp_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

add_executable(psp_analyze "${PORTABLEKIT_ROOT}/tools/analyzer_main.cpp")
target_link_libraries(psp_analyze PRIVATE psprecomp_core)
target_compile_definitions(psp_analyze PRIVATE
    PSPRECOMP_DEFAULT_NIDS_CSV="${PORTABLEKIT_ROOT}/configs/nids.csv")

add_executable(psp_recomp "${PORTABLEKIT_ROOT}/tools/codegen_main.cpp")
target_link_libraries(psp_recomp PRIVATE psprecomp_core)

add_executable(dump_function "${PORTABLEKIT_ROOT}/tools/dump_function.cpp")
target_link_libraries(dump_function PRIVATE psprecomp_core)

function(psprecomp_enable_host_avx target)
    # /arch:AVX appears after the directory-wide /arch:AVX2 option in MSVC's
    # command line and therefore silently downgrades the whole target. AVX2
    # already includes the AVX scalar instructions this compatibility switch
    # was introduced for, so do not emit the weaker override in an AVX2 build.
    if(PSPRECOMP_HOST_AVX AND MSVC AND NOT PSPRECOMP_NATIVE_AVX2)
        target_compile_options(${target} PRIVATE /arch:AVX)
    endif()
endfunction()

if(PSPRECOMP_BUILD_TESTS)
    enable_testing()
    add_executable(psprecomp_tests "${PORTABLEKIT_ROOT}/tests/test_main.cpp")
    target_link_libraries(psprecomp_tests PRIVATE psprecomp_core)
    add_dependencies(psprecomp_tests psp_recomp)
    target_compile_definitions(psprecomp_tests PRIVATE
        PSPRECOMP_CODEGEN_PATH="$<TARGET_FILE:psp_recomp>")
    add_test(NAME psprecomp_tests COMMAND psprecomp_tests)
endif()

# Save-data checks that need no game data: crypto test vectors and round trips,
# and the ad hoc server against the client, a raw protocol peer and local
# network discovery, all over the loopback address. Both link a stand-in
# profile, because they exercise host code that asks the profile for names.
if(PSPRECOMP_BUILD_TESTS)
    add_executable(portablekit_savedata_tests
        "${PORTABLEKIT_ROOT}/tests/savedata_tests.cpp"
        "${PORTABLEKIT_ROOT}/tests/test_profile.cpp"
        "${PORTABLEKIT_ROOT}/host/profile.cpp"
        "${PORTABLEKIT_ROOT}/host/save_data/aes128.cpp"
        "${PORTABLEKIT_ROOT}/host/save_data/param_sfo.cpp"
        "${PORTABLEKIT_ROOT}/host/save_data/savedata_crypto.cpp"
        "${PORTABLEKIT_ROOT}/host/crypto_keys_builtin.cpp"
        "${PORTABLEKIT_ROOT}/host/save_data/savedata_store.cpp"
        "${PORTABLEKIT_ROOT}/host/save_data/save_transfer.cpp"
        "${PORTABLEKIT_ROOT}/third_party/tiny_aes/aes.c")
    set_source_files_properties("${PORTABLEKIT_ROOT}/third_party/tiny_aes/aes.c" PROPERTIES LANGUAGE CXX)
    target_include_directories(portablekit_savedata_tests PRIVATE
        "${PORTABLEKIT_ROOT}/host" "${PORTABLEKIT_ROOT}/third_party" "${PORTABLEKIT_ROOT}/include")
    set_target_properties(portablekit_savedata_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    add_test(NAME portablekit_savedata_tests COMMAND portablekit_savedata_tests)

    add_executable(portablekit_adhoc_tests
        "${PORTABLEKIT_ROOT}/tests/adhoc_tests.cpp"
        "${PORTABLEKIT_ROOT}/tests/test_profile.cpp"
        "${PORTABLEKIT_ROOT}/host/profile.cpp"
        "${PORTABLEKIT_ROOT}/host/adhoc/client.cpp"
        "${PORTABLEKIT_ROOT}/host/adhoc/discovery.cpp"
        "${PORTABLEKIT_ROOT}/host/adhoc/server.cpp")
    target_include_directories(portablekit_adhoc_tests PRIVATE
        "${PORTABLEKIT_ROOT}/host" "${PORTABLEKIT_ROOT}/include")
    find_package(Threads REQUIRED)
    target_link_libraries(portablekit_adhoc_tests PRIVATE Threads::Threads)
    if(WIN32)
        target_link_libraries(portablekit_adhoc_tests PRIVATE ws2_32 iphlpapi)
    endif()
    set_target_properties(portablekit_adhoc_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    add_test(NAME portablekit_adhoc_tests COMMAND portablekit_adhoc_tests)
endif()

# Editions of a game (ProfileVariant) against the stand-in profile.
if(PSPRECOMP_BUILD_TESTS)
    add_executable(portablekit_profile_tests
        "${PORTABLEKIT_ROOT}/tests/profile_tests.cpp"
        "${PORTABLEKIT_ROOT}/tests/test_profile.cpp"
        "${PORTABLEKIT_ROOT}/host/profile.cpp")
    target_include_directories(portablekit_profile_tests PRIVATE "${PORTABLEKIT_ROOT}/host" "${PORTABLEKIT_ROOT}/include")
    set_target_properties(portablekit_profile_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    add_test(NAME portablekit_profile_tests COMMAND portablekit_profile_tests)
endif()

# Fixed-size memory pools (sceKernel*Fpl): their bookkeeping, with no kernel.
if(PSPRECOMP_BUILD_TESTS)
    add_executable(portablekit_pool_tests
        "${PORTABLEKIT_ROOT}/tests/pool_tests.cpp"
        "${PORTABLEKIT_ROOT}/host/kernel/fixed_pool.cpp")
    target_include_directories(portablekit_pool_tests PRIVATE "${PORTABLEKIT_ROOT}/host")
    set_target_properties(portablekit_pool_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    add_test(NAME portablekit_pool_tests COMMAND portablekit_pool_tests)
endif()

# Each platform's settings defaults.
if(PSPRECOMP_BUILD_TESTS)
    add_executable(portablekit_settings_tests
        "${PORTABLEKIT_ROOT}/tests/settings_defaults_tests.cpp"
        "${PORTABLEKIT_ROOT}/tests/test_profile.cpp"
        "${PORTABLEKIT_ROOT}/host/profile.cpp"
        "${PORTABLEKIT_ROOT}/host/settings/settings.cpp"
        "${PORTABLEKIT_ROOT}/host/install/user_data.cpp"
        "${PORTABLEKIT_ROOT}/host/input/bindings.cpp")
    target_include_directories(portablekit_settings_tests PRIVATE "${PORTABLEKIT_ROOT}/host")
    target_link_libraries(portablekit_settings_tests PRIVATE psprecomp_core)
    set_target_properties(portablekit_settings_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    add_test(NAME portablekit_settings_tests COMMAND portablekit_settings_tests)
endif()

# The on-screen touch controls: layout, fingers and the stick's maths.
if(PSPRECOMP_BUILD_TESTS)
    add_executable(portablekit_touch_tests
        "${PORTABLEKIT_ROOT}/tests/touch_controls_tests.cpp"
        "${PORTABLEKIT_ROOT}/host/input/touch_controls.cpp")
    target_include_directories(portablekit_touch_tests PRIVATE "${PORTABLEKIT_ROOT}/host")
    set_target_properties(portablekit_touch_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    add_test(NAME portablekit_touch_tests COMMAND portablekit_touch_tests)
endif()

# Editions of a game (ProfileVariant) against the stand-in profile.
if(PSPRECOMP_BUILD_TESTS)
    add_executable(portablekit_profile_tests
        "${PORTABLEKIT_ROOT}/tests/profile_tests.cpp"
        "${PORTABLEKIT_ROOT}/tests/test_profile.cpp"
        "${PORTABLEKIT_ROOT}/host/profile.cpp")
    target_include_directories(portablekit_profile_tests PRIVATE "${PORTABLEKIT_ROOT}/host" "${PORTABLEKIT_ROOT}/include")
    set_target_properties(portablekit_profile_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    add_test(NAME portablekit_profile_tests COMMAND portablekit_profile_tests)
endif()

# Texture pack import: finding a pack in the layouts packs come in, the
# checks made before copying, and the copy, swap and backup; and the PSP's DXT
# block layout. No game data.
if(PSPRECOMP_BUILD_TESTS)
    add_executable(portablekit_texture_pack_tests
        "${PORTABLEKIT_ROOT}/tests/texture_pack_tests.cpp"
        "${PORTABLEKIT_ROOT}/tests/test_profile.cpp"
        "${PORTABLEKIT_ROOT}/host/profile.cpp"
        "${PORTABLEKIT_ROOT}/host/gpu/texture_decode.cpp"
        "${PORTABLEKIT_ROOT}/host/gpu/texture_pack.cpp"
        "${PORTABLEKIT_ROOT}/host/gpu/texture_pack_import.cpp")
    target_include_directories(portablekit_texture_pack_tests PRIVATE
        "${PORTABLEKIT_ROOT}/host" "${PORTABLEKIT_ROOT}/host/gpu" "${PORTABLEKIT_ROOT}/third_party")
    find_package(Threads REQUIRED)
    target_link_libraries(portablekit_texture_pack_tests PRIVATE psprecomp_core Threads::Threads)
    set_target_properties(portablekit_texture_pack_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    add_test(NAME portablekit_texture_pack_tests COMMAND portablekit_texture_pack_tests)
endif()

# Renderer helpers that need no GPU.
if(PSPRECOMP_BUILD_TESTS)
    add_executable(portablekit_render_tests "${PORTABLEKIT_ROOT}/tests/render_tests.cpp")
    target_include_directories(portablekit_render_tests PRIVATE "${PORTABLEKIT_ROOT}/host")
    target_link_libraries(portablekit_render_tests PRIVATE psprecomp_core)
    set_target_properties(portablekit_render_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    add_test(NAME portablekit_render_tests COMMAND portablekit_render_tests)
endif()

# Frame interpolation checks: matching draws, cuts, blending, the present
# clock and the frame rate governor.
if(PSPRECOMP_BUILD_TESTS)
    add_executable(portablekit_interpolation_tests
        "${PORTABLEKIT_ROOT}/tests/interpolation_tests.cpp"
        "${PORTABLEKIT_ROOT}/host/gpu/frame_interpolation.cpp"
        "${PORTABLEKIT_ROOT}/host/gpu/frame_pacing.cpp")
    target_compile_features(portablekit_interpolation_tests PRIVATE cxx_std_20)
    target_include_directories(portablekit_interpolation_tests PRIVATE
        "${PORTABLEKIT_ROOT}/host" "${PORTABLEKIT_ROOT}/include")
    set_target_properties(portablekit_interpolation_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    add_test(NAME portablekit_interpolation_tests COMMAND portablekit_interpolation_tests)
endif()

# Keyboard and mouse bindings: names, settings.ini spelling, the menu's
# assignment rules and what held keys press. No SDL or game data.
if(PSPRECOMP_BUILD_TESTS)
    add_executable(portablekit_input_tests
        "${PORTABLEKIT_ROOT}/tests/input_tests.cpp"
        "${PORTABLEKIT_ROOT}/host/input/bindings.cpp")
    target_include_directories(portablekit_input_tests PRIVATE "${PORTABLEKIT_ROOT}/host")
    set_target_properties(portablekit_input_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    add_test(NAME portablekit_input_tests COMMAND portablekit_input_tests)
endif()

# Fast loading's decision: when a load runs ahead of real time.
if(PSPRECOMP_BUILD_TESTS)
    add_executable(portablekit_fast_loading_tests
        "${PORTABLEKIT_ROOT}/tests/fast_loading_tests.cpp"
        "${PORTABLEKIT_ROOT}/host/kernel/load_detector.cpp")
    target_include_directories(portablekit_fast_loading_tests PRIVATE "${PORTABLEKIT_ROOT}/host")
    set_target_properties(portablekit_fast_loading_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    add_test(NAME portablekit_fast_loading_tests COMMAND portablekit_fast_loading_tests)
endif()

# The system layer belongs to a game's target, so nothing above compiles
# host/main.cpp or most of host/. That let two compile errors through until a
# second game was built. This target builds the whole backend-independent
# system layer against the stand-in profile and links it, so a framework build
# catches them on its own. It is not a port: no renderer, no FFmpeg, no game,
# and it is never meant to be run.
if(PSPRECOMP_BUILD_TESTS)
    set(_check_nids "${CMAKE_CURRENT_BINARY_DIR}/host_check_nids/nid_table.inc")
    add_custom_command(
        OUTPUT "${_check_nids}"
        COMMAND ${CMAKE_COMMAND} "-DINPUT=${PORTABLEKIT_ROOT}/configs/nids.csv" -DOUTPUT=${_check_nids}
                -P "${PORTABLEKIT_ROOT}/tools/embed_nids.cmake"
        DEPENDS "${PORTABLEKIT_ROOT}/configs/nids.csv" "${PORTABLEKIT_ROOT}/tools/embed_nids.cmake"
        COMMENT "Embedding the NID table for the host check")

    set(_check_sources
        "${PORTABLEKIT_ROOT}/host/main.cpp"
        "${PORTABLEKIT_ROOT}/host/generated_stub.cpp"
        "${PORTABLEKIT_ROOT}/tests/test_profile.cpp"
        "${PORTABLEKIT_ROOT}/host/crypto_keys_builtin.cpp"
        "${_check_nids}")
    foreach(_source IN LISTS PORTABLEKIT_HOST_SOURCES)
        list(APPEND _check_sources "${PORTABLEKIT_ROOT}/${_source}")
    endforeach()

    # tiny-AES-c is C; the project only enables C++, which compiles it unchanged.
    set_source_files_properties("${PORTABLEKIT_ROOT}/third_party/tiny_aes/aes.c" PROPERTIES LANGUAGE CXX)
    add_executable(portablekit_host_check ${_check_sources})
    target_compile_features(portablekit_host_check PRIVATE cxx_std_20)
    target_include_directories(portablekit_host_check PRIVATE
        "${PORTABLEKIT_ROOT}/host" "${PORTABLEKIT_ROOT}/third_party"
        "${CMAKE_CURRENT_BINARY_DIR}/host_check_nids")
    target_link_libraries(portablekit_host_check PRIVATE psprecomp_core ${CMAKE_DL_LIBS})
    find_package(Threads REQUIRED)
    target_link_libraries(portablekit_host_check PRIVATE Threads::Threads)
    if(WIN32)
        target_link_libraries(portablekit_host_check PRIVATE ws2_32 iphlpapi)
    endif()
    set_target_properties(portablekit_host_check PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()
