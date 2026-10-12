#include "elf_loader.h"

#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <unordered_map>

namespace abr {
namespace {

// ARM reloc types we handle.
constexpr std::uint32_t kAbs32 = 2;
constexpr std::uint32_t kRel32 = 3;
constexpr std::uint32_t kThmCall = 10;
constexpr std::uint32_t kGlobDat = 21;
constexpr std::uint32_t kJumpSlot = 22;
constexpr std::uint32_t kRelative = 23;

std::uint32_t ProtFromFlags(std::uint32_t flags) {
    std::uint32_t prot = 0;
    if (flags & PF_R) {
        prot |= PROT_READ;
    }
    if (flags & PF_W) {
        prot |= PROT_WRITE;
    }
    if (flags & PF_X) {
        prot |= PROT_EXEC;
    }
    return prot;
}

std::uint32_t AlignDown(std::uint32_t v, std::uint32_t a) {
    return v & ~(a - 1);
}
std::uint32_t AlignUp(std::uint32_t v, std::uint32_t a) {
    return (v + a - 1) & ~(a - 1);
}

}  // namespace

bool ElfLoader::Load(const char* path, LoadedModule& out, std::string& err) {
    if (!mem_.ok()) {
        err = "guest memory not reserved";
        return false;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        err = "open failed";
        return false;
    }
    struct stat st {};
    if (fstat(fd, &st) != 0) {
        close(fd);
        err = "fstat failed";
        return false;
    }
    const std::size_t len = static_cast<std::size_t>(st.st_size);
    const std::uint8_t* file =
        static_cast<const std::uint8_t*>(mmap(nullptr, len, PROT_READ, MAP_PRIVATE, fd, 0));
    close(fd);
    if (file == MAP_FAILED) {
        err = "mmap file failed";
        return false;
    }
    auto fail = [&](const std::string& msg) {
        err = msg;
        munmap(const_cast<std::uint8_t*>(file), len);
        return false;
    };

    if (len < sizeof(Elf32_Ehdr)) {
        return fail("too small");
    }
    Elf32_Ehdr ehdr;
    std::memcpy(&ehdr, file, sizeof(ehdr));
    if (std::memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0 || ehdr.e_ident[EI_CLASS] != ELFCLASS32 ||
        ehdr.e_ident[EI_DATA] != ELFDATA2LSB || ehdr.e_machine != EM_ARM || ehdr.e_type != ET_DYN) {
        return fail("not a 32-bit ARM shared object");
    }

    // Map PT_LOAD segments at linked addresses (bias 0).
    for (int i = 0; i < ehdr.e_phnum; i++) {
        Elf32_Phdr ph;
        std::memcpy(&ph, file + ehdr.e_phoff + i * ehdr.e_phentsize, sizeof(ph));
        if (ph.p_type != PT_LOAD) {
            continue;
        }
        const std::uint32_t page = 0x1000;
        const std::uint32_t start = AlignDown(ph.p_vaddr, page);
        const std::uint32_t end = AlignUp(ph.p_vaddr + ph.p_memsz, page);
        std::printf("[phdr] LOAD vaddr=%08x filesz=%08x memsz=%08x flags=%x -> map [%08x,%08x)\n",
                    ph.p_vaddr, ph.p_filesz, ph.p_memsz, ph.p_flags, start, end);
        if (!mem_.Map(start, end - start, PROT_READ | PROT_WRITE)) {
            return fail("segment map failed");
        }
        out.segments.push_back({start, end - start, static_cast<int>(ProtFromFlags(ph.p_flags))});
        std::uint8_t* dst = mem_.host(ph.p_vaddr);
        if (ph.p_filesz > 0) {
            if (ph.p_offset + ph.p_filesz > len) {
                return fail("segment beyond EOF");
            }
            std::memcpy(dst, file + ph.p_offset, ph.p_filesz);
        }
        if (ph.p_memsz > ph.p_filesz) {
            std::memset(dst + ph.p_filesz, 0, ph.p_memsz - ph.p_filesz);
        }
        if (!mem_.Protect(start, end - start, ProtFromFlags(ph.p_flags))) {
            return fail("segment protect failed");
        }
    }

    // Locate .ARM.exidx (SHT_ARM_EXIDX) for __gnu_Unwind_Find_exidx support.
    // The table lives inside a mapped PT_LOAD (bias 0 => VA == link address).
    if (ehdr.e_shoff != 0 && ehdr.e_shentsize >= sizeof(Elf32_Shdr)) {
        for (std::uint32_t si = 0;
             si < ehdr.e_shnum && (std::uint64_t)(si + 1) * ehdr.e_shentsize <= len; si++) {
            Elf32_Shdr sh;
            std::memcpy(&sh, file + ehdr.e_shoff + si * ehdr.e_shentsize, sizeof(sh));
            if (sh.sh_type == SHT_ARM_EXIDX && sh.sh_entsize >= 8 && sh.sh_size >= 8) {
                out.exidx_va = sh.sh_addr;
                out.exidx_count = sh.sh_size / 8;
                std::printf("[exidx] VA=%08x count=%u\n", out.exidx_va, out.exidx_count);
                break;
            }
        }
    }

    auto va2host = [&](std::uint32_t va) -> std::uint8_t* {
        // Find containing LOAD segment to validate.
        for (int i = 0; i < ehdr.e_phnum; i++) {
            Elf32_Phdr ph;
            std::memcpy(&ph, file + ehdr.e_phoff + i * ehdr.e_phentsize, sizeof(ph));
            if (ph.p_type == PT_LOAD && va >= ph.p_vaddr && va < ph.p_vaddr + ph.p_memsz) {
                return mem_.host(va);
            }
        }
        return nullptr;
    };

    // Parse DYNAMIC.
    const Elf32_Phdr* dynph = nullptr;
    Elf32_Phdr phbuf;
    for (int i = 0; i < ehdr.e_phnum; i++) {
        std::memcpy(&phbuf, file + ehdr.e_phoff + i * ehdr.e_phentsize, sizeof(phbuf));
        if (phbuf.p_type == PT_DYNAMIC) {
            dynph = &phbuf;
            break;
        }
    }
    if (!dynph) {
        return fail("no PT_DYNAMIC");
    }
    std::uint32_t strtab = 0, symtab = 0, hash = 0;
    std::uint32_t rel = 0, relsz = 0, jmprel = 0, pltrelsz = 0;
    std::uint32_t init_array = 0, init_arraysz = 0, fini_array = 0, fini_arraysz = 0;
    for (std::uint32_t off = 0;; off += 8) {
        Elf32_Dyn dyn;
        std::memcpy(&dyn, mem_.host(dynph->p_vaddr + off), sizeof(dyn));
        if (dyn.d_tag == DT_NULL) {
            break;
        }
        switch (dyn.d_tag) {
            case DT_STRTAB:
                strtab = dyn.d_un.d_ptr;
                break;
            case DT_SYMTAB:
                symtab = dyn.d_un.d_ptr;
                break;
            case DT_HASH:
                hash = dyn.d_un.d_ptr;
                break;
            case DT_REL:
                rel = dyn.d_un.d_ptr;
                break;
            case DT_RELSZ:
                relsz = dyn.d_un.d_val;
                break;
            case DT_JMPREL:
                jmprel = dyn.d_un.d_ptr;
                break;
            case DT_PLTRELSZ:
                pltrelsz = dyn.d_un.d_val;
                break;
            case DT_INIT_ARRAY:
                init_array = dyn.d_un.d_ptr;
                break;
            case DT_INIT_ARRAYSZ:
                init_arraysz = dyn.d_un.d_val;
                break;
            case DT_FINI_ARRAY:
                fini_array = dyn.d_un.d_ptr;
                break;
            case DT_FINI_ARRAYSZ:
                fini_arraysz = dyn.d_un.d_val;
                break;
            default:
                break;
        }
        if (off > 4096) {
            return fail("dynamic table runaway");
        }
    }
    if (!strtab || !symtab || !rel) {
        return fail("missing dynamic entries");
    }

    auto read32 = [&](std::uint32_t va, std::uint32_t& v) -> bool {
        std::uint8_t* p = va2host(va);
        if (!p) {
            return false;
        }
        std::memcpy(&v, p, 4);
        return true;
    };
    auto write32 = [&](std::uint32_t va, std::uint32_t v) -> bool {
        std::uint8_t* p = va2host(va);
        if (!p) {
            return false;
        }
        std::memcpy(p, &v, 4);
        return true;
    };

    // Symbol table size via DT_HASH (nchain).
    std::uint32_t nchain = 0;
    if (hash) {
        if (!read32(hash + 4, nchain)) {
            return fail("hash unreadable");
        }
    }
    auto get_sym = [&](std::uint32_t idx, Elf32_Sym& sym) -> bool {
        if (idx >= nchain) {
            return false;
        }
        std::uint8_t* p = va2host(symtab + idx * sizeof(Elf32_Sym));
        if (!p) {
            return false;
        }
        std::memcpy(&sym, p, sizeof(sym));
        return true;
    };
    auto get_name = [&](std::uint32_t name_off, std::string& name) -> bool {
        std::uint8_t* base = va2host(strtab);
        if (!base) {
            return false;
        }
        // Bound the scan to avoid runaway on corrupt tables.
        for (std::uint32_t i = 0; i < 4096; i++) {
            char c = static_cast<char>(base[name_off + i]);
            if (c == '\0') {
                name.assign(reinterpret_cast<char*>(base + name_off), i);
                return true;
            }
        }
        return false;
    };

    // Index defined symbols by name for fast lookup.
    std::unordered_map<std::string, std::uint32_t> defined;
    for (std::uint32_t i = 0; i < nchain; i++) {
        Elf32_Sym sym;
        if (!get_sym(i, sym)) {
            return fail("symtab unreadable");
        }
        if (sym.st_shndx != SHN_UNDEF && sym.st_name != 0) {
            std::string name;
            if (get_name(sym.st_name, name)) {
                defined.emplace(std::move(name), sym.st_value);
            }
        }
    }

    // Trampoline page: map writable, emit, then protect RX.
    if (!mem_.Map(kTrampBase, kTrampSize, PROT_READ | PROT_WRITE)) {
        return fail("tramp map failed");
    }
    std::uint32_t next_tramp = kTrampBase;
    auto emit_tramp = [&](std::uint32_t swi) -> std::uint32_t {
        const std::uint32_t va = next_tramp;
        next_tramp += 4;
        const std::uint32_t insn = 0xEF000000u | (swi & 0xFFFFFFu);  // svc #swi
        std::memcpy(mem_.host(va), &insn, 4);
        return va;
    };
    // Slot 0: HALT trampoline (svc #0), LR target for init calls.
    out.halt_va = emit_tramp(kSwiHalt);

    std::unordered_map<std::string, std::uint32_t> import_index;
    auto get_import = [&](const std::string& name, bool is_func) -> std::uint32_t {
        auto it = import_index.find(name);
        if (it != import_index.end()) {
            return it->second;
        }
        const std::uint32_t idx = static_cast<std::uint32_t>(out.imports.size());
        Import imp;
        imp.name = name;
        imp.is_function = is_func;
        imp.tramp_va = emit_tramp(idx + 1);  // swi = idx+1
        out.imports.push_back(std::move(imp));
        import_index.emplace(name, idx);
        return idx;
    };

    auto apply_one = [&](std::uint32_t r_off, std::uint32_t type, std::uint32_t symidx) -> bool {
        if (r_off == 0x672010u) {
            std::printf("[relaudit] write to 00672010: type=%u symidx=%u\n", type, symidx);
        }
        std::uint32_t cur = 0;
        if (!read32(r_off, cur)) {
            return false;
        }
        switch (type) {
            case kRelative:
                return write32(r_off, cur + out.bias);
            case kGlobDat:
            case kJumpSlot: {
                Elf32_Sym sym;
                if (symidx != 0 && get_sym(symidx, sym) && sym.st_shndx != SHN_UNDEF) {
                    return write32(r_off, sym.st_value + out.bias);
                }
                std::string name;
                if (symidx == 0 || !get_name(({
                                                Elf32_Sym s;
                                                get_sym(symidx, s) ? s.st_name : 0;
                                            }),
                                            name)) {
                    return false;
                }
                // Data objects referenced via GLOB_DAT need addresses, not calls.
                if (symidx != 0 && get_sym(symidx, sym) &&
                    ELF32_ST_TYPE(sym.st_info) == STT_OBJECT) {
                    const std::uint32_t idx = get_import(name, false);
                    PendingData pd;
                    pd.loc_va = r_off;
                    pd.import_idx = idx;
                    std::uint32_t cur2 = 0;
                    read32(r_off, cur2);
                    pd.addend = cur2;
                    out.pending_data.push_back(pd);
                    return true;
                }
                const std::uint32_t idx = get_import(name, true);
                return write32(r_off, out.imports[idx].tramp_va);
            }
            case kAbs32: {
                Elf32_Sym sym;
                if (symidx == 0) {
                    return true;  // S=0: value already = addend.
                }
                if (!get_sym(symidx, sym)) {
                    return false;
                }
                if (sym.st_shndx != SHN_UNDEF) {
                    return write32(r_off, cur + sym.st_value + out.bias);
                }
                std::string name;
                if (!get_name(sym.st_name, name)) {
                    return false;
                }
                if (ELF32_ST_TYPE(sym.st_info) == STT_FUNC) {
                    const std::uint32_t idx = get_import(name, true);
                    return write32(r_off, cur + out.imports[idx].tramp_va);
                }
                const std::uint32_t idx = get_import(name, false);
                // Data object address unknown yet (dispatcher assigns guest
                // slots later). Defer: record and patch via ResolveData().
                PendingData pd;
                pd.loc_va = r_off;
                pd.import_idx = idx;
                pd.addend = cur;
                out.pending_data.push_back(pd);
                return true;
            }
            case kRel32: {
                if (symidx == 0) {
                    return true;
                }
                Elf32_Sym sym;
                if (!get_sym(symidx, sym) || sym.st_shndx == SHN_UNDEF) {
                    return false;
                }
                return write32(r_off, cur + sym.st_value + out.bias);
            }
            case kThmCall: {
                if (symidx == 0) {
                    return true;
                }
                Elf32_Sym sym;
                if (!get_sym(symidx, sym)) {
                    return false;
                }
                if (sym.st_shndx != SHN_UNDEF) {
                    return true;  // internal branch: already correct.
                }
                std::string name;
                if (!get_name(sym.st_name, name)) {
                    return false;
                }
                // Can't easily rewrite a Thumb BL here; like JUMP_SLOT it should
                // have gone through the PLT. Bail loudly so we notice.
                (void)name;
                return false;
            }
            default:
                return false;
        }
    };

    // Writable window for relocation targets: temporarily add WRITE to text?
    // TEXTREL absent, but RELRO/GOT pages are RW already. GOT may live in
    // RELRO (mapped RW here; embolden: mprotect dance skipped, we keep RW).
    auto apply_table = [&](std::uint32_t base, std::uint32_t size) -> bool {
        for (std::uint32_t i = 0; i < size; i += 8) {
            Elf32_Rel r;
            std::memcpy(&r, mem_.host(base + i), sizeof(r));
            if (!apply_one(r.r_offset, ELF32_R_TYPE(r.r_info), ELF32_R_SYM(r.r_info))) {
                return false;
            }
        }
        return true;
    };
    // RELATIVE relocs can target read-only segments (const data with absolute
    // addresses). Open a temporary RW window over every RX LOAD, restore after.
    struct ProtRange {
        std::uint32_t start, size, prot;
    };
    std::vector<ProtRange> rx_ranges;
    for (int i = 0; i < ehdr.e_phnum; i++) {
        Elf32_Phdr ph;
        std::memcpy(&ph, file + ehdr.e_phoff + i * ehdr.e_phentsize, sizeof(ph));
        if (ph.p_type != PT_LOAD) {
            continue;
        }
        const std::uint32_t prot = ProtFromFlags(ph.p_flags);
        if ((prot & PROT_WRITE) == 0) {
            const std::uint32_t start = AlignDown(ph.p_vaddr, 0x1000);
            const std::uint32_t end = AlignUp(ph.p_vaddr + ph.p_memsz, 0x1000);
            rx_ranges.push_back({start, end - start, prot});
            if (!mem_.Protect(start, end - start, prot | PROT_WRITE)) {
                return fail("rw window failed");
            }
        }
    }
    bool rel_ok = apply_table(rel, relsz);
    if (rel_ok && pltrelsz) {
        rel_ok = apply_table(jmprel, pltrelsz);
    }
    for (const auto& r : rx_ranges) {
        mem_.Protect(r.start, r.size, r.prot);
    }
    if (!rel_ok) {
        return fail(pltrelsz ? "relocation failed" : "REL relocation failed");
    }

    // JNI function trampolines: 256 slots, swi = kSwiJniBase+i. Runtime builds
    // the JNIEnv function table from these (see dispatch.cpp CallJni).
    out.jni_tramp_base = next_tramp;
    for (std::uint32_t i = 0; i < 256; i++) {
        emit_tramp(kSwiJniBase + i);
    }
    // JavaVM function trampolines (GetEnv etc. for JNI_OnLoad/GetJNIEnv path).
    out.jvm_tramp_base = next_tramp;
    for (std::uint32_t i = 0; i < 8; i++) {
        emit_tramp(kSwiJniBase + 0x100 + i);
    }
    if (next_tramp > kTrampBase + kTrampSize) {
        return fail("tramp page overflow");
    }

    if (!mem_.Protect(kTrampBase, kTrampSize, PROT_READ | PROT_EXEC)) {
        return fail("tramp protect failed");
    }

    for (std::uint32_t i = 0; i < init_arraysz; i += 4) {
        std::uint32_t fn = 0;
        if (!read32(init_array + i, fn)) {
            return fail("init_array unreadable");
        }
        if (fn != 0 && fn != 0xFFFFFFFFu) {
            out.init_array.push_back(fn);
        }
    }
    for (std::uint32_t i = 0; i < fini_arraysz; i += 4) {
        std::uint32_t fn = 0;
        if (!read32(fini_array + i, fn)) {
            return fail("fini_array unreadable");
        }
        if (fn != 0 && fn != 0xFFFFFFFFu) {
            out.fini_array.push_back(fn);
        }
    }

    out.path = path;
    out.bias = 0;
    munmap(const_cast<std::uint8_t*>(file), len);
    return true;
}

bool ElfLoader::ResolveData(LoadedModule& mod, std::uint32_t import_idx, std::uint32_t guest_addr) {
    for (const auto& pd : mod.pending_data) {
        if (pd.import_idx != import_idx) {
            continue;
        }
        const std::uint32_t v = pd.addend + guest_addr;
        std::memcpy(mem_.host(pd.loc_va), &v, 4);
    }
    mod.imports[import_idx].tramp_va = guest_addr;
    return true;
}

}  // namespace abr
