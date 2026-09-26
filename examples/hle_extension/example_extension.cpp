// An example HLE extension module: one function filled in, one overridden, one
// wrapped (a chained override that passes every call on) and one key declared.
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

// sceKernelDcacheWritebackInvalidateAll: says once that it saw a call, then passes it
// to the implementation it wraps, unchanged.
void watch_writeback_invalidate(ext::Runtime &rt, ext::AllegrexContext &ctx, const ext::Handler &previous) {
    ext::log_once("example-dcache-invalidate",
                  "[example] sceKernelDcacheWritebackInvalidateAll passed on by the example extension");
    previous(rt, ctx);
}

} // namespace

PORTABLEKIT_HLE_EXTENSION(example) {
    // A key the module would read with ext::key("example.demo"). Its value is
    // not secret, it only shows the mechanism: 00112233445566778899AABBCCDDEEFF.
    registry.declare_key("example.demo", "a8faed6abbf35c12a4b26e40f6feb19d736d90045c83b9f9a31f638d323e6811");
    registry.add("sceHttp", 0x87797BDDu, "sceHttpsLoadDefaultCert", load_default_cert);
    registry.override_builtin("UtilsForUser", 0x79D1C3FAu, "sceKernelDcacheWritebackAll", writeback_data_cache);
    registry.override_chained("UtilsForUser", 0xB435DEC5u, "sceKernelDcacheWritebackInvalidateAll",
                              watch_writeback_invalidate);
}
