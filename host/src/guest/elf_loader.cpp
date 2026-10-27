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

// PT_GNU_RELRO is not in every <elf.h>; the value is ABI-fixed.
constexpr std::uint32_t kGnuRelro = 0x6474e552u;

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

bool ElfLoader::Load(const char* path, LoadedModule& out, std::string& err, std::uint32_t bias) {
    const bool first = out.halt_va == 0;  // else: add to the libraries in `out`
    LoadedModule::Extra extra;
    extra.path = path;
    extra.bias = bias;
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

    // Map PT_LOAD segments at linked address + bias.
    for (int i = 0; i < ehdr.e_phnum; i++) {
        Elf32_Phdr ph;
        std::memcpy(&ph, file + ehdr.e_phoff + i * ehdr.e_phentsize, sizeof(ph));
        if (ph.p_type != PT_LOAD) {
            continue;
        }
        const std::uint32_t page = 0x1000;
        const std::uint32_t start = AlignDown(ph.p_vaddr, page) + bias;
        const std::uint32_t end = AlignUp(ph.p_vaddr + ph.p_memsz, page) + bias;
        std::printf("[phdr] LOAD vaddr=%08x filesz=%08x memsz=%08x flags=%x -> map [%08x,%08x)\n",
                    ph.p_vaddr, ph.p_filesz, ph.p_memsz, ph.p_flags, start, end);
        if (!mem_.Map(start, end - start, PROT_READ | PROT_WRITE)) {
            return fail("segment map failed");
        }
        out.segments.push_back({start, end - start, static_cast<int>(ProtFromFlags(ph.p_flags))});
        std::uint32_t& lo = first ? out.span_lo : extra.span_lo;
        std::uint32_t& hi = first ? out.span_hi : extra.span_hi;
        if (hi == 0 || start < lo) {
            lo = start;
        }
        if (end > hi) {
            hi = end;
        }
        std::uint8_t* dst = mem_.host(ph.p_vaddr + bias);
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

    // PT_GNU_RELRO: the real linker mprotects this to R once relocations are
    // applied, so the dynamic loader itself never writes .got/.dynamic/
    // init_array at runtime. The host keeps the pages RW (they hold host-written
    // relocation targets), so a GUEST wild write here was silent -- proven
    // 2026-10-18: GOT slot 0x6FFE28 zeroed mid-run -> `ldr pc,[r12,#0x3c4]!` ->
    // jump to 0 -> worker died without closing its promise. Record the range
    // (glibc's _dl_protect_relro rounding: both ends page-truncated) so the
    // runtime can refuse + name guest writes there.
    for (int i = 0; i < ehdr.e_phnum; i++) {
        Elf32_Phdr ph;
        std::memcpy(&ph, file + ehdr.e_phoff + i * ehdr.e_phentsize, sizeof(ph));
        if (ph.p_type != kGnuRelro || ph.p_memsz == 0) {
            continue;
        }
        const std::uint32_t page = 0x1000;
        const std::uint32_t rstart = AlignDown(ph.p_vaddr, page) + bias;
        // The END rounds UP, like glibc's _dl_protect_relro: the page that
        // holds the last byte of the RELRO is protected too. Rounding it
        // down (as this did) left that page unguarded -- a guest write
        // there would not have been refused.
        const std::uint32_t rend = AlignUp(ph.p_vaddr + ph.p_memsz, page) + bias;
        if (rend > rstart) {
            if (first) {
                out.relro_start = rstart;
                out.relro_size = rend - rstart;
            } else {
                extra.relro_start = rstart;
                extra.relro_size = rend - rstart;
            }
            std::printf("[phdr] GNU_RELRO [%08x,%08x) -- guest writes refused\n", rstart, rend);
        }
    }

    bool found_build_id = false;
    // G3.4 (2026-10-18): capture the GNU build-id from PT_NOTE. The whole host
    // assumes ONE exact guest image (hardcoded guest VAs, patch sites,
    // behavioural hooks in apphost/dispatch), so the pin should be checkable at
    // runtime. 20-byte desc, no crypto needed.
    for (int i = 0; i < ehdr.e_phnum; i++) {
        Elf32_Phdr ph;
        std::memcpy(&ph, file + ehdr.e_phoff + i * ehdr.e_phentsize, sizeof(ph));
        if (ph.p_type != PT_NOTE || ph.p_offset + ph.p_filesz > len) {
            continue;
        }
        const std::uint8_t* p = file + ph.p_offset;
        const std::uint8_t* pend = p + ph.p_filesz;
        while (p + 12 <= pend) {
            std::uint32_t namesz = 0, descsz = 0, ntype = 0;
            std::memcpy(&namesz, p, 4);
            std::memcpy(&descsz, p + 4, 4);
            std::memcpy(&ntype, p + 8, 4);
            const std::uint8_t* name = p + 12;
            const std::uint8_t* desc = name + ((namesz + 3u) & ~3u);
            const std::uint8_t* next = desc + ((descsz + 3u) & ~3u);
            if (next > pend) {
                break;
            }
            if (ntype == 3 && namesz == 4 && std::memcmp(name, "GNU", 4) == 0 && descsz > 0) {
                if (first) {
                    out.build_id.assign(desc, desc + descsz);
                }
                found_build_id = true;
                break;
            }
            p = next;
        }
        if (found_build_id) {
            break;
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
                if (first) {
                    out.exidx_va = sh.sh_addr + bias;
                    out.exidx_count = sh.sh_size / 8;
                } else {
                    extra.exidx_va = sh.sh_addr + bias;
                    extra.exidx_count = sh.sh_size / 8;
                }
                std::printf("[exidx] VA=%08x count=%u\n", sh.sh_addr + bias, sh.sh_size / 8);
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
                return mem_.host(va + bias);
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
    std::uint32_t strtab = 0, strsz = 0, symtab = 0, hash = 0;
    std::uint32_t rel = 0, relsz = 0, jmprel = 0, pltrelsz = 0;
    std::uint32_t init_array = 0, init_arraysz = 0, fini_array = 0, fini_arraysz = 0;
    std::vector<std::uint32_t> needed_off;  // DT_NEEDED name offsets (strtab)
    for (std::uint32_t off = 0;; off += 8) {
        Elf32_Dyn dyn;
        std::memcpy(&dyn, mem_.host(dynph->p_vaddr + bias + off), sizeof(dyn));
        if (dyn.d_tag == DT_NULL) {
            break;
        }
        switch (dyn.d_tag) {
            case DT_STRTAB:
                strtab = dyn.d_un.d_ptr;
                break;
            case DT_STRSZ:
                strsz = dyn.d_un.d_val;
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
            case DT_NEEDED:
                needed_off.push_back(dyn.d_un.d_val);
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
        // Bound the scan to avoid runaway on corrupt tables, and to the
        // string table itself: the read was not range-checked, so a corrupt
        // name_off could read past the mapped segment (the guest address
        // space is PROT_NONE outside the mapped parts -- a host SIGSEGV).
        if (name_off >= strsz) {
            return false;
        }
        const std::uint32_t limit = std::min<std::uint32_t>(4096, strsz - name_off);
        for (std::uint32_t i = 0; i < limit; i++) {
            char c = static_cast<char>(base[name_off + i]);
            if (c == '\0') {
                name.assign(reinterpret_cast<char*>(base + name_off), i);
                return true;
            }
        }
        return false;
    };

    // Index defined symbols by name (guest addresses). A name an earlier
    // library already defines keeps that definition (linker search order).
    std::unordered_map<std::string, std::uint32_t>& defined = out.defined;
    out.lib_defined.emplace_back();
    std::unordered_map<std::string, std::uint32_t>& own = out.lib_defined.back();
    for (std::uint32_t i = 0; i < nchain; i++) {
        Elf32_Sym sym;
        if (!get_sym(i, sym)) {
            return fail("symtab unreadable");
        }
        if (sym.st_shndx != SHN_UNDEF && sym.st_name != 0) {
            std::string name;
            if (get_name(sym.st_name, name)) {
                own.emplace(name, sym.st_value + bias);
                defined.emplace(std::move(name), sym.st_value + bias);
            }
        }
    }
    // A library's own exports, by name, so an undefined symbol can be told
    // apart from one this very library defines (the merged table above
    // may hold an earlier library's definition of the same name).
    if (first) {
        out.bias = bias;
    }

    // Trampoline page: map writable, emit, then protect RX. A later library
    // reopens it and continues after the slots already emitted.
    if (first ? !mem_.Map(kTrampBase, kTrampSize, PROT_READ | PROT_WRITE)
              : !mem_.Protect(kTrampBase, kTrampSize, PROT_READ | PROT_WRITE)) {
        return fail("tramp map failed");
    }
    std::uint32_t next_tramp = first ? kTrampBase : out.next_tramp;
    // Guest-side VFP thunks live in the upper part of the trampoline page so
    // the 4-byte svc slots keep their layout (the STUCK diagnostics map a pc
    // in the slot area back to an import name by slot arithmetic).
    std::uint32_t next_vfp_thunk = first ? kTrampBase + kTrampSize - 0x1000 : out.next_vfp_thunk;
    // vmov d0,r0,r1 ; vsqrt.f64 d0,d0 ; vmov r0,r1,d0 ; bx lr
    static const std::uint8_t kFastSqrtThunk[16] = {
        0x10, 0x0b, 0x41, 0xec, 0xc0, 0x0b, 0xb1, 0xee,
        0x10, 0x0b, 0x51, 0xec, 0x1e, 0xff, 0x2f, 0xe1,
    };
    auto emit_tramp = [&](std::uint32_t swi) -> std::uint32_t {
        const std::uint32_t va = next_tramp;
        next_tramp += 4;
        const std::uint32_t insn = 0xEF000000u | (swi & 0xFFFFFFu);  // svc #swi
        std::memcpy(mem_.host(va), &insn, 4);
        return va;
    };
    if (first) {
        // Slot 0: HALT trampoline (svc #0), LR target for init calls.
        out.halt_va = emit_tramp(kSwiHalt);
        // Slot 1: pthread_once init-return trampoline. Distinct from HALT so a
        // halt from any other source (thread exit, qsort comparator) can never
        // be mistaken for "the once init routine returned" (see Runtime::OnSvc).
        out.once_va = emit_tramp(kSwiOnce);
    }

    std::unordered_map<std::string, std::uint32_t> import_index;
    for (std::uint32_t i = 0; i < out.imports.size(); i++) {
        import_index.emplace(out.imports[i].name, i);  // host imports are shared
    }
    auto get_import = [&](const std::string& name, bool is_func) -> std::uint32_t {
        auto it = import_index.find(name);
        if (it != import_index.end()) {
            return it->second;
        }
        const std::uint32_t idx = static_cast<std::uint32_t>(out.imports.size());
        Import imp;
        imp.name = name;
        imp.is_function = is_func;
        imp.fast_math = static_cast<std::uint8_t>(FastMathForName(name));
        imp.blocking = BlockingImportName(name);
        imp.tramp_va = emit_tramp(idx + 1);  // swi = idx+1
        imp.call_va = imp.tramp_va;
        if (name == "sqrt") {
            // Guest-side VFP thunk: the call is answered by the guest's own
            // ARM32 code (vsqrt.f64), so it never leaves the JIT -- no SVC, no
            // host libm, no import dispatch at all. Seasons' boot ran a sort
            // whose comparator called sqrt millions of times; every one of
            // those paid a full host round trip.
            //
            // The bytes are the NDK's own codegen for `sqrt(double)` under
            // -mfloat-abi=softfp (assembled, not hand-encoded):
            //   ec410b10  vmov   d0, r0, r1   ; d0 = the double (r0 = low word)
            //   eeb10bc0  vsqrt.f64 d0, d0
            //   ec510b10  vmov   r0, r1, d0   ; r0:r1 = the result
            //   e12fff1e  bx     lr
            // Same softfp convention as the host path (double in r0:r1, low
            // word first) and IEEE-correctly rounded, like the native device's
            // own VFP sqrt. d0 is caller-saved in the base PCS, so clobbering
            // it is safe.
            const std::uint32_t va = next_vfp_thunk;
            next_vfp_thunk += sizeof(kFastSqrtThunk);
            std::memcpy(mem_.host(va), kFastSqrtThunk, sizeof(kFastSqrtThunk));
            imp.call_va = va;
        }
        out.imports.push_back(std::move(imp));
        import_index.emplace(name, idx);
        return idx;
    };

    // A weak reference nobody defines resolves to 0 (ELF), and the code then
    // tests it for presence. These are the weak names the guests reference
    // that Android's system libraries do not export (checked against the NDK
    // r27 stubs of libc, libm, libdl, libstdc++, liblog and libandroid, API
    // 21-25), so on a device they are 0; as imports they would get a
    // trampoline address and read as present. Every other weak name stays an
    // import: getauxval and __gnu_Unwind_Find_exidx are exported by libc.
    auto unexported_weak = [](const Elf32_Sym& s, const std::string& name) {
        static const char* const kNames[] = {"__data_start", "data_start", "__cxa_begin_cleanup",
                                             "__cxa_type_match", "__cxa_call_unexpected"};
        if (ELF32_ST_BIND(s.st_info) != STB_WEAK) {
            return false;
        }
        for (const char* k : kNames) {
            if (name == k) {
                return true;
            }
        }
        return false;
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
                return write32(r_off, cur + bias);
            case kGlobDat:
            case kJumpSlot: {
                Elf32_Sym sym;
                if (symidx != 0 && get_sym(symidx, sym) && sym.st_shndx != SHN_UNDEF) {
                    return write32(r_off, sym.st_value + bias);
                }
                std::string name;
                if (symidx == 0 || !get_name(({
                                                Elf32_Sym s;
                                                get_sym(symidx, s) ? s.st_name : 0;
                                            }),
                                            name)) {
                    return false;
                }
                // Defined by a library loaded before this one: bind to it.
                if (!first) {
                    const auto it = out.defined.find(name);
                    if (it != out.defined.end()) {
                        return write32(r_off, it->second);
                    }
                }
                if (get_sym(symidx, sym) && unexported_weak(sym, name)) {
                    return write32(r_off, 0);
                }
                // Data objects referenced via GLOB_DAT need addresses, not calls.
                if (symidx != 0 && get_sym(symidx, sym) &&
                    ELF32_ST_TYPE(sym.st_info) == STT_OBJECT) {
                    const std::uint32_t idx = get_import(name, false);
                    PendingData pd;
                    pd.loc_va = r_off + bias;  // guest address of the slot
                    pd.import_idx = idx;
                    std::uint32_t cur2 = 0;
                    read32(r_off, cur2);
                    pd.addend = cur2;
                    out.pending_data.push_back(pd);
                    return true;
                }
                const std::uint32_t idx = get_import(name, true);
                return write32(r_off, out.imports[idx].call_va);
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
                    return write32(r_off, cur + sym.st_value + bias);
                }
                std::string name;
                if (!get_name(sym.st_name, name)) {
                    return false;
                }
                if (!first) {
                    const auto it = out.defined.find(name);
                    if (it != out.defined.end()) {
                        return write32(r_off, cur + it->second);
                    }
                }
                if (unexported_weak(sym, name)) {
                    return write32(r_off, cur);  // S = 0
                }
                if (ELF32_ST_TYPE(sym.st_info) == STT_FUNC) {
                    const std::uint32_t idx = get_import(name, true);
                    return write32(r_off, cur + out.imports[idx].call_va);
                }
                const std::uint32_t idx = get_import(name, false);
                // Data object address unknown yet (dispatcher assigns guest
                // slots later). Defer: record and patch via ResolveData().
                PendingData pd;
                pd.loc_va = r_off + bias;  // guest address of the slot
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
                return write32(r_off, cur + sym.st_value + bias);
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
            const std::uint8_t* rp = va2host(base + i);
            if (!rp) {
                return false;
            }
            std::memcpy(&r, rp, sizeof(r));
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
            const std::uint32_t start = AlignDown(ph.p_vaddr, 0x1000) + bias;
            const std::uint32_t end = AlignUp(ph.p_vaddr + ph.p_memsz, 0x1000) + bias;
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

    if (first) {
        // JNI function trampolines: 256 slots, swi = kSwiJniBase+i. Runtime
        // builds the JNIEnv function table from these (see dispatch.cpp CallJni).
        out.jni_tramp_base = next_tramp;
        for (std::uint32_t i = 0; i < 256; i++) {
            emit_tramp(kSwiJniBase + i);
        }
        // JavaVM function trampolines (GetEnv etc. for JNI_OnLoad/GetJNIEnv path).
        out.jvm_tramp_base = next_tramp;
        for (std::uint32_t i = 0; i < 8; i++) {
            emit_tramp(kSwiJniBase + 0x100 + i);
        }
    }
    out.next_tramp = next_tramp;
    out.next_vfp_thunk = next_vfp_thunk;
    if (next_tramp > kTrampBase + kTrampSize) {
        return fail("tramp page overflow");
    }
    // The guest-side VFP thunks live in the top of the page; the 4-byte svc
    // slots must not reach them (a guest with thousands of imports would).
    if (next_tramp > next_vfp_thunk) {
        return fail("tramp page overflow: svc slots reached the VFP thunk area");
    }

    if (!mem_.Protect(kTrampBase, kTrampSize, PROT_READ | PROT_EXEC)) {
        return fail("tramp protect failed");
    }

    // The NULL entries are KEPT (as 0) so that the index of an entry here
    // is the index it has in the guest's DT_INIT_ARRAY. Dropping them shifted
    // every later index -- the "PARKED #22" check and the HARNESS_SKIP
    // index then addressed the wrong function. The init loops skip the 0.
    const std::uint32_t init_begin = static_cast<std::uint32_t>(out.init_array.size());
    for (std::uint32_t i = 0; i < init_arraysz; i += 4) {
        std::uint32_t fn = 0;
        if (!read32(init_array + i, fn)) {
            return fail("init_array unreadable");
        }
        out.init_array.push_back(fn == 0xFFFFFFFFu ? 0 : fn);
    }
    LoadedModule::LibInfo info;
    info.init_begin = init_begin;
    info.init_end = static_cast<std::uint32_t>(out.init_array.size());
    for (const std::uint32_t no : needed_off) {
        std::string nm;
        if (get_name(no, nm)) {
            info.needed.push_back(nm);
        }
    }
    out.libs.push_back(std::move(info));
    for (std::uint32_t i = 0; i < fini_arraysz; i += 4) {
        std::uint32_t fn = 0;
        if (!read32(fini_array + i, fn)) {
            return fail("fini_array unreadable");
        }
        out.fini_array.push_back(fn == 0xFFFFFFFFu ? 0 : fn);
    }

    if (first) {
        out.path = path;
    } else {
        out.more.push_back(extra);
    }
    munmap(const_cast<std::uint8_t*>(file), len);
    return true;
}

// Bind a data import to a guest address: patch every GOT slot that refers
// to it and point the import itself at that address. A data import has no
// call target -- its "trampoline" address is a code address the guest must
// never dereference as data. The caller range-checks the slots first.
bool ResolveDataImport(GuestMemory& mem, LoadedModule& mod, std::uint32_t import_idx,
                       std::uint32_t guest_addr) {
    if (import_idx >= mod.imports.size()) {
        return false;
    }
    for (const auto& pd : mod.pending_data) {
        if (pd.import_idx != import_idx) {
            continue;
        }
        const std::uint32_t v = pd.addend + guest_addr;
        std::memcpy(mem.host(pd.loc_va), &v, 4);
    }
    mod.imports[import_idx].tramp_va = guest_addr;
    // Data imports use the slot as their guest address, so call_va must
    // follow it (call_va is "the address the guest branches to").
    mod.imports[import_idx].call_va = guest_addr;
    return true;
}

}  // namespace abr
