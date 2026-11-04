// Packed files: read-only game files inside the app's own APKs (base.apk and
// its split APKs), shown to the guest as ordinary files under fixed roots of
// the app's private storage.
//
// Why: a title that keeps big data outside its APK (an OBB, downloaded
// episodes) cannot get that data onto a phone without root any more
// (Android 11+ closes Android/obb and Android/data to file managers). Put
// into APK splits instead, the data installs with the app (SAI, pm
// install-multiple), is never unpacked, and is read in place.
//
// Layout in an APK: assets/abport/<root>/<path>, STORED (not deflated; the
// packer aligns each entry to 16 KiB so a mapping can point at it). Roots:
//   files   the app's files dir (guest /data/data/<pkg>/files)
//   ext     the app's external dir as the host keeps it (<data>/ext, i.e.
//           guest <storage>/Android/data/<pkg>)
//   sdcard  the private shared storage (host.private_sdcard: <files>/sdcard)
//
// Overlay rules (as an overlay filesystem with a read-only lower layer):
// - a real file at the same host path wins: the guest wrote or replaced it;
// - a packed file opened for writing (no truncation), appended to, renamed
//   or truncated is first copied to a real file ("copy-up");
// - unlink of a packed file, or of a real file that hides one, records a
//   deletion mark kept in a file next to the app's data, so the packed one
//   does not come back;
// - packed directories exist as real (empty) directories, made at boot, so
//   stat/opendir/mkdir work unchanged; opendir adds the packed entries;
// - lookups ignore case (shared storage on a device does).
#pragma once

#include <sys/stat.h>
#include <sys/types.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace abvfs {

// One entry of a zip's central directory.
struct ZipEntry {
    std::string name;
    std::uint32_t method = 0;  // 0 stored, 8 deflated
    std::uint64_t csize = 0, usize = 0;
    std::uint64_t data_off = 0;  // file offset of the entry's bytes
    std::int64_t mtime = 0;      // the DOS stamp as UTC seconds
};

// Reads the central directory. A zip64 archive (over 4 GB or 65535
// entries) is refused with a message: APKs stay below that (the packer
// splits them).
bool ReadZipIndex(const std::string& path, std::vector<ZipEntry>* out, std::string* err);

// base.apk first, then the split_*.apk files of its directory, sorted:
// Android installs an app's split APKs next to its base.apk under that name.
std::vector<std::string> AppApks(const std::string& base_apk);

// A packed file.
struct File {
    int apk = -1;
    std::uint64_t off = 0, size = 0;
    std::int64_t mtime = 0;
    std::uint64_t ino = 0;
};

// One open of a packed file: its window in the APK and the file position.
// Shared by every guest fd dup() makes of it and by a FILE fdopen() makes
// of it, as an open file description is.
struct Slice {
    std::uint64_t off = 0, size = 0, ino = 0;
    std::int64_t mtime = 0;
    dev_t dev = 0;
    std::mutex mu;  // pos
    std::uint64_t pos = 0;
};

// read(2) at the slice position; pread(2) within the slice; lseek(2).
// Return -1 with errno set on failure, as the calls do.
ssize_t SliceRead(Slice& s, int fd, void* buf, std::size_t n);
ssize_t SlicePread(const Slice& s, int fd, void* buf, std::size_t n, std::uint64_t at);
std::int64_t SliceSeek(Slice& s, std::int64_t off, int whence);
// stat of a packed file: a read-write regular file as the guest would see
// it after install (it may change it, by copy-up).
void SliceStat(const Slice& s, struct stat* st);
// A read-only FILE over the slice; it owns fd (fclose closes it).
FILE* SliceFile(int fd, std::shared_ptr<Slice> s);

class Pack {
  public:
    // A root: its name in the APK and its host directory.
    void AddRoot(const std::string& name, const std::string& host_dir);
    // Indexes the APKs, loads the deletion marks, makes the directory
    // skeleton. Returns a one-line summary for the log.
    std::string Build(const std::vector<std::string>& apks, const std::string& marks_file);
    bool Empty() const { return files_.empty(); }

    // By host path, ignoring case; a deleted file is absent.
    const File* Find(const std::string& host_path) const;
    // Packed entries of a host directory, deleted ones left out: name as
    // packed, true for a directory.
    std::vector<std::pair<std::string, bool>> Children(const std::string& host_dir) const;
    // Records that the guest removed host_path (if it is packed).
    void MarkDeleted(const std::string& host_path);

    // A slice for f with its own host fd of the APK (O_RDONLY | O_CLOEXEC);
    // -1 with errno on failure.
    int Open(const File& f, std::shared_ptr<Slice>* out) const;
    void Stat(const File& f, struct stat* st) const;
    // Copies f to a real file at host_path (via a temporary and rename).
    bool CopyUp(const File& f, const std::string& host_path) const;

  private:
    struct Root {
        std::string name, host, host_lc;
    };
    // "<root>/<rel>" in lower case, "" when host_path is under no root.
    std::string Key(const std::string& host_path) const;
    bool LiveFileLocked(const std::string& key) const;
    bool LiveDirLocked(const std::string& key) const;

    std::vector<Root> roots_;
    std::vector<std::string> apks_;
    std::vector<dev_t> apk_dev_;
    std::unordered_map<std::string, File> files_;
    // directory key -> entries (name as packed, is a directory)
    std::unordered_map<std::string, std::vector<std::pair<std::string, bool>>> dirs_;
    std::string marks_file_;
    mutable std::shared_mutex marks_mu_;
    std::unordered_set<std::string> marks_;
};

}  // namespace abvfs
