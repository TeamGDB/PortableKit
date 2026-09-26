# HLE extension modules

An HLE extension module implements PSP system functions -- the HLE a game's imports reach, named by library and NID -- in a library built separately from PortableKit's own sources. It is linked into a program at build time. It can:

- **fill in** functions PortableKit leaves as logging stubs, and
- **override** PortableKit's own implementation of a function, when it says so for that function.

PortableKit knows nothing about any particular module. A build without modules is exactly the build it was before; a build with them says at startup which modules it carries and which of their functions are active.

## Writing a module

A module is a directory with a `CMakeLists.txt` and its sources:

```text
my_module/
  CMakeLists.txt
  my_module.cpp
  LICENSE            the module's own licence
```

```cmake
portablekit_add_hle_extension(my_module
    TITLE "My module"        # what the logs call it; the name when omitted
    VERSION "0.3"            # optional
    LICENSE "MIT"            # required: the licence the module states
    SOURCES my_module.cpp)
```

`portablekit_add_hle_extension()` (in [`cmake/PortableKit.cmake`](../cmake/PortableKit.cmake)) makes a static library `portablekit_hle_extension_<name>` from `SOURCES`, with PortableKit's `include/` and `host/` on its include path and the framework's compiler settings. It also takes `INCLUDE_DIRECTORIES`, `LINK_LIBRARIES` and `COMPILE_DEFINITIONS` for the module's own needs. `<name>` is a C identifier, unique in the build.

The sources include [`include/portablekit/hle_extension.hpp`](../include/portablekit/hle_extension.hpp) and define one entry function with the same name:

```cpp
#include "portablekit/hle_extension.hpp"

namespace ext = portablekit::hle_extension;

PORTABLEKIT_HLE_EXTENSION(my_module) {
    // Fills in: used only where PortableKit has no implementation.
    registry.add("sceHttp", 0xAB1ABE07u, "sceHttpInit", [](ext::Runtime &rt, ext::AllegrexContext &ctx) {
        const std::uint32_t pool_size = ext::arg(ctx, 0);
        // ... read and write guest memory through rt.memory() ...
        ext::finish(ctx, 0u);
    });
    // Overrides: replaces PortableKit's implementation.
    registry.override_builtin("UtilsForUser", 0x79D1C3FAu, "sceKernelDcacheWritebackAll", handler);
}
```

[`examples/hle_extension/`](../examples/hle_extension/) is a complete module of one fill-in, one override and one chained override, all behaving as PortableKit already does.

### The interface

Everything a module may rely on is in `include/portablekit/hle_extension.hpp`. `PORTABLEKIT_HLE_EXTENSION_INTERFACE` is raised when it changes incompatibly.

| | |
| --- | --- |
| `Registry::add(library, nid, name, handler, mode)` | Adds a function. `name` is for logs and may be empty where [`configs/nids.csv`](../configs/nids.csv) names the NID; a name the table lacks is added to it. `mode` is `Mode::FillIn` (the default) or `Mode::Override`. |
| `Registry::override_builtin(library, nid, name, handler)` | `add` with `Mode::Override`. |
| `Registry::override_chained(library, nid, name, chained)` | Wraps the implementation already bound: `chained` is `void(Runtime &, AllegrexContext &, const Handler &previous)`, and either handles the call itself or passes it on unchanged with `previous(rt, ctx)` as its last act (see below). Since interface version 2. |
| `Handler` | `void(Runtime &, AllegrexContext &)`, the signature of PortableKit's own handlers. |
| `arg(ctx, i)`, `arg64(ctx, i)` | Arguments in a0-a3, t0-t3; a 64-bit one in an even-aligned pair. |
| `Runtime::memory()`, `read_cstring()` | Guest memory by guest address. |
| `finish(ctx, v)`, `finish64(ctx, v)` | Return a result. The last thing a handler does, unless it blocks. |
| `wait_host(ctx, timeout, poll)` | Block the calling thread until `poll` returns a value, which becomes the result. For work the host does meanwhile (a socket, a worker thread). |
| `delay(ctx, us, result)` | Block the calling thread for a time. |
| `call_guest(ctx, function, {args...}, on_return)` | Call back into the game in the calling thread, as a library does; `on_return` then finishes the import or calls again. |
| `queue_guest_call(function, {args...}, on_return)` | Queue a call in interrupt context, as the system delivers a handler the game registered (an event, a state change). |
| Arguments of either | Up to eight (`kMaxGuestArguments`): a0-a3, then t0-t3. |
| `current_thread_id()` | The calling guest thread's id. |
| `now_us()` | Emulated time since boot. |
| `log_once(key, message)` | Log a line the first time `key` is seen. |
| `env(name)` | The port's variable `<PREFIX>_<name>`. |
| `Registry::declare_key(name, sha256, size)` | Declares a key the module reads (see [Keys](#keys)). Since interface version 3. |
| `key(name)`, `key_bytes(name)`, `kirk_key(slot)` | The program's keys by their keys-file name: PortableKit's own and declared ones; empty when missing. |
| `read_open_file(fd, offset, bytes)` | The raw bytes of an open guest file, without moving its position. |
| `set_file_filter(fd, filter)` | What the game reads from an open file instead of its bytes (see [Filtered files](#filtered-files)). |

Handlers run on the emulation thread, one at a time. A module may include PortableKit's `host/` headers to reach further than this, but those change without notice; a need that comes up twice belongs in the header instead.

## Building modules into a program

Give the build the modules' directories, as a CMake list:

```sh
cmake -S . -B out -G Ninja -DPORTABLEKIT_HLE_EXTENSION_DIRS="/path/to/my_module;/path/to/other"
cmake -S apps/portablekit -B out-app -G Ninja -DPORTABLEKIT_HLE_EXTENSION_DIRS=/path/to/my_module
```

The framework's `CMakeLists.txt` adds each directory, in order, before any program is made, and every program `portablekit_add_game()` makes afterwards -- a port, or the desktop app with its automatic profile -- links all of them. A port may also call `portablekit_add_hle_extension()` itself, between adding PortableKit and `portablekit_add_game()`. A relative directory is taken relative to the top-level source directory. Empty, the default, adds nothing.

## Precedence

At startup, `install_system()` registers PortableKit's HLE, then the profile's `register_extra_hle` hook, and then applies the modules in the order the build was given them:

1. A **fill-in** is bound only where neither PortableKit nor the profile implements the function. Where one does, theirs stays and the startup log says the module's function is not used.
2. An **override** is bound whether or not the function is implemented. Where it replaces an implementation it is logged, once per function, at startup.
3. A **chained override** wraps whatever is bound when it is applied -- PortableKit's implementation, the profile's, or an earlier module's -- and always binds. Chained overrides of one function from several modules therefore compose: the last module listed is called first, and each passes on what it does not handle. Each is logged at startup with what it passes calls to. Where nothing was bound before, `previous` does what a logging stub does: logs the call once and returns 0.
4. Otherwise, between modules and within one, **the first to add a function keeps it**; a later addition is logged as not used.

Only after that are the remaining imports bound to logging stubs, so a module's function is never replaced by a stub.

### Handling part of a function

A chained override lets a module take over one case of a function and leave the rest to the implementation it replaces, for example one action of a utility dialog:

```cpp
PORTABLEKIT_HLE_EXTENSION(my_module) {
    registry.override_chained("sceUtility", nid, "sceUtilityNetconfInitStart",
        [](ext::Runtime &rt, ext::AllegrexContext &ctx, const ext::Handler &previous) {
            const std::uint32_t action = rt.memory().load32(ext::arg(ctx, 0) + kActionOffset);
            if (action != kMyAction) return previous(rt, ctx);  // unchanged, as the game called it
            // ... handle this action ...
            ext::finish(ctx, 0u);
        });
}
```

`previous` must be called with the caller's registers as the handler found them, and as its last act, in place of `finish()`.

## Keys

A module that needs keys the console's crypto uses declares each name it reads, at registration:

```cpp
registry.declare_key("vendor.table", "<sha256 of the value, lower-case hex>", 0x90);  // fingerprint and size optional
```

and reads it at run time with `ext::key_bytes("vendor.table")` (`ext::key` for 16-byte keys, `ext::kirk_key(slot)` for `kirk.aes.<slot>`). The names are the keys file's, not case-sensitive, and a lookup also finds PortableKit's own keys (`kirk.aes.5D`, `kirk.cmd1`, `savedata.2`, `tag.<tag>`).

- **Desktop app.** The player's keys file accepts a declared name like one of PortableKit's own: it checks the length and, when one is declared, the fingerprint, and a wrong value is a problem that refuses the file on import. A declared `kirk.aes.<slot>` is also a KIRK key for PortableKit. A name nothing declares is a warning and is left out, but no longer refuses the file. `portablekit keys status` lists the declared keys by module, present or missing (`--json`: `extension_keys`). A name PortableKit checks itself keeps PortableKit's check.
- **Ports.** PortableKit's own keys are compiled in. Declared keys come from an optional keys file in the same format: `<PREFIX>_KEYS`, else `keys.txt` in the data folder (`<PREFIX>_DATA_DIR`, else the port's own); names nothing declares are left alone there. Startup says `HLE extension keys: N of M declared keys present (<path>)`, with a line for each wrong one.

An entry function may be called more than once (the keys are collected before the game starts); it only adds to its registry.

## Filtered files

`set_file_filter(fd, filter)` puts a `FileFilter` (its `size()` and `read(offset, destination, length)`) between the game and an open file, such as the plain text of an encrypted container. While it is set, PortableKit's own `sceIoRead`, `sceIoReadAsync`, `sceIoLseek`, `sceIoLseek32` (and their Async forms) and the disc seek ioctl `0x01010005` work on what the filter gives: positions and sizes count its bytes, and asynchronous results come with `sceIoWaitAsync`/`sceIoPollAsync` as usual. The position is left where it was when the filter is set. The filter reads the file's own bytes with `read_open_file`, which a filter does not change. A null filter restores the file; closing it (`sceIoClose`, `sceIoCloseAsync`) drops the filter. The raw disc device (`umd0:` and `sce_lbn` sector files, which count sectors) and directories take no filter. Writing to a filtered file writes the file's own bytes.

## What the log says

```text
HLE extension: Example HLE extension 1.0 (MIT)
HLE extensions: 3 functions (2 overriding built-ins) from Example HLE extension
[hle-extension] UtilsForUser::sceKernelDcacheWritebackAll is Example HLE extension's, overriding the built-in
[hle-extension] UtilsForUser::sceKernelDcacheWritebackInvalidateAll is Example HLE extension's, passing what it does not handle to the built-in
HLE imports: 269 total, 379 implemented, 38 logging stubs; 2 imports served by HLE extensions
```

With `<PREFIX>_LIST_STUBS`, the list of unimplemented imports is followed by the imports extension modules serve, each with its module and whether it fills in or overrides.

`<PREFIX>_NO_HLE_EXTENSIONS=1` ignores every module for that run, leaving exactly PortableKit's own HLE, so the two can be compared on the same build:

```text
HLE extensions: 1 module linked, ignored (PORTABLEKIT_NO_HLE_EXTENSIONS)
```

## Tests

`portablekit_io_filter_tests` runs PortableKit's own IoFileMgrForUser handlers on a memory stick folder with a filter: synchronous and asynchronous reads and seeks, the raw bytes, removing it and closing. `portablekit_hle_extension_tests` builds the example module into a runtime with no kernel and checks a fill-in, an override, a fill-in left to the built-in, two modules adding one function, chained overrides, the log lines, and declared keys: collecting them, checking a value, looking keys up and reading a port's keys file.

## Licensing

An extension module may carry its own licence, which it states in `LICENSE` of its `portablekit_add_hle_extension()` call and which the program prints at startup. PortableKit's own code stays under its MIT licence. A build that includes a module is distributed under the terms that combination requires; whoever distributes such a build is responsible for meeting them, including shipping the module's licence text and, where its licence asks for it, its source.
