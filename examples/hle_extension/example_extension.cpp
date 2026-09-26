// An example HLE extension module: one function filled in, one overridden.
// Both behave as PortableKit already does, so building it into a program
// changes nothing a game can see; it shows the mechanism and is what
// tests/hle_extension_tests.cpp checks it with.

#include "portablekit/hle_extension.hpp"

namespace {

namespace ext = portablekit::hle_extension;

// sceHttpsLoadDefaultCert, which PortableKit leaves to a logging stub: the
// stub already returns 0, and this returns 0 without the log line.
void load_default_cert(ext::Runtime &, ext::AllegrexContext &ctx) { ext::finish(ctx, 0u); }

// sceKernelDcacheWritebackAll: PortableKit's own returns 0 too, since the host
// has no data cache to write back. The override says once that it ran.
void writeback_data_cache(ext::Runtime &, ext::AllegrexContext &ctx) {
    ext::log_once("example-dcache", "[example] sceKernelDcacheWritebackAll handled by the example extension");
    ext::finish(ctx, 0u);
}

} // namespace

PORTABLEKIT_HLE_EXTENSION(example) {
    registry.add("sceHttp", 0x87797BDDu, "sceHttpsLoadDefaultCert", load_default_cert);
    registry.override_builtin("UtilsForUser", 0x79D1C3FAu, "sceKernelDcacheWritebackAll", writeback_data_cache);
}
