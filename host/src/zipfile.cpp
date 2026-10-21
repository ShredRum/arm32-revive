// Mini-zip reader: the extracted code from apphost.cpp (2026-10-22), moved
// here so the unit test can reach it. The behaviour is unchanged; only the
// logger became a callback.
#include "zipfile.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace abr {
namespace {

// The default logger (the app installs its own).
void DefaultLog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}

void (*g_log)(const char* fmt, ...) = &DefaultLog;

}  // namespace

void ZipSetLog(ZipLogFn fn) {
    if (fn) {
        g_log = fn;
    }
}

// Mini-zip reader for our own base.apk. Packaging guarantees the guest .so and
// the seeds are STORED (apktool.yml doNotCompress), so there is no inflate here.
// Walks the central directory in order. fn returns true to stop the walk.
bool ZipWalk(const std::string& apk, const std::function<bool(const ZipEntry&)>& fn) {
    FILE* f = std::fopen(apk.c_str(), "rb");
    if (!f) {
        g_log("zip open %s failed", apk.c_str());
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    // EOCD scan (last 64K+22).
    const long scan0 = sz > 66000 ? sz - 66000 : 0;
    long cd_off = -1, cd_n = 0;
    std::vector<std::uint8_t> tail((std::size_t)(sz - scan0));
    std::fseek(f, scan0, SEEK_SET);
    if (std::fread(tail.data(), 1, tail.size(), f) != tail.size()) {
        std::fclose(f);
        return false;
    }
    auto rd32 = [&](std::size_t o) -> std::uint32_t {
        return (std::uint32_t)tail[o] | ((std::uint32_t)tail[o + 1] << 8) |
               ((std::uint32_t)tail[o + 2] << 16) | ((std::uint32_t)tail[o + 3] << 24);
    };
    auto rd16 = [&](std::size_t o) -> std::uint32_t {
        return (std::uint32_t)tail[o] | ((std::uint32_t)tail[o + 1] << 8);
    };
    for (std::size_t o = 0; o + 22 <= tail.size(); o++) {
        if (rd32(o) == 0x06054B50u) {
            cd_n = rd16(o + 10);
            cd_off = (long)rd32(o + 16);
        }
    }
    if (cd_off < 0) {
        std::fclose(f);
        g_log("zip EOCD not found");
        return false;
    }
    std::fseek(f, cd_off, SEEK_SET);
    bool stop = false;
    for (long i = 0; i < cd_n && !stop; i++) {
        std::uint8_t h[46];
        if (std::fread(h, 1, 46, f) != 46) {
            break;
        }
        auto h32 = [&](int o) -> std::uint32_t {
            return (std::uint32_t)h[o] | ((std::uint32_t)h[o + 1] << 8) |
                   ((std::uint32_t)h[o + 2] << 16) | ((std::uint32_t)h[o + 3] << 24);
        };
        auto h16 = [&](int o) -> std::uint32_t {
            return (std::uint32_t)h[o] | ((std::uint32_t)h[o + 1] << 8);
        };
        if (h32(0) != 0x02014B50u) {
            break;
        }
        const std::uint32_t nl = h16(28), el = h16(30), cl = h16(32);
        std::string nm(nl, '\0');
        if (nl && std::fread(nm.data(), 1, nl, f) != nl) {
            break;
        }
        std::fseek(f, el + cl, SEEK_CUR);
        ZipEntry e;
        e.name = nm;
        e.method = (std::uint16_t)h16(10);
        e.csize = h32(20);
        e.usize = h32(24);
        e.data_off = h32(42);
        if (fn(e)) {
            stop = true;
        }
    }
    std::fclose(f);
    return true;
}

bool ZipExtractStored(const std::string& apk, const std::string& inner, const std::string& out) {
    ZipEntry hit;
    bool found = false;
    ZipWalk(apk, [&](const ZipEntry& e) {
        if (e.name == inner) {
            hit = e;
            found = true;
            return true;
        }
        return false;
    });
    if (!found || hit.method != 0) {
        g_log("zip entry %s %s", inner.c_str(), found ? "not STORED" : "missing");
        return false;
    }
    FILE* f = std::fopen(apk.c_str(), "rb");
    if (!f) {
        return false;
    }
    // Local header -> data.
    std::fseek(f, hit.data_off, SEEK_SET);
    std::uint8_t lh[30];
    if (std::fread(lh, 1, 30, f) != 30) {
        std::fclose(f);
        return false;
    }
    const std::uint32_t lnl = (std::uint32_t)lh[26] | ((std::uint32_t)lh[27] << 8);
    const std::uint32_t lel = (std::uint32_t)lh[28] | ((std::uint32_t)lh[29] << 8);
    std::fseek(f, hit.data_off + 30 + lnl + lel, SEEK_SET);
    FILE* o = std::fopen(out.c_str(), "wb");
    if (!o) {
        std::fclose(f);
        return false;
    }
    std::vector<std::uint8_t> buf(1 << 20);
    std::uint32_t left = hit.usize;
    while (left) {
        const std::uint32_t n = left > buf.size() ? (std::uint32_t)buf.size() : left;
        if (std::fread(buf.data(), 1, n, f) != n) {
            break;
        }
        std::fwrite(buf.data(), 1, n, o);
        left -= n;
    }
    std::fclose(o);
    std::fclose(f);
    g_log("zip extracted %s (%uB)", inner.c_str(), hit.usize - left);
    if (left != 0) {
        // A short read leaves a TRUNCATED file behind; the next boot would
        // take it for the guest and load a broken image. Remove it.
        std::remove(out.c_str());
        g_log("zip extract FAILED (%uB missing) -- removed the partial file", left);
    }
    return left == 0;
}

// Entry names under a prefix, in central-directory order.
std::vector<std::string> ZipListPrefix(const std::string& apk, const std::string& prefix) {
    std::vector<std::string> v;
    ZipWalk(apk, [&](const ZipEntry& e) {
        if (e.name.size() >= prefix.size() && e.name.compare(0, prefix.size(), prefix) == 0) {
            v.push_back(e.name);
        }
        return false;
    });
    return v;
}


}  // namespace abr
