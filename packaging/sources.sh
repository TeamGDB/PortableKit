# Third-party sources a release bundles, pinned by version and SHA-256.
# Sourced by the release scripts (scripts/release_macos.sh,
# scripts/release_linux.sh and packaging/linux/build_in_sdk.sh). When a
# version changes here or in cmake/FFmpeg.cmake, a game's
# THIRD_PARTY_NOTICES.md must change to match; the release scripts refuse to
# package when they disagree.

SDL3_VERSION=3.4.16
SDL3_URL="https://github.com/libsdl-org/SDL/releases/download/release-${SDL3_VERSION}/SDL3-${SDL3_VERSION}.tar.gz"
SDL3_SHA256=7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68

# FFmpeg is not pinned here: the build itself downloads, checks and builds the
# LGPL-only FFmpeg it bundles (cmake/FFmpeg.cmake, PORTABLEKIT_FFMPEG=bundled).

# Japanese text needs a CJK font. Releases carry one as a fallback for systems
# (and Flatpak runtimes) without one.
NOTO_CJK_TAG=Sans2.004
NOTO_CJK_URL="https://github.com/notofonts/noto-cjk/raw/${NOTO_CJK_TAG}/Sans/OTF/Japanese/NotoSansCJKjp-Regular.otf"
NOTO_CJK_SHA256=68a3fc98800b2a27b371f2fb79991daf3633bd89309d4ffaa6946fd587f375b5
NOTO_CJK_LICENSE_URL="https://github.com/notofonts/noto-cjk/raw/${NOTO_CJK_TAG}/LICENSE"
NOTO_CJK_LICENSE_SHA256=6a73f9541c2de74158c0e7cf6b0a58ef774f5a780bf191f2d7ec9cc53efe2bf2

# The build environment: the Steam Runtime 3 "sniper" SDK (Debian 11, glibc
# 2.31), so the result runs on SteamOS and on most distributions of the last
# few years, pinned by digest (the 3.0.20260805 SDK) so a rebuild uses the
# same compilers; override with PORTABLEKIT_SDK_IMAGE.
SDK_IMAGE="${PORTABLEKIT_SDK_IMAGE:-registry.gitlab.steamos.cloud/steamrt/sniper/sdk@sha256:1c33c507bc75d012e77df5727f93b0d5b8c3f7c8d4142ba5f7a16882cc92e014}"

# The Flatpak runtime the bundle targets.
FLATPAK_RUNTIME_VERSION=25.08

# --- macOS ------------------------------------------------------------------

# The oldest macOS the release runs on. Everything bundled is built for it or
# rewritten to it, after checking that every system symbol the binaries import
# exists in that release's SDK (see release_macos.sh).
MACOS_DEPLOYMENT_TARGET=13.0

# The Vulkan loader, built from source for the deployment target above. It
# finds MoltenVK through the driver manifest in the app bundle's
# Contents/Resources/vulkan/icd.d.
VULKAN_SDK_TAG=vulkan-sdk-1.4.357.0
VULKAN_LOADER_URL="https://github.com/KhronosGroup/Vulkan-Loader/archive/refs/tags/${VULKAN_SDK_TAG}.tar.gz"
VULKAN_LOADER_SHA256=54f2537df22313768da0317dda2abdaaab7711b4081c48c869a79db343d0ae70
VULKAN_HEADERS_URL="https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/${VULKAN_SDK_TAG}.tar.gz"
VULKAN_HEADERS_SHA256=e87dce08116151f6b6d7de6b6faf41498e87e6cf848ff16fa3bd5402190ad4a3

# MoltenVK, Vulkan on Metal: the Khronos release build, unmodified except that
# only its arm64 slice is kept.
MOLTENVK_VERSION=1.4.2
MOLTENVK_URL="https://github.com/KhronosGroup/MoltenVK/releases/download/v${MOLTENVK_VERSION}/MoltenVK-macos.tar"
MOLTENVK_SHA256=f95765a6229cb7b915990a2890ce12ebe36a730b021545d3d52ae69ce4c4024e
