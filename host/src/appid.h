// Process identity, derived at runtime -- no per-title build.
//
// On Android the main process name IS the package name, and it lives in
// /proc/self/cmdline as a NUL-terminated argv[0]. Unlike the app directory
// (/data/app/<pkg>-1, <pkg>-2) it carries no install-generation suffix, and it
// is readable from any uid without special permission. A ':process' suffix
// (android:process) is stripped. When the file does not hold a plausible
// package name -- the offline harness, a PC build, an odd launcher -- the
// fallback below is returned; the harness sets its file mapping explicitly
// anyway, so the fallback only matters for a product host that somehow has no
// cmdline.
//
// Why not compile it in: one libabhost.so serves several titles (same JNI
// namespace, union forwarders), and the engine VAs/hooks are the only truly
// per-title data. Deriving the identity here removes the package from the
// source entirely.
#pragma once
#include <cstdio>
#include <cstring>
#include <string>

namespace abr {

// Used only when /proc/self/cmdline does not name a package.
inline const char* FallbackPackage() {
    return "com.rovio.angrybirdsrio";
}

inline const std::string& AppPackage() {
    static const std::string pkg = [] {
        char buf[256] = {0};
        std::size_t n = 0;
        if (FILE* f = std::fopen("/proc/self/cmdline", "rb")) {
            n = std::fread(buf, 1, sizeof(buf) - 1, f);
            std::fclose(f);
        }
        // cmdline is argv[0], NUL-terminated; buf is always NUL-terminated.
        std::string s(buf);
        const std::size_t colon = s.find(':');
        if (colon != std::string::npos) {
            s.resize(colon);
        }
        // A path ("./harness", "/usr/bin/...") or empty is not a package.
        if (s.empty() || s[0] == '/' || s[0] == '.') {
            return std::string(FallbackPackage());
        }
        return s;
    }();
    return pkg;
}

// The files directory as the guest spells it. /data/data/<pkg> is the user-0
// alias of /data/user/0/<pkg>; the file bridge also matches the /data/user/<N>
// spelling through the package tail (see Runtime::HostPath), so either form is
// safe to hand to the guest.
inline std::string GuestFilesPrefix() {
    return "/data/data/" + AppPackage() + "/files";
}

}  // namespace abr
