PortableKit for Windows x64
===========================

PortableKit plays PSP games you own by recompiling them on this PC. It ships
no game and no keys. Everything it writes stays in the "data" folder next to
portablekit.exe: move or delete this whole folder and nothing is left behind.
Keep it in a folder you can write to (not Program Files).


Start
-----

Double-click portablekit.exe. The library window opens.

  Add a game...   choose the .iso of your own PSP game disc (or drop it on
                  the window).
  Play            starts the game at once under the interpreter (slow) and
                  compiles it in the background; the running game switches to
                  compiled code when that is done (first a quick build, then
                  an optimised one). Esc in the game opens the menu.

The compiler it uses is in the "toolchain" folder (LLVM/Clang, llvm-mingw);
nothing needs installing.


Keys (optional)
---------------

Without keys, a game can be added when its disc carries an unencrypted
executable (BOOT.BIN; many do), and saves are kept unencrypted. With keys you
provide yourself, encrypted executables can be decrypted and saves are
written in the PSP's own format. Import a keys file with

    portablekit keys import <your file>

Its format is described in docs/DESKTOP_APP.md of the PortableKit
repository. This program contains no keys and does not say where to get them.


Command line
------------

    portablekit add <image.iso> [--executable <decrypted EBOOT>]
    portablekit list | info <game> | status <game>
    portablekit compile <game> [--opt 0|2|tiered]
    portablekit run <game> [--interpreter]
    portablekit export <game> <folder> [--source-only | --library-only]
    portablekit keys import <file> | keys status
    portablekit cache clear <game>
    portablekit toolchain
    portablekit version

Add --json for machine-readable output, --data-dir <folder> for another data
folder.


Your data
---------

    data\games\<game>\   the game's executable prepared from your disc,
                         settings, saves (ms0)
    data\cache\<game>\   the recompiled and compiled game code, made on this PC

The compiled code and anything "export" writes are made from your copy of the
game and are for your own use only. Never share them.


Report a problem: https://github.com/TeamGDB/PortableKit/issues (never attach
game files). Third-party software and its licenses: licenses\
