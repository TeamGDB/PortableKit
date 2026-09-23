# Source provenance and license boundaries

PortableKit separates independently written framework and profile code from third-party components with their own licenses.

## Framework

The reusable PortableKit runtime, decoder, analyzer and code generator in the repository root are distributed under the MIT License. Game-specific addresses and implementations are not accepted in the reusable core.

The project may use public hardware documentation, observable program behavior and other implementations as technical references. Reference material is used to understand behavior and architecture; source code from incompatible copyleft projects is not imported into the MIT framework.

## Decryption

The framework contains no general-purpose EBOOT or PRX decrypter. The installer (`host/install`) prepares one game's executable from the player's own disc image: it accepts exactly one encrypted file per profile, identified by the SHA-256 the profile declares, decrypts it with the fixed crypto-engine keys the file format uses and the key the profile supplies for its release's tag, and checks its output against the SHA-256 of the executable the profile was generated from. Any other file is refused. It was written for this project from public descriptions of the file format and the crypto primitives; no code from other implementations was copied or adapted. The key values are published technical constants. Its AES implementation is tiny-AES-c (public domain), kept with its notice in `third_party/tiny_aes`. Developers can still prepare the executable outside the project and supply it through the profile's game directory (`<PREFIX>_GAME_DIR`).

## Save data

`host/save_data` implements the PSP save-data format — the `PARAM.SFO` layout, the encryption of the data file and the hashes that protect a save — so that saves can be exchanged with a PSP. It was written for this project from public descriptions of the PSP save data format, with no code copied or adapted from other implementations, and checked against saves made by a PSP. Its AES-128 cipher is the same tiny-AES-c the installer uses; the self-tests check it against FIPS-197 and the CMAC built on it against RFC 4493. The fixed key values it uses are published technical constants. None of it decrypts executables.

## Ad hoc networking

`host/adhoc` and `host/hle/hle_adhoc.cpp` let the game's ad hoc play reach other players through the PSP ad hoc servers players already run. The client was written for this project from the protocols' documented and observed behaviour: the servers' published packet layouts, opcodes and ports, and the traffic between the game and a server. No code was copied or adapted from other implementations. The PSP library calls it serves follow their public API descriptions and the game's own calls, traced while it runs.

The built-in server (`server.cpp`, used by *Host a session* and `--adhoc-server`) was also written for this project from the same protocols' documented and observed behaviour: the packet layouts, opcodes, ports and the order of the messages a server sends, checked against the project's own client. No code, structure or text was copied or adapted from other server implementations. The local network discovery protocol (`discovery.cpp`) is the project's own design.

## Texture packs

`host/gpu/texture_pack.cpp` and `replacement_textures.cpp` load HD texture packs made for PPSSPP, in its `textures.ini` format. They were written for this project. The format was learnt from PPSSPP's public documentation and pull requests about texture replacement, from reading PPSSPP's GPL-licensed source to find the format's facts (the layout of a key, the hash seeds, how the palette and the texture's dimensions enter the key, the order in which wildcard keys are tried, the ini sections and options), and from the keys a real community pack for Monster Hunter Portable 3rd HD Ver. uses, which the implementation reproduces. Only those facts and constants were taken; no code was copied, adapted or translated line by line. PPSSPP's `quick` hash is not implemented. Hashing uses xxHash, and PNG files are read and written with stb_image and stb_image_write, all unmodified (table below).

## Profile code

A profile owns its generated AOT corpus, address-specific lowering, HLE behavior and native fast paths. Those files stay in the profile's own repository, and generated code is never committed at all, so they do not become hidden dependencies of the generic framework.

## Third-party components

Third-party source, binary dependencies, shader code and notices stay beside the code that needs them. Their original copyright and license notices must be preserved.

| Component | Used by | License | How it is included |
| --- | --- | --- | --- |
| [Dear ImGui](https://github.com/ocornut/imgui) 1.92.9b | in-game menu and setup screens | MIT | Unmodified copy in `third_party/imgui` with its `LICENSE.txt` |
| [tiny-AES-c](https://github.com/kokke/tiny-AES-c) | installer and save data | Unlicense (public domain) | Unmodified copy in `third_party/tiny_aes` with its `UNLICENSE` |
| [stb_truetype](https://github.com/nothings/stb) 1.26 | game text | MIT or public domain | Single header, `third_party/stb_truetype.h`, notice at its end |
| [stb_image](https://github.com/nothings/stb) 2.30 and stb_image_write 1.16 | texture pack images (PNG only) | MIT or public domain | Single headers from commit `2c980bb`, `third_party/stb_image.h` and `stb_image_write.h`, notices at their ends |
| [xxHash](https://github.com/Cyan4973/xxHash) 0.8.3 | texture pack keys | BSD-2-Clause | Unmodified `xxhash.h` of release v0.8.3 in `third_party/xxhash` with its `LICENSE` |
| [SDL3](https://www.libsdl.org/) | window, input, audio output | zlib | External dependency, linked dynamically; Linux releases ship an unmodified build in `lib/` |
| [FFmpeg](https://ffmpeg.org/) (`libavcodec`, `libavutil`) | ATRAC3 music and H.264/ATRAC3plus movie decoding (`host/audio/atrac_decoder.cpp`, `host/movie/avc_decoder.cpp`) | LGPL-2.1-or-later (the Windows build: LGPL-3.0-or-later) | Linked dynamically; no FFmpeg source is in the repository. By default the build downloads the unmodified [FFmpeg 7.1.5 release](https://ffmpeg.org/releases/ffmpeg-7.1.5.tar.xz) (SHA-256 `de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f`) and builds it with `--enable-shared --disable-static --disable-programs --disable-doc --disable-avdevice --disable-avformat --disable-avfilter --disable-swscale --disable-swresample --disable-network --disable-autodetect --disable-everything --enable-decoder=atrac3,atrac3p,h264 --disable-x86asm --disable-debug` (`cmake/FFmpeg.cmake`). On Windows it uses the unmodified prebuilt [LGPL shared build](https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-06-30-13-34/ffmpeg-n7.1.5-1-g7d0e842004-win64-lgpl-shared-7.1.zip) of FFmpeg 7.1.5 (commit `7d0e842004`) from [BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds). The licence text and a note with the source location and configuration are copied next to the libraries. `-DPORTABLEKIT_FFMPEG=system` uses an FFmpeg found through `pkg-config` instead |
| [Noto Sans CJK JP](https://github.com/notofonts/noto-cjk) | a port's releases: fallback font for Japanese text | SIL Open Font License 1.1 | Downloaded by the release build, shipped in `fonts/`; not in the repository |

A port carries these notices in its own packaging, together with the license texts.

## Binary releases

The source is MIT, but a binary release of a port is a combination, and it has to meet every component's terms:

- **Notices.** Ship the licence text of every component in the table above that the binary contains or bundles: Dear ImGui, stb, xxHash and tiny-AES-c are compiled in; SDL3 and FFmpeg are shipped as shared libraries.
- **FFmpeg (LGPL).** It must stay dynamically linked and replaceable by the user: ship the libraries as separate files, unmodified, with their licence text and a note saying where their exact source is (the build writes that note next to the libraries). Do not link FFmpeg statically into a release, and do not enable its GPL or non-free parts; the bundled build refuses to. The prebuilt Windows libraries are LGPL-3.0-or-later, which additionally means a user must be able to replace them and run the result.
- **Patents.** The bundled FFmpeg decodes H.264 video. Codec patents are separate from copyright licences, and the LGPL does not grant them; whoever publishes binaries is responsible for any patent licensing that applies where they distribute. This is not legal advice.
- **Fonts.** A release that ships Noto Sans CJK ships its OFL-1.1 licence with it.



## Contribution rule

Do not paste or adapt source from a project whose license is incompatible with the destination file. Reimplement required behavior from specifications, observations or independently documented semantics, and record the source of third-party material when it is intentionally included under a compatible license.
