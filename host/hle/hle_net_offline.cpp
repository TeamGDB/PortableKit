// Infrastructure networking, answered as a PSP with no access point: the
// libraries initialise, but nothing connects, resolves or downloads. Games
// check for a network in many ways; these answers let them see "no network"
// and carry on, rather than stubs that claim success with nothing behind it.
// Ad hoc play is hle_adhoc.cpp; real sockets are not needed yet.
//
// No game has been seen calling these past initialising, so each says
// UNVERIFIED on its first call. The error values are negative codes in each
// library's range where its documentation gives none, and BSD errno values
// (ENETUNREACH, ENOTCONN, EWOULDBLOCK) for the socket calls.

#include "../profile.hpp"
#include "hle_common.hpp"
#include "utility_dialog.hpp"

#include "psprecomp/common.hpp"

#include <cstdio>
#include <iostream>
#include <set>
#include <sstream>
#include <string>

namespace portablekit {
namespace {

// BSD errno values, as the PSP's network stack uses them.
constexpr std::uint32_t kEWouldBlock = 35u;
constexpr std::uint32_t kENetUnreach = 51u;
constexpr std::uint32_t kENotConn = 57u;

// Not traced: failures in each library's error range.
constexpr std::uint32_t kApctlNotConnected = 0x80410A04u;
constexpr std::uint32_t kResolverFailed = 0x80410408u;
constexpr std::uint32_t kHttpNotConnected = 0x80431100u;

struct NetState {
    std::uint32_t errno_value{};
    std::uint32_t next_socket{1u};
    std::uint32_t next_handler{1u};
    std::uint32_t next_resolver{1u};
    std::uint32_t next_http_id{1u};
    DialogLifecycle html_viewer;
};

NetState &net() {
    static NetState state;
    return state;
}

void first_call(const char *name) {
    static std::set<std::string> said;
    if (said.insert(name).second)
        std::cerr << "[net] " << name << " (UNVERIFIED: no game traced yet; answered as a PSP with no network)\n";
}

// A socket call that fails: -1, with errno set for sceNetInetGetErrno.
auto failing(const char *name, std::uint32_t errno_value) {
    return [name, errno_value](Runtime &, AllegrexContext &ctx) {
        first_call(name);
        net().errno_value = errno_value;
        kernel().finish(ctx, 0xFFFFFFFFu);
    };
}

auto answering(const char *name, std::uint32_t result) {
    return [name, result](Runtime &, AllegrexContext &ctx) {
        first_call(name);
        kernel().finish(ctx, result);
    };
}

// An id for a template, connection or request: they exist, they only never
// connect.
auto new_http_id(const char *name) {
    return [name](Runtime &, AllegrexContext &ctx) {
        first_call(name);
        kernel().finish(ctx, net().next_http_id++);
    };
}

} // namespace

void register_net_offline(HleRegistrar &hle) {
    // sceNetInet: sockets can be made; nothing reaches anywhere.
    for (const char *name : {"sceNetInetInit", "sceNetInetTerm"}) hle.add("sceNetInet", name, answering(name, 0u));
    hle.add("sceNetInet", "sceNetInetSocket", [](Runtime &, AllegrexContext &ctx) {
        first_call("sceNetInetSocket");
        kernel().finish(ctx, net().next_socket++);
    });
    for (const char *name : {"sceNetInetBind", "sceNetInetListen", "sceNetInetClose", "sceNetInetCloseWithRST",
                             "sceNetInetSetsockopt", "sceNetInetShutdown"})
        hle.add("sceNetInet", name, answering(name, 0u));
    hle.add("sceNetInet", "sceNetInetConnect", failing("sceNetInetConnect", kENetUnreach));
    hle.add("sceNetInet", "sceNetInetSend", failing("sceNetInetSend", kENotConn));
    hle.add("sceNetInet", "sceNetInetSendto", failing("sceNetInetSendto", kENetUnreach));
    hle.add("sceNetInet", "sceNetInetRecv", failing("sceNetInetRecv", kENotConn));
    hle.add("sceNetInet", "sceNetInetRecvfrom", failing("sceNetInetRecvfrom", kEWouldBlock));
    hle.add("sceNetInet", "sceNetInetAccept", failing("sceNetInetAccept", kEWouldBlock));
    hle.add("sceNetInet", "sceNetInetGetpeername", failing("sceNetInetGetpeername", kENotConn));
    // (socket, level, option, value *, length *): zero, four bytes long.
    hle.add("sceNetInet", "sceNetInetGetsockopt", [](Runtime &rt, AllegrexContext &ctx) {
        first_call("sceNetInetGetsockopt");
        if (arg(ctx, 3) != 0u) rt.memory().store32(arg(ctx, 3), 0u);
        if (ctx.gpr[8] != 0u) rt.memory().store32(ctx.gpr[8], 4u);
        kernel().finish(ctx, 0u);
    });
    // (socket, sockaddr *, length *): an unbound IPv4 address.
    hle.add("sceNetInet", "sceNetInetGetsockname", [](Runtime &rt, AllegrexContext &ctx) {
        first_call("sceNetInetGetsockname");
        auto &memory = rt.memory();
        if (arg(ctx, 1) != 0u) {
            for (std::uint32_t i = 0; i < 16u; ++i) memory.store8(arg(ctx, 1) + i, 0u);
            memory.store8(arg(ctx, 1), 16u);  // sin_len
            memory.store8(arg(ctx, 1) + 1u, 2u);  // AF_INET
        }
        if (arg(ctx, 2) != 0u) memory.store32(arg(ctx, 2), 16u);
        kernel().finish(ctx, 0u);
    });
    // Nothing is ever ready.
    hle.add("sceNetInet", "sceNetInetSelect", answering("sceNetInetSelect", 0u));
    hle.add("sceNetInet", "sceNetInetGetErrno", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, net().errno_value);
    });
    hle.add("sceNetInet", "sceNetInetGetPspError", [](Runtime &, AllegrexContext &ctx) {
        first_call("sceNetInetGetPspError");
        kernel().finish(ctx, net().errno_value == 0u ? 0u : 0x80410000u | net().errno_value);
    });
    hle.add("sceNetInet", "sceNetInetGetTcpcbstat", answering("sceNetInetGetTcpcbstat", 0u));
    // sceNetInetInetAddr(const char *): a dotted quad in network byte order,
    // 0xFFFFFFFF (INADDR_NONE) for anything else.
    hle.add("sceNetInet", "sceNetInetInetAddr", [](Runtime &rt, AllegrexContext &ctx) {
        first_call("sceNetInetInetAddr");
        const std::string text = read_cstring(rt.memory(), arg(ctx, 0), 64u);
        unsigned a = 0, b = 0, c = 0, d = 0;
        char tail = 0;
        const int got = std::sscanf(text.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail);
        const bool valid = got == 4 && a < 256u && b < 256u && c < 256u && d < 256u;
        kernel().finish(ctx, valid ? (a | b << 8u | c << 16u | d << 24u) : 0xFFFFFFFFu);
    });
    // sceNetInetInetNtop(af, const void *src, char *dst, size): IPv4 only.
    hle.add("sceNetInet", "sceNetInetInetNtop", [](Runtime &rt, AllegrexContext &ctx) {
        first_call("sceNetInetInetNtop");
        auto &memory = rt.memory();
        const std::uint32_t address = memory.load32(arg(ctx, 1));
        char text[16];
        std::snprintf(text, sizeof(text), "%u.%u.%u.%u", address & 0xFFu, address >> 8u & 0xFFu, address >> 16u & 0xFFu,
                      address >> 24u);
        const std::string value = text;
        if (arg(ctx, 0) != 2u || arg(ctx, 3) <= value.size()) {
            kernel().finish(ctx, 0u);
            return;
        }
        write_cstring(memory, arg(ctx, 2), value, arg(ctx, 3));
        kernel().finish(ctx, arg(ctx, 2));
    });

    // sceNetApctl: no access point is ever joined.
    for (const char *name : {"sceNetApctlInit", "sceNetApctlTerm", "sceNetApctlDelHandler", "sceNetApctlDisconnect"})
        hle.add("sceNetApctl", name, answering(name, 0u));
    hle.add("sceNetApctl", "sceNetApctlAddHandler", [](Runtime &, AllegrexContext &ctx) {
        first_call("sceNetApctlAddHandler");
        kernel().finish(ctx, net().next_handler++);
    });
    hle.add("sceNetApctl", "sceNetApctlGetInfo", answering("sceNetApctlGetInfo", kApctlNotConnected));
    hle.try_add("sceNetApctl", "sceNetApctlConnect", answering("sceNetApctlConnect", kApctlNotConnected));
    // (int *state): PSP_NET_APCTL_STATE_DISCONNECTED.
    hle.try_add("sceNetApctl", "sceNetApctlGetState", [](Runtime &rt, AllegrexContext &ctx) {
        first_call("sceNetApctlGetState");
        if (arg(ctx, 0) != 0u) rt.memory().store32(arg(ctx, 0), 0u);
        kernel().finish(ctx, 0u);
    });

    // sceNetResolver: resolvers can be made; no name resolves.
    for (const char *name : {"sceNetResolverInit", "sceNetResolverTerm", "sceNetResolverDelete"})
        hle.add("sceNetResolver", name, answering(name, 0u));
    hle.add("sceNetResolver", "sceNetResolverCreate", [](Runtime &rt, AllegrexContext &ctx) {
        first_call("sceNetResolverCreate");
        if (arg(ctx, 0) != 0u) rt.memory().store32(arg(ctx, 0), net().next_resolver++);
        kernel().finish(ctx, 0u);
    });
    hle.add("sceNetResolver", "sceNetResolverStartNtoA", answering("sceNetResolverStartNtoA", kResolverFailed));

    // sceHttp: templates, connections and requests can be made; sending
    // fails.
    for (const char *name :
         {"sceHttpInit", "sceHttpEnd", "sceHttpsInit", "sceHttpsEnd", "sceHttpsLoadDefaultCert", "sceHttpSetMallocFunction",
          "sceHttpSaveSystemCookie", "sceHttpLoadSystemCookie", "sceHttpSetRecvTimeOut", "sceHttpSetSendTimeOut",
          "sceHttpSetConnectTimeOut", "sceHttpDisableCookie", "sceHttpDisableCache", "sceHttpDisableAuth",
          "sceHttpAddExtraHeader", "sceHttpSetRedirectCallback", "sceHttpAbortRequest", "sceHttpDeleteTemplate",
          "sceHttpDeleteConnection", "sceHttpDeleteRequest"})
        hle.add("sceHttp", name, answering(name, 0u));
    for (const char *name : {"sceHttpCreateTemplate", "sceHttpCreateConnectionWithURL", "sceHttpCreateRequestWithURL"})
        hle.add("sceHttp", name, new_http_id(name));
    for (const char *name : {"sceHttpSendRequest", "sceHttpReadData", "sceHttpGetStatusCode", "sceHttpGetContentLength",
                             "sceHttpGetAllHeader"})
        hle.add("sceHttp", name, answering(name, kHttpNotConnected));
    // (request, int *error)
    hle.add("sceHttp", "sceHttpGetNetworkPspError", [](Runtime &rt, AllegrexContext &ctx) {
        first_call("sceHttpGetNetworkPspError");
        if (arg(ctx, 1) != 0u) rt.memory().store32(arg(ctx, 1), kHttpNotConnected);
        kernel().finish(ctx, 0u);
    });
    // (request, int *error, unsigned *detail)
    hle.add("sceHttp", "sceHttpsGetSslError", [](Runtime &rt, AllegrexContext &ctx) {
        first_call("sceHttpsGetSslError");
        if (arg(ctx, 1) != 0u) rt.memory().store32(arg(ctx, 1), 0u);
        if (arg(ctx, 2) != 0u) rt.memory().store32(arg(ctx, 2), 0u);
        kernel().finish(ctx, 0u);
    });
    for (const char *name : {"sceSslInit", "sceSslEnd"}) hle.add("sceSsl", name, answering(name, 0u));

    // sceUtility: the network modules load; the browser dialog opens and
    // closes at once, having shown nothing.
    for (const char *name : {"sceUtilityLoadNetModule", "sceUtilityUnloadNetModule"})
        hle.add("sceUtility", name, answering(name, 0u));
    hle.add("sceUtility", "sceUtilityHtmlViewerInitStart", [](Runtime &rt, AllegrexContext &ctx) {
        first_call("sceUtilityHtmlViewerInitStart");
        if (net().html_viewer.active()) {
            kernel().finish(ctx, kErrorUtilityInvalidStatus);
            return;
        }
        if (arg(ctx, 0) != 0u) rt.memory().store32(arg(ctx, 0) + dialog_common::kResultOffset, 0u);
        net().html_viewer.start();
        kernel().finish(ctx, 0u);
    });
    hle.add("sceUtility", "sceUtilityHtmlViewerUpdate", [](Runtime &, AllegrexContext &ctx) {
        (void)net().html_viewer.poll();
        kernel().finish(ctx, 0u);
    });
    hle.add("sceUtility", "sceUtilityHtmlViewerGetStatus", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, net().html_viewer.poll());
    });
    hle.add("sceUtility", "sceUtilityHtmlViewerShutdownStart", [](Runtime &, AllegrexContext &ctx) {
        (void)net().html_viewer.shutdown();
        kernel().finish(ctx, 0u);
    });
}

} // namespace portablekit
