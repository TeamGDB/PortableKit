# An HLE extension module, as small as one can be (docs/HLE_EXTENSIONS.md).
# Build it into a program with
#   -DPORTABLEKIT_HLE_EXTENSION_DIRS=<PortableKit>/examples/hle_extension
portablekit_add_hle_extension(example
    TITLE "Example HLE extension"
    VERSION "1.0"
    LICENSE "MIT"
    SOURCES example_extension.cpp)
