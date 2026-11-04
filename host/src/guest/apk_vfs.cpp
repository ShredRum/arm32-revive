// Packed files (apk_vfs.h).
#include "apk_vfs.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>

namespace abvfs {

namespace {

std::string Lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

bool PreadAll(int fd, void* buf, std::size_t n, std::uint64_t at) {
    auto* p = static_cast<std::uint8_t*>(buf);
    while (n > 0) {
        const ssize_t r = ::pread(fd, p, n, static_cast<off_t>(at));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        p += r;
        n -= static_cast<std::size_t>(r);
        at += static_cast<std::uint64_t>(r);
    }
    return true;
}

std::uint32_t Le16(const std::uint8_t* p) { return p[0] | (p[1] << 8); }
std::uint32_t Le32(const std::uint8_t* p) { return Le16(p) | (Le16(p + 2) << 16); }

std::int64_t DosTime(std::uint32_t t, std::uint32_t d) {
    std::tm tm{};
    tm.tm_year = static_cast<int>((d >> 9) & 0x7f) + 80;
    tm.tm_mon = static_cast<int>((d >> 5) & 0xf) - 1;
    tm.tm_mday = static_cast<int>(d & 0x1f);
    tm.tm_hour = static_cast<int>((t >> 11) & 0x1f);
    tm.tm_min = static_cast<int>((t >> 5) & 0x3f);
    tm.tm_sec = static_cast<int>((t & 0x1f) * 2);
    return static_cast<std::int64_t>(timegm(&tm));
}

// dir/part, where part takes the spelling of an existing entry of dir that
// matches it ignoring case (so a packed "Android" joins a real "android").
std::string JoinCI(const std::string& dir, const std::string& part) {
    const std::string exact = dir + "/" + part;
    struct stat st {};
    if (::stat(exact.c_str(), &st) == 0) return exact;
    if (DIR* d = ::opendir(dir.c_str())) {
        while (const dirent* e = ::readdir(d)) {
            if (::strcasecmp(e->d_name, part.c_str()) == 0) {
                const std::string hit = dir + "/" + e->d_name;
                ::closedir(d);
                return hit;
            }
        }
        ::closedir(d);
    }
    return exact;
}

}  // namespace

bool ReadZipIndex(const std::string& path, std::vector<ZipEntry>* out, std::string* err) {
    out->clear();
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        *err = std::string("open: ") + std::strerror(errno);
        return false;
    }
    struct Closer {
        int fd;
        ~Closer() { ::close(fd); }
    } closer{fd};
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        *err = "fstat failed";
        return false;
    }
    const std::uint64_t size = static_cast<std::uint64_t>(st.st_size);
    const std::uint64_t scan = std::min<std::uint64_t>(size, 22 + 65535);
    std::vector<std::uint8_t> tail(scan);
    if (!PreadAll(fd, tail.data(), scan, size - scan)) {
        *err = "read of the end record failed";
        return false;
    }
    std::int64_t eocd = -1;
    for (std::int64_t i = static_cast<std::int64_t>(scan) - 22; i >= 0; i--) {
        if (Le32(&tail[static_cast<std::size_t>(i)]) == 0x06054b50u) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        *err = "no end of central directory";
        return false;
    }
    const std::uint8_t* e = &tail[static_cast<std::size_t>(eocd)];
    const std::uint32_t count = Le16(e + 10);
    const std::uint32_t cd_size = Le32(e + 12);
    const std::uint32_t cd_off = Le32(e + 16);
    if (count == 0xffffu || cd_size == 0xffffffffu || cd_off == 0xffffffffu) {
        *err = "zip64 archive (not supported: keep each APK under 4 GB)";
        return false;
    }
    if (static_cast<std::uint64_t>(cd_off) + cd_size > size) {
        *err = "central directory out of the file";
        return false;
    }
    std::vector<std::uint8_t> cd(cd_size);
    if (cd_size && !PreadAll(fd, cd.data(), cd_size, cd_off)) {
        *err = "read of the central directory failed";
        return false;
    }
    std::size_t p = 0;
    for (std::uint32_t i = 0; i < count; i++) {
        if (p + 46 > cd.size() || Le32(&cd[p]) != 0x02014b50u) {
            *err = "bad central directory entry";
            return false;
        }
        const std::uint8_t* h = &cd[p];
        const std::uint32_t nlen = Le16(h + 28), xlen = Le16(h + 30), clen = Le16(h + 32);
        if (p + 46 + nlen + xlen + clen > cd.size()) {
            *err = "central directory entry out of bounds";
            return false;
        }
        ZipEntry z;
        z.name.assign(reinterpret_cast<const char*>(h + 46), nlen);
        z.method = Le16(h + 10);
        z.mtime = DosTime(Le16(h + 12), Le16(h + 14));
        z.csize = Le32(h + 20);
        z.usize = Le32(h + 24);
        const std::uint64_t lho = Le32(h + 42);
        p += 46 + nlen + xlen + clen;
        // The local header's name and extra lengths can differ from the
        // central ones (alignment padding lives in the local extra field).
        std::uint8_t lh[30];
        if (!PreadAll(fd, lh, 30, lho) || Le32(lh) != 0x04034b50u) {
            *err = "bad local header for " + z.name;
            return false;
        }
        z.data_off = lho + 30 + Le16(lh + 26) + Le16(lh + 28);
        if (z.data_off + z.csize > size) {
            *err = "entry data out of the file: " + z.name;
            return false;
        }
        out->push_back(std::move(z));
    }
    return true;
}

std::vector<std::string> AppApks(const std::string& base_apk) {
    std::vector<std::string> out;
    if (base_apk.empty()) return out;
    out.push_back(base_apk);
    const std::size_t slash = base_apk.find_last_of('/');
    const std::string dir = slash == std::string::npos ? "." : base_apk.substr(0, slash);
    std::vector<std::string> rest;
    if (DIR* d = ::opendir(dir.c_str())) {
        while (const dirent* e = ::readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > 10 && n.compare(0, 6, "split_") == 0 &&
                n.compare(n.size() - 4, 4, ".apk") == 0) {
                const std::string full = dir + "/" + n;
                if (full != base_apk) rest.push_back(full);
            }
        }
        ::closedir(d);
    }
    std::sort(rest.begin(), rest.end());
    out.insert(out.end(), rest.begin(), rest.end());
    return out;
}

ssize_t SlicePread(const Slice& s, int fd, void* buf, std::size_t n, std::uint64_t at) {
    if (at >= s.size) return 0;
    n = static_cast<std::size_t>(std::min<std::uint64_t>(n, s.size - at));
    ssize_t r;
    do {
        r = ::pread(fd, buf, n, static_cast<off_t>(s.off + at));
    } while (r < 0 && errno == EINTR);
    return r;
}

ssize_t SliceRead(Slice& s, int fd, void* buf, std::size_t n) {
    std::lock_guard<std::mutex> l(s.mu);
    const ssize_t r = SlicePread(s, fd, buf, n, s.pos);
    if (r > 0) s.pos += static_cast<std::uint64_t>(r);
    return r;
}

std::int64_t SliceSeek(Slice& s, std::int64_t off, int whence) {
    std::lock_guard<std::mutex> l(s.mu);
    std::int64_t base = 0;
    if (whence == SEEK_SET) {
        base = 0;
    } else if (whence == SEEK_CUR) {
        base = static_cast<std::int64_t>(s.pos);
    } else if (whence == SEEK_END) {
        base = static_cast<std::int64_t>(s.size);
    } else {
        errno = EINVAL;
        return -1;
    }
    const std::int64_t to = base + off;
    if (to < 0) {
        errno = EINVAL;
        return -1;
    }
    s.pos = static_cast<std::uint64_t>(to);
    return to;
}

void SliceStat(const Slice& s, struct stat* st) {
    std::memset(st, 0, sizeof *st);
    st->st_dev = s.dev;
    st->st_ino = static_cast<ino_t>(s.ino);
    st->st_mode = S_IFREG | 0600;  // the app's own private file
    st->st_nlink = 1;
    st->st_uid = ::getuid();
    st->st_gid = ::getgid();
    st->st_size = static_cast<off_t>(s.size);
    st->st_blksize = 4096;
    st->st_blocks = static_cast<blkcnt_t>((s.size + 511) / 512);
    st->st_atime = st->st_mtime = st->st_ctime = static_cast<time_t>(s.mtime);
}

namespace {
struct Cookie {
    int fd;
    std::shared_ptr<Slice> s;
};
#if defined(__BIONIC__)
int CkRead(void* c, char* b, int n) {
    auto* k = static_cast<Cookie*>(c);
    return static_cast<int>(SliceRead(*k->s, k->fd, b, static_cast<std::size_t>(n)));
}
fpos_t CkSeek(void* c, fpos_t o, int w) {
    auto* k = static_cast<Cookie*>(c);
    return static_cast<fpos_t>(SliceSeek(*k->s, static_cast<std::int64_t>(o), w));
}
#else
ssize_t CkRead(void* c, char* b, std::size_t n) {
    auto* k = static_cast<Cookie*>(c);
    return SliceRead(*k->s, k->fd, b, n);
}
int CkSeek(void* c, off64_t* o, int w) {
    auto* k = static_cast<Cookie*>(c);
    const std::int64_t r = SliceSeek(*k->s, static_cast<std::int64_t>(*o), w);
    if (r < 0) return -1;
    *o = static_cast<off64_t>(r);
    return 0;
}
#endif
int CkClose(void* c) {
    auto* k = static_cast<Cookie*>(c);
    const int r = ::close(k->fd);
    delete k;
    return r;
}
}  // namespace

FILE* SliceFile(int fd, std::shared_ptr<Slice> s) {
    auto* k = new Cookie{fd, std::move(s)};
#if defined(__BIONIC__)
    FILE* f = ::funopen(k, CkRead, nullptr, CkSeek, CkClose);
#else
    cookie_io_functions_t io{};
    io.read = CkRead;
    io.seek = CkSeek;
    io.close = CkClose;
    FILE* f = ::fopencookie(k, "r", io);
#endif
    if (!f) {
        delete k;  // fd stays the caller's
    }
    return f;
}

void Pack::AddRoot(const std::string& name, const std::string& host_dir) {
    if (host_dir.empty()) return;
    std::string h = host_dir;
    while (h.size() > 1 && h.back() == '/') h.pop_back();
    roots_.push_back(Root{name, h, Lower(h)});
}

std::string Pack::Key(const std::string& host_path) const {
    const std::string lc = Lower(host_path);
    const Root* best = nullptr;
    for (const Root& r : roots_) {
        if (lc.compare(0, r.host_lc.size(), r.host_lc) == 0 &&
            (lc.size() == r.host_lc.size() || lc[r.host_lc.size()] == '/') &&
            (!best || r.host_lc.size() > best->host_lc.size())) {
            best = &r;
        }
    }
    if (!best) return std::string();
    std::string rel = lc.substr(best->host_lc.size());
    // Normalise: no trailing slash, no doubled slashes.
    std::string clean;
    for (char c : rel) {
        if (c == '/' && !clean.empty() && clean.back() == '/') continue;
        clean += c;
    }
    while (!clean.empty() && clean.back() == '/') clean.pop_back();
    return best->name + clean;  // clean starts with '/' or is empty
}

bool Pack::LiveFileLocked(const std::string& key) const {
    return files_.count(key) != 0 && marks_.count(key) == 0;
}

bool Pack::LiveDirLocked(const std::string& key) const {
    const auto it = dirs_.find(key);
    if (it == dirs_.end()) return false;
    for (const auto& c : it->second) {
        const std::string k = key + "/" + Lower(c.first);
        if (c.second ? LiveDirLocked(k) : LiveFileLocked(k)) return true;
    }
    return false;
}

std::string Pack::Build(const std::vector<std::string>& apks, const std::string& marks_file) {
    apks_ = apks;
    marks_file_ = marks_file;
    std::size_t skipped_deflated = 0, skipped_root = 0, dup = 0;
    std::string errors;
    static const std::string kPrefix = "assets/abport/";
    for (std::size_t i = 0; i < apks_.size(); i++) {
        struct stat ast {};
        apk_dev_.push_back(::stat(apks_[i].c_str(), &ast) == 0 ? ast.st_dev : 0);
        std::vector<ZipEntry> zs;
        std::string err;
        if (!ReadZipIndex(apks_[i], &zs, &err)) {
            errors += " " + apks_[i] + ": " + err + ";";
            continue;
        }
        for (const ZipEntry& z : zs) {
            if (z.name.compare(0, kPrefix.size(), kPrefix) != 0 || z.name.back() == '/') continue;
            const std::string rest = z.name.substr(kPrefix.size());
            const std::size_t slash = rest.find('/');
            if (slash == std::string::npos) continue;
            const std::string root = rest.substr(0, slash);
            const std::string rel = rest.substr(slash);  // "/a/b"
            if (std::none_of(roots_.begin(), roots_.end(),
                             [&](const Root& r) { return r.name == root; })) {
                skipped_root++;
                continue;
            }
            if (z.method != 0 || z.csize != z.usize) {
                skipped_deflated++;
                continue;
            }
            const std::string key = root + Lower(rel);
            File f;
            f.apk = static_cast<int>(i);
            f.off = z.data_off;
            f.size = z.usize;
            f.mtime = z.mtime;
            f.ino = (std::hash<std::string>{}(key) | (1ull << 62)) & ~(1ull << 63);
            if (!files_.emplace(key, f).second) {
                dup++;
                continue;
            }
            // Directory entries along the path, spelled as packed.
            std::string dkey = root;
            std::size_t at = 1;
            while (true) {
                const std::size_t next = rel.find('/', at);
                const bool last = next == std::string::npos;
                const std::string part = rel.substr(at, last ? std::string::npos : next - at);
                auto& kids = dirs_[dkey];
                const std::string lp = Lower(part);
                if (std::none_of(kids.begin(), kids.end(), [&](const auto& c) {
                        return Lower(c.first) == lp;
                    })) {
                    kids.emplace_back(part, !last);
                }
                if (last) break;
                dkey += "/" + lp;
                at = next + 1;
            }
        }
    }
    if (FILE* m = std::fopen(marks_file_.c_str(), "r")) {
        char line[4096];
        while (std::fgets(line, sizeof line, m)) {
            std::string s = line;
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            if (!s.empty()) marks_.insert(s);
        }
        std::fclose(m);
    }
    // Skeleton: every packed directory that still holds a live file exists
    // as a real directory (joining an existing one that differs in case).
    std::size_t made = 0;
    std::function<void(const std::string&, const std::string&)> walk =
        [&](const std::string& key, const std::string& host) {
            const auto it = dirs_.find(key);
            if (it == dirs_.end()) return;
            for (const auto& c : it->second) {
                if (!c.second) continue;
                const std::string k = key + "/" + Lower(c.first);
                if (!LiveDirLocked(k)) continue;
                const std::string h = JoinCI(host, c.first);
                struct stat st {};
                if (::stat(h.c_str(), &st) != 0) {
                    std::error_code ec;
                    std::filesystem::create_directories(h, ec);
                    made++;
                }
                walk(k, h);
            }
        };
    for (const Root& r : roots_) {
        if (!LiveDirLocked(r.name)) continue;
        std::error_code ec;
        std::filesystem::create_directories(r.host, ec);
        walk(r.name, r.host);
    }
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "%zu files from %zu apk(s), %zu deletion marks, %zu dirs made, "
                  "skipped: %zu deflated, %zu unknown root, %zu duplicate",
                  files_.size(), apks_.size(), marks_.size(), made, skipped_deflated,
                  skipped_root, dup);
    return std::string(buf) + (errors.empty() ? "" : "; errors:" + errors);
}

const File* Pack::Find(const std::string& host_path) const {
    if (files_.empty()) return nullptr;
    const std::string key = Key(host_path);
    if (key.empty()) return nullptr;
    const auto it = files_.find(key);
    if (it == files_.end()) return nullptr;
    std::shared_lock<std::shared_mutex> l(marks_mu_);
    return marks_.count(key) ? nullptr : &it->second;
}

std::vector<std::pair<std::string, bool>> Pack::Children(const std::string& host_dir) const {
    std::vector<std::pair<std::string, bool>> out;
    if (files_.empty()) return out;
    const std::string key = Key(host_dir);
    if (key.empty()) return out;
    const auto it = dirs_.find(key);
    if (it == dirs_.end()) return out;
    std::shared_lock<std::shared_mutex> l(marks_mu_);
    for (const auto& c : it->second) {
        const std::string k = key + "/" + Lower(c.first);
        if (c.second ? LiveDirLocked(k) : LiveFileLocked(k)) out.push_back(c);
    }
    return out;
}

void Pack::MarkDeleted(const std::string& host_path) {
    if (files_.empty()) return;
    const std::string key = Key(host_path);
    if (key.empty() || files_.count(key) == 0) return;
    std::unique_lock<std::shared_mutex> l(marks_mu_);
    if (!marks_.insert(key).second) return;
    if (FILE* m = std::fopen(marks_file_.c_str(), "a")) {
        std::fprintf(m, "%s\n", key.c_str());
        std::fflush(m);
        ::fsync(fileno(m));
        std::fclose(m);
    }
}

int Pack::Open(const File& f, std::shared_ptr<Slice>* out) const {
    const int fd = ::open(apks_[static_cast<std::size_t>(f.apk)].c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    auto s = std::make_shared<Slice>();
    s->off = f.off;
    s->size = f.size;
    s->ino = f.ino;
    s->mtime = f.mtime;
    s->dev = apk_dev_[static_cast<std::size_t>(f.apk)];
    *out = std::move(s);
    return fd;
}

void Pack::Stat(const File& f, struct stat* st) const {
    Slice s;
    s.size = f.size;
    s.ino = f.ino;
    s.mtime = f.mtime;
    s.dev = apk_dev_[static_cast<std::size_t>(f.apk)];
    SliceStat(s, st);
}

bool Pack::CopyUp(const File& f, const std::string& host_path) const {
    std::shared_ptr<Slice> s;
    const int in = Open(f, &s);
    if (in < 0) return false;
    const std::string tmp = host_path + ".abport-copy";
    const int out = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (out < 0) {
        ::close(in);
        return false;
    }
    bool ok = true;
    std::vector<char> buf(1 << 20);
    for (std::uint64_t at = 0; ok && at < f.size;) {
        const ssize_t r = SlicePread(*s, in, buf.data(), buf.size(), at);
        if (r <= 0) {
            ok = false;
            break;
        }
        for (ssize_t w = 0; w < r;) {
            const ssize_t k = ::write(out, buf.data() + w, static_cast<std::size_t>(r - w));
            if (k < 0 && errno == EINTR) continue;
            if (k <= 0) {
                ok = false;
                break;
            }
            w += k;
        }
        at += static_cast<std::uint64_t>(r);
    }
    ::close(in);
    if (ok) {
        const struct timespec ts[2] = {{static_cast<time_t>(f.mtime), 0},
                                       {static_cast<time_t>(f.mtime), 0}};
        ::futimens(out, ts);
        ok = ::fsync(out) == 0;
    }
    ok = (::close(out) == 0) && ok;
    if (!ok || ::rename(tmp.c_str(), host_path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

}  // namespace abvfs
