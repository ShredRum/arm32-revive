// Unit test for the mini-zip reader (the app's own base.apk: the guest .so
// and the seeds).
//
// The behaviour under test is the one that protects the next boot: a SHORT
// read (the extraction was interrupted) must not leave a truncated file on
// disk -- the next boot takes that file for the guest and loads a broken
// image. The positive case matters just as much: a good zip must still be
// extracted, byte for byte.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "zipfile.h"

namespace {

int g_fail = 0;

void Expect(const std::string& what, const std::string& got, const std::string& want) {
    if (got == want) {
        std::printf("ok   %-40s -> \"%s\"\n", what.c_str(), got.c_str());
        return;
    }
    std::printf("FAIL %-40s -> \"%s\" (want \"%s\")\n", what.c_str(), got.c_str(), want.c_str());
    g_fail++;
}

void Put16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>(x & 0xff));
    v.push_back(static_cast<std::uint8_t>(x >> 8));
}

void Put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int i = 0; i < 4; i++) {
        v.push_back(static_cast<std::uint8_t>((x >> (8 * i)) & 0xff));
    }
}

// A STORED zip with one entry. `declared_usize` is what the central
// directory claims -- the knob that makes the reader's copy loop run short.
std::vector<std::uint8_t> MakeZip(const std::string& name, const std::vector<std::uint8_t>& data,
                                   std::uint32_t declared_usize) {
    std::vector<std::uint8_t> v;
    // Local file header.
    Put32(v, 0x04034b50u);
    Put16(v, 10);
    Put16(v, 0);   // flags
    Put16(v, 0);   // method 0 = STORED
    Put16(v, 0);   // time
    Put16(v, 0);   // date
    Put32(v, 0);   // crc (the reader does not check it)
    Put32(v, static_cast<std::uint32_t>(data.size()));
    Put32(v, declared_usize);
    Put16(v, static_cast<std::uint16_t>(name.size()));
    Put16(v, 0);   // extra
    v.insert(v.end(), name.begin(), name.end());
    v.insert(v.end(), data.begin(), data.end());
    // Central directory.
    const std::uint32_t cd_off = static_cast<std::uint32_t>(v.size());
    Put32(v, 0x02014b50u);
    Put16(v, 20);
    Put16(v, 10);
    Put16(v, 0);
    Put16(v, 0);
    Put16(v, 0);
    Put16(v, 0);
    Put32(v, 0);
    Put32(v, static_cast<std::uint32_t>(data.size()));
    Put32(v, declared_usize);
    Put16(v, static_cast<std::uint16_t>(name.size()));
    Put16(v, 0);
    Put16(v, 0);
    Put16(v, 0);
    Put16(v, 0);
    Put32(v, 0);
    Put32(v, 0);  // local header offset
    v.insert(v.end(), name.begin(), name.end());
    const std::uint32_t cd_size = static_cast<std::uint32_t>(v.size()) - cd_off;
    // End of central directory.
    Put32(v, 0x06054b50u);
    Put16(v, 0);
    Put16(v, 0);
    Put16(v, 1);
    Put16(v, 1);
    Put32(v, cd_size);
    Put32(v, cd_off);
    Put16(v, 0);
    return v;
}

bool WriteFile(const std::string& path, const std::vector<std::uint8_t>& data) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    const bool ok = data.empty() || std::fwrite(data.data(), 1, data.size(), f) == data.size();
    std::fclose(f);
    return ok;
}

bool Exists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

long Size(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        return -1;
    }
    return static_cast<long>(st.st_size);
}

}  // namespace

int main() {
    char tmpl[] = "/tmp/abr_zip_test_XXXXXX";
    const char* dir = ::mkdtemp(tmpl);
    if (!dir) {
        std::printf("FAIL cannot create a temp dir\n");
        return 1;
    }
    const std::string d(dir);
    const std::string name = "assets/seeds/test.bin";
    std::vector<std::uint8_t> data(64);
    for (std::size_t i = 0; i < data.size(); i++) {
        data[i] = static_cast<std::uint8_t>(i * 7 + 3);
    }

    // --- a good zip: extracted, byte for byte.
    {
        const std::string zip = d + "/good.zip";
        const std::string out = d + "/good.bin";
        WriteFile(zip, MakeZip(name, data, static_cast<std::uint32_t>(data.size())));
        const bool ok = abr::ZipExtractStored(zip, name, out);
        Expect("a good zip extracts", ok ? "1" : "0", "1");
        Expect("the extracted file exists", Exists(out) ? "1" : "0", "1");
        Expect("the extracted size is right", std::to_string(Size(out) == 64 ? 1 : 0), "1");
        std::vector<std::uint8_t> back(64, 0);
        FILE* f = std::fopen(out.c_str(), "rb");
        const std::size_t got = f ? std::fread(back.data(), 1, back.size(), f) : 0;
        if (f) {
            std::fclose(f);
        }
        Expect("the extracted content matches", (got == 64 && back == data) ? "1" : "0", "1");
    }

    // --- a short read (the extraction was cut): the copy loop must fail and
    //     the partial file must NOT stay on disk.
    {
        const std::string zip = d + "/short.zip";
        const std::string out = d + "/short.bin";
        // The directory claims 4096 bytes, the file only carries 64.
        WriteFile(zip, MakeZip(name, data, 4096));
        const bool ok = abr::ZipExtractStored(zip, name, out);
        Expect("a short read fails the extraction", ok ? "0" : "1", "1");
        Expect("the partial file is removed", Exists(out) ? "0" : "1", "1");
    }

    std::printf(g_fail == 0 ? "\nALL OK\n" : "\n%d FAILURES\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
