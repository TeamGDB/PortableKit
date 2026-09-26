PortableKit for macOS (Apple Silicon)
=====================================

PortableKit plays PSP games you own by recompiling them on this Mac. It ships
no game and no keys.

Needs: a Mac with Apple Silicon (M1 or newer) and macOS @MINIMUM_SYSTEM_VERSION@ or newer.
Compiling games needs Apple's free Command Line Tools; PortableKit says how
to install them when they are missing, and plays games under its interpreter
(slower) until then.


Install
-------

Drag PortableKit into a folder you can write to, such as Applications or a
folder of its own. Everything it writes goes to "PortableKit Data" next to
PortableKit.app: move or delete both and nothing is left behind.


First start
-----------

This build is not notarized by Apple, so the first time you open it macOS
says that it cannot verify the developer and offers only Done or Move to
Trash. Choose Done, then:

  1. Open System Settings > Privacy & Security.
  2. Scroll down to the message about PortableKit and click "Open Anyway".
  3. Confirm with your password or Touch ID, then "Open Anyway" once more.

Or, from Terminal:

    xattr -dr com.apple.quarantine /Applications/PortableKit.app

The library window then opens. "Add a game..." takes the .iso of your own
PSP game disc; Play starts it under the interpreter at once and compiles it
in the background. Esc in the game opens the menu.


Keys (optional)
---------------

Without keys, a game can be added when its disc carries an unencrypted
executable (BOOT.BIN; many do), and saves are kept unencrypted. The keys
file's format is described in docs/DESKTOP_APP.md of the PortableKit
repository. This program contains no keys and does not say where to get them.


Command line
------------

    PortableKit.app/Contents/MacOS/PortableKit help

lists the commands (add, list, compile, run, export, keys, version).


Report a problem: https://github.com/TeamGDB/PortableKit/issues (never attach
game files). Third-party software and its licenses:
PortableKit.app/Contents/Resources/licenses/
