// Mini-zip reader for our own base.apk (and for the unit test).
//
// Packaging guarantees the guest .so and the seeds are STORED
// (apktool.yml doNotCompress), so there is no inflate here.
//
// This lives in its own module (not inside apphost.cpp) so that the native
// unit test can exercise the extraction -- in particular the rule that a
// SHORT read must not leave a truncated file behind: the next boot would take
// that file for the guest and load a broken image.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace abr {

struct ZipEntry {
    std::string name;
    std::uint32_t data_off = 0, csize = 0, usize = 0;
    std::uint16_t method = 0;
};

// The app's logger (apphost's Log): printf-style.
using ZipLogFn = void (*)(const char* fmt, ...);
void ZipSetLog(ZipLogFn fn);

// Walks the central directory in order. fn returns true to stop the walk.
bool ZipWalk(const std::string& apk, const std::function<bool(const ZipEntry&)>& fn);

// Extracts a STORED entry. On a short read the partial file is REMOVED and
// false is returned: a truncated file on disk would be taken for the guest on
// the next boot.
bool ZipExtractStored(const std::string& apk, const std::string& inner, const std::string& out);

// Entry names under a prefix, in central-directory order.
std::vector<std::string> ZipListPrefix(const std::string& apk, const std::string& prefix);

}  // namespace abr
