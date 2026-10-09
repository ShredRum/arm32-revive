// distv: disassemble ARM/Thumb words from the game .so (LOAD0: VA == file off).
// Usage: distv <file> <hex-va> <count> <arm|thumb>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "dynarmic/interface/A32/disassembler.h"

int main(int argc, char** argv) {
    if (argc < 5) {
        std::printf("usage: distv <file> <hex-va> <count> <arm|thumb>\n");
        return 2;
    }
    const std::uint32_t va = static_cast<std::uint32_t>(std::strtoul(argv[2], nullptr, 16));
    const int count = std::atoi(argv[3]);
    const bool thumb = std::strcmp(argv[4], "thumb") == 0;
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) {
        return 1;
    }
    std::vector<std::uint8_t> buf(thumb ? count * 2 : count * 4);
    std::fseek(f, va, SEEK_SET);
    if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) {
        return 1;
    }
    std::fclose(f);
    for (int i = 0; i < count; i++) {
        if (thumb) {
            std::uint16_t w = std::uint16_t(buf[2 * i] | (buf[2 * i + 1] << 8));
            std::printf("%08x  %04x  %s\n", va + i * 2, w,
                        Dynarmic::A32::DisassembleThumb16(w).c_str());
        } else {
            std::uint32_t w = std::uint32_t(buf[4 * i] | (buf[4 * i + 1] << 8) |
                                            (buf[4 * i + 2] << 16) | (buf[4 * i + 3] << 24));
            std::printf("%08x  %08x  %s\n", va + i * 4, w,
                        Dynarmic::A32::DisassembleArm(w).c_str());
        }
    }
    return 0;
}
