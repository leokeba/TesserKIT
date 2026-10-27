#include "tesserkit/files.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <utility>

namespace tesserkit {

using tesser::Access;
using tesser::Check;
using tesser::Status;

namespace {

constexpr int64_t kValidTime = 1577836800;  // 2020-01-01: older mtimes are a clock that wasn't set

Check unmounted() { return Check::fail(Status::NotAllowed, "no filesystem"); }

bool isDir(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool exists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

// The directory holding `rel` ("" at the root), and its last name.
std::pair<std::string, std::string> split(const std::string& rel) {
    size_t slash = rel.rfind('/');
    if (slash == std::string::npos) return {"", rel};
    return {rel.substr(0, slash), rel.substr(slash + 1)};
}

// The hidden temporary file a write of `rel` ("dir/name", split) goes to
// before it is renamed over it: ".name.part", so no valid name gives the
// settings' directory (".settings").
std::string tempPath(const std::string& root, const std::pair<std::string, std::string>& parts) {
    return root + "/" + (parts.first.empty() ? "" : parts.first + "/") + "." + parts.second + ".part";
}

Check noRoom() { return Check::fail(Status::TooLarge, "not enough space: the rest is kept for the settings"); }

// A file being uploaded, written to a hidden temporary file beside it and
// renamed over it once complete. `room`: the bytes it may take.
class PathSink : public tesser::FileSink {
public:
    PathSink(Files& files, std::string rel, std::string target, std::string temp, FILE* f, uint64_t room)
        : files_(files),
          rel_(std::move(rel)),
          target_(std::move(target)),
          temp_(std::move(temp)),
          f_(f),
          room_(room) {}
    ~PathSink() override {
        if (f_) fclose(f_);
        if (!done_) ::remove(temp_.c_str());
    }
    Check write(const uint8_t* data, size_t len) override {
        // An upload that announced less than it sends, or nothing.
        if (len > room_ - written_) return noRoom();
        if (fwrite(data, 1, len, f_) != len) return fileWriteError(errno);
        written_ += len;
        return Check::ok();
    }
    Check finish(tesser::JsonWriter& reply) override {
        (void)reply;
        bool flushed = fclose(f_) == 0;
        f_ = nullptr;
        if (!flushed) return fileWriteError(errno);
        // Some VFS drivers don't rename over an existing file.
        if (::rename(temp_.c_str(), target_.c_str()) != 0 &&
            (::remove(target_.c_str()) != 0 || ::rename(temp_.c_str(), target_.c_str()) != 0)) {
            return Check::fail(Status::Internal, "the file couldn't be written");
        }
        done_ = true;
        files_.touched(rel_);
        return Check::ok();
    }

private:
    Files& files_;
    std::string rel_, target_, temp_;
    FILE* f_;
    uint64_t room_, written_ = 0;
    bool done_ = false;
};

class PathSource : public tesser::FileSource {
public:
    PathSource(FILE* f, int64_t size) : f_(f), size_(size) {}
    ~PathSource() override { fclose(f_); }
    int64_t size() const override { return size_; }
    int read(uint8_t* buf, size_t cap) override {
        size_t n = fread(buf, 1, cap, f_);
        if (n == 0 && ferror(f_)) return -1;
        return static_cast<int>(n);
    }

private:
    FILE* f_;
    int64_t size_;
};

}  // namespace

Check fileWriteError(int err) {
    if (err == ENOSPC) return Check::fail(Status::TooLarge, "the filesystem is full");
    // LittleFS allocates its caches as it writes: a short heap fails it.
    if (err == ENOMEM) return Check::fail(Status::Busy, "not enough free memory to write the file now");
    return Check::fail(Status::Internal, "the file couldn't be written");
}

Files::Files(Device& dev) : Service(dev, "files") {}

Files& Files::partition(const char* label) {
    label_ = label;
    return *this;
}

Files& Files::root(const char* path) {
    root_ = path;
    while (root_.size() > 1 && root_.back() == '/') root_.pop_back();
    return *this;
}

Check Files::checkName(std::string_view name) {
    if (name.empty() || name.size() > kMaxName) return Check::fail(Status::InvalidValue, "1 to 32 characters");
    if (name[0] == '.') return Check::fail(Status::InvalidValue, "can't start with a dot");
    for (char c : name) {
        if (c == '/' || c == '\\' || static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
            return Check::fail(Status::InvalidValue, "no slashes or control characters");
        }
    }
    return Check::ok();
}

// Normalizes `rel` ("/a/b/", "a/b") to "a/b", checking each name; "" is the
// root. False if it isn't valid.
static bool normalize(std::string_view rel, std::string& out) {
    out.clear();
    size_t i = 0;
    while (i < rel.size()) {
        size_t end = rel.find('/', i);
        if (end == std::string_view::npos) end = rel.size();
        std::string_view name = rel.substr(i, end - i);
        i = end + 1;
        if (name.empty()) continue;
        if (!Files::checkName(name).isOk()) return false;
        if (!out.empty()) out += '/';
        out.append(name.data(), name.size());
    }
    return out.size() <= Files::kMaxPath;
}

std::string Files::path(std::string_view rel) const {
    std::string n;
    if (!normalize(rel, n)) return std::string();
    return n.empty() ? root_ : root_ + "/" + n;
}

std::vector<FileEntry> Files::list(std::string_view dir, size_t max, bool* truncated) const {
    std::vector<FileEntry> out;
    if (truncated) *truncated = false;
    std::string p = path(dir);
    if (!mounted_ || p.empty()) return out;
    DIR* d = opendir(p.c_str());
    if (!d) return out;
    while (struct dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;  // ".", "..", and uploads in progress
        FileEntry f;
        f.name = e->d_name;
        struct stat st;
        if (stat((p + "/" + f.name).c_str(), &st) != 0) continue;
        f.isDir = S_ISDIR(st.st_mode);
        f.size = f.isDir ? 0 : static_cast<uint32_t>(st.st_size);
        f.modified = st.st_mtime >= kValidTime ? static_cast<int64_t>(st.st_mtime) : 0;
        out.push_back(std::move(f));
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const FileEntry& a, const FileEntry& b) {
        return a.isDir != b.isDir ? a.isDir : a.name < b.name;
    });
    if (out.size() > max) {
        out.resize(max);
        if (truncated) *truncated = true;
    }
    return out;
}

Check Files::mkdir(std::string_view rel) {
    if (!mounted_) return unmounted();
    std::string n;
    if (!normalize(rel, n) || n.empty()) return Check::fail(Status::InvalidValue, "not a valid path");
    // Each directory on the way; the first one made is the change listings see.
    bool made = false;
    for (size_t slash = n.find('/');; slash = n.find('/', slash + 1)) {
        std::string sub = n.substr(0, slash);
        std::string p = root_ + "/" + sub;
        if (!isDir(p)) {
            if (exists(p)) return Check::fail(Status::InvalidValue, "a file has that name");
            if (::mkdir(p.c_str(), 0775) != 0) return Check::fail(Status::Internal, "the directory couldn't be made");
            if (!made) touched(sub);
            made = true;
        }
        if (slash == std::string::npos) break;
    }
    return Check::ok();
}

Check Files::write(std::string_view rel, std::string_view data) {
    if (!mounted_) return unmounted();
    std::string n;
    if (!normalize(rel, n) || n.empty()) return Check::fail(Status::InvalidValue, "not a valid path");
    auto parts = split(n);
    if (!parts.first.empty()) {
        Check c = mkdir(parts.first);
        if (!c.isOk()) return c;
    }
    std::string target = root_ + "/" + n;
    if (isDir(target)) return Check::fail(Status::InvalidValue, "a directory has that name");
    if (settings_ && data.size() > roomBytes()) return noRoom();
    std::string temp = tempPath(root_, parts);
    FILE* f = fopen(temp.c_str(), "wb");
    if (!f) return Check::fail(Status::Internal, "the file couldn't be written");
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    int err = ok ? 0 : errno;
    if (fclose(f) != 0 && ok) {
        ok = false;
        err = errno;
    }
    if (ok && ::rename(temp.c_str(), target.c_str()) != 0) {
        ok = ::remove(target.c_str()) == 0 && ::rename(temp.c_str(), target.c_str()) == 0;
    }
    if (!ok) {
        if (!err) err = errno;
        ::remove(temp.c_str());
        return fileWriteError(err);
    }
    touched(n);
    return Check::ok();
}

Check Files::read(std::string_view rel, std::string& out, size_t max, tesser::Reservation* held) {
    if (!mounted_) return unmounted();
    std::string p = path(rel);
    struct stat st;
    if (p.empty() || stat(p.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) return Check::fail(Status::NotFound, "no such file");
    if (static_cast<uint64_t>(st.st_size) > max) return Check::fail(Status::TooLarge, "the file is too large");
    // One block, from TesserAPI's byte budget: a short heap refuses rather
    // than aborts.
    size_t bytes = static_cast<size_t>(st.st_size) + 1;
    tesser::Reservation r = tesser::reserveBytes(bytes, true, tesser::mallocPool(bytes));
    if (!r) return Check::fail(Status::Busy, "not enough free memory to read the file now");
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return Check::fail(Status::Internal, "the file couldn't be read");
    out.resize(static_cast<size_t>(st.st_size));
    r.settle();
    if (held) *held = std::move(r);
    size_t n = fread(&out[0], 1, out.size(), f);
    fclose(f);
    if (n != out.size()) return Check::fail(Status::Internal, "the file couldn't be read");
    return Check::ok();
}

// Removes `p`, a file or a directory with what it holds.
static bool removeAll(const std::string& p) {
    if (!isDir(p)) return ::remove(p.c_str()) == 0;
    DIR* d = opendir(p.c_str());
    if (!d) return false;
    std::vector<std::string> names;
    while (struct dirent* e = readdir(d)) {
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) names.push_back(e->d_name);
    }
    closedir(d);
    bool ok = true;
    for (const std::string& n : names) ok = removeAll(p + "/" + n) && ok;
    return ::rmdir(p.c_str()) == 0 && ok;
}

Check Files::remove(std::string_view rel) {
    if (!mounted_) return unmounted();
    std::string n;
    if (!normalize(rel, n) || n.empty()) return Check::fail(Status::InvalidValue, "not a valid path");
    std::string p = root_ + "/" + n;
    if (!exists(p)) return Check::fail(Status::NotFound, "no such file");
    bool ok = removeAll(p);
    touched(n);
    return ok ? Check::ok() : Check::fail(Status::Internal, "it couldn't be removed");
}

Check Files::rename(std::string_view from, std::string_view to) {
    if (!mounted_) return unmounted();
    std::string a, b;
    if (!normalize(from, a) || a.empty() || !normalize(to, b) || b.empty()) {
        return Check::fail(Status::InvalidValue, "not a valid path");
    }
    if (!exists(root_ + "/" + a)) return Check::fail(Status::NotFound, "no such file");
    if (exists(root_ + "/" + b)) return Check::fail(Status::InvalidValue, "that name is taken");
    if (b.compare(0, a.size() + 1, a + "/") == 0) return Check::fail(Status::InvalidValue, "into itself");
    std::string into = split(b).first;
    if (!into.empty() && !isDir(root_ + "/" + into)) return Check::fail(Status::NotFound, "no such directory");
    if (::rename((root_ + "/" + a).c_str(), (root_ + "/" + b).c_str()) != 0) {
        return Check::fail(Status::Internal, "it couldn't be renamed");
    }
    touched(a);
    if (split(a).first != into) touched(b);
    return Check::ok();
}

uint64_t Files::freeBytes() const {
    uint64_t total = 0, used = 0;
    if (!mounted_ || !fs_->usage(total, used)) return 0;
    return used < total ? total - used : 0;
}

uint64_t Files::roomBytes() const {
    uint64_t free = freeBytes();
    if (!settings_) return free;
    return free > reserve_ ? free - reserve_ : 0;
}

std::unique_ptr<tesser::FileSink> Files::sink(std::string_view rel, tesser::FileRequest& request) {
    std::string n;
    if (!mounted_) {
        request.refuse(Status::NotAllowed, "no filesystem");
        return nullptr;
    }
    if (!normalize(rel, n) || n.empty()) {
        request.refuse(Status::InvalidValue, "not a valid path");
        return nullptr;
    }
    std::string target = root_ + "/" + n;
    struct stat st;
    bool there = stat(target.c_str(), &st) == 0;
    if (there && S_ISDIR(st.st_mode)) {
        request.refuse(Status::InvalidValue, "a directory has that name");
        return nullptr;
    }
    // The old file is there until the new one is complete.
    uint64_t room = roomBytes();
    if (request.size > room) {
        request.refuse(Status::TooLarge, settings_ && request.size <= freeBytes() ? noRoom().message : "not enough space");
        return nullptr;
    }
    auto parts = split(n);
    if (!parts.first.empty() && !isDir(root_ + "/" + parts.first)) {
        request.refuse(Status::NotFound, "no such directory");
        return nullptr;
    }
    std::string temp = tempPath(root_, parts);
    FILE* f = fopen(temp.c_str(), "wb");
    if (!f) {
        request.refuse(Status::Internal, "the file couldn't be written");
        return nullptr;
    }
    // Checked as it is written, too, when the settings need their room.
    return std::unique_ptr<tesser::FileSink>(
        new PathSink(*this, n, std::move(target), std::move(temp), f, settings_ ? room : UINT64_MAX));
}

std::unique_ptr<tesser::FileSource> Files::source(std::string_view rel, tesser::FileRequest& request) {
    std::string p = mounted_ ? path(rel) : std::string();
    struct stat st;
    if (p.empty() || stat(p.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) {
        request.refuse(Status::NotFound, "no such file");
        return nullptr;
    }
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) {
        request.refuse(Status::Internal, "the file couldn't be read");
        return nullptr;
    }
    request.filename = split(p).second;
    return std::unique_ptr<tesser::FileSource>(new PathSource(f, static_cast<int64_t>(st.st_size)));
}

void describe(tesser::Object& o, FileMove& m) {
    o.value("from", m.from);
    o.value("to", m.to);
}

void Files::touched(std::string_view rel, bool unknown) {
    // The stored backups are listed again only for a change there.
    size_t n = strlen(kBackupDir);
    if (unknown || (rel.compare(0, n, kBackupDir) == 0 && (rel.size() == n || rel[n] == '/'))) {
        backupsChanged_.store(true);
    }
    std::string dir;
    if (!unknown) {
        size_t slash = rel.rfind('/');
        dir = "/";
        if (slash != std::string_view::npos) dir.append(rel.data(), slash);
    }
    {
        tesser::MutexGuard g(pendingLock_);
        if (std::find(pending_.begin(), pending_.end(), dir) == pending_.end()) {
            // Many at once (a format, an application's burst): one "anything".
            if (pending_.size() < 8) {
                pending_.push_back(std::move(dir));
            } else {
                pending_.assign(1, std::string());
            }
        }
    }
    generation_.fetch_add(1);
}

void Files::refresh(uint32_t nowMs) {
    listed_ = generation();
    refreshedAtMs_ = nowMs;
    if (mounted_) {
        uint64_t total = 0, used = 0;
        if (fs_->usage(total, used)) {
            total_ = total;
            used_ = used;
        }
    }
    if (object_) object_->changed();
}

void Files::listCall(const std::string& dir, tesser::Call& call) const {
    std::string n;
    if (!mounted_) return call.fail(Status::NotAllowed, "no filesystem");
    if (!normalize(dir, n)) return call.fail(Status::InvalidValue, "not a valid path");
    if (!isDir(n.empty() ? root_ : root_ + "/" + n)) return call.fail(Status::NotFound, "no such directory");
    bool truncated = false;
    std::vector<FileEntry> entries = list(n, kMaxEntries, &truncated);
    call.replyWith([&](tesser::JsonWriter& w) {
        w.beginObject();
        w.key("path");
        w.string("/" + n);
        w.key("entries");
        w.beginArray();
        for (const FileEntry& e : entries) {
            w.beginObject();
            w.key("name");
            w.string(e.name);
            w.key("size");
            w.uinteger(e.size);
            w.key("dir");
            w.boolean(e.isDir);
            w.key("modified");
            w.integer(e.modified);
            w.endObject();
        }
        w.endArray();
        w.key("truncated");
        w.boolean(truncated);
        w.endObject();
    });
}

void Files::describe(tesser::Object& o) {
    object_ = &o;
    // TesserUI's file browser (its "files" widget) speaks to the children
    // below by name, and keeps the folder shown in its own route.
    o.label("Files")
        .ui("icon", "folder")
        .ui("group", "Maintenance")
        .ui("summary", "used")
        .ui("widget", "files")
        .readAccess(Access::Admin);
    o.value("mounted", static_cast<const bool&>(mounted_)).ui("advanced");
    o.value("total", static_cast<const uint64_t&>(total_)).unit("B");
    o.value("used", static_cast<const uint64_t&>(used_)).unit("B");
    o.action("list", [this](const std::string& dir, tesser::Call& call) { listCall(dir, call); })
        .doc("A directory's entries, by its path (\"/\" the root): {path, entries: [{name, size, dir, modified}], "
             "truncated}, at most 64");
    fileNode_ = &o.file("file")
                     .download([this](tesser::FileRequest& r) -> std::unique_ptr<tesser::FileSource> {
                         if (r.filename.empty()) {
                             r.refuse(Status::InvalidValue, "which file? its path in ?filename=");
                             return nullptr;
                         }
                         return source(r.filename, r);
                     })
                     .upload([this](tesser::FileRequest& r) -> std::unique_ptr<tesser::FileSink> {
                         if (r.filename.empty()) {
                             r.refuse(Status::InvalidValue, "which file? its path in ?filename=");
                             return nullptr;
                         }
                         return sink(r.filename, r);
                     })
                     .label("File")
                     .doc("Any file, by its path in ?filename= (/www/index.html); an upload replaces it whole, "
                          "once complete");
    o.action("mkdir", [this](const std::string& dir) {
         std::string p = path(dir);
         if (!p.empty() && p != root_ && exists(p)) return Check::fail(Status::InvalidValue, "that name is taken");
         return mkdir(dir);
     }).label("New folder");
    o.action("move", [this](const FileMove& m) { return rename(m.from, m.to); })
        .label("Move")
        .doc("Renames a file or a folder, or moves it to another folder: {from, to}, two paths");
    o.action("delete", [this](const std::string& p) { return remove(p); })
        .label("Delete")
        .doc("A file, or a folder with what it holds");
    changedNode_ = &o.event("changed")
                        .label("Changed")
                        .doc("A folder whose entries changed (\"/www\"), or \"\" when it isn't known which")
                        .ui("hidden");
    o.action("format", [this] {
         if (!mounted_) return unmounted();
         // With the settings here, every file but theirs: a format would
         // leave them only in memory until written again, so a power cut
         // or a short heap then would lose them all, Wi-Fi's included.
         bool ok = settings_ ? eraseFiles() : fs_->format();
         changed();
         refresh(dev_.loopMs());
         return ok ? Check::ok() : Check::fail(Status::Internal, "the filesystem couldn't be formatted");
     })
        .label("Erase every file")
        .ui("confirm", "Erase every file?")
        .ui("advanced");
}

bool Files::mount() {
    if (mountTried_) return mounted_;
    mountTried_ = true;
    Platform& p = dev_.platform();
    if (!p.fs) p.fs = espFilesystem();
    fs_ = p.fs;
    mounted_ = fs_ && fs_->mount(label_, root_.c_str(), mountError_);
    if (!mounted_ && mountError_.empty()) {
        tesser::logError("tesserkit: no filesystem (a \"files\" or \"spiffs\" partition, and LittleFS)");
    }
    return mounted_;
}

tesser::Storage* Files::settingsStorage(tesser::Storage* nvs, const std::vector<std::string>& keys, std::string& why) {
    why.clear();
    // The filesystem's settings can't be used this boot, but they are there:
    // if they were moved before, NVS's copy dates from the move.
    auto older = [nvs, &why] {
        std::string marker;
        if (nvs && nvs->load(SettingsStore::kMoved, marker)) {
            why += "; NVS's copy is older than the filesystem's";
            tesser::logError("tesserkit: settings: %s", why.c_str());
        }
    };
    if (!mount()) {
        why = "the filesystem didn't mount";
        if (!mountError_.empty()) why += " (" + mountError_ + ")";
        older();
        return nullptr;
    }
    settingsFiles_.reset(new tesser::FileStorage(root_ + "/" + kSettingsDir));
    std::string said;
    switch (moveSettings(nvs, *settingsFiles_, keys, said)) {
        case Moved::Unreadable:
            settingsFiles_.reset();
            why = said;
            older();
            return nullptr;
        case Moved::Failed:
            settingsFiles_.reset();
            why = "moving them to the filesystem failed (" + said + ")";
            return nullptr;
        case Moved::Restored:
            why = said;  // in use, but told: what the filesystem held is gone
            break;
        case Moved::Before:
        case Moved::Now:
            break;
    }
    settings_ = true;
    return settingsFiles_.get();
}

Files& Files::settingsReserve(size_t bytes) {
    reserve_ = bytes;
    return *this;
}

bool Files::eraseFiles() {
    DIR* d = opendir(root_.c_str());
    if (!d) return false;
    std::vector<std::string> names;
    while (struct dirent* e = readdir(d)) {
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0 && strcmp(e->d_name, kSettingsDir) != 0) {
            names.push_back(e->d_name);
        }
    }
    closedir(d);
    bool ok = true;
    for (const std::string& n : names) ok = removeAll(root_ + "/" + n) && ok;
    return ok;
}

void Files::start() {
    mount();  // done already if the settings are here
    generation_.fetch_add(1);  // read at the first tick; no client listed anything yet
    refresh(dev_.loopMs());
    // A file larger than the partition is refused before it is sent.
    if (mounted_) fileNode_->maxSize(total_);
    backupsChanged_.store(false);
    refreshBackups();
}

void Files::tick(uint32_t nowMs) {
    // The usage and the stored backups follow changes at most every
    // kRefreshMs; `changed` below goes out at once.
    if (nowMs - refreshedAtMs_ >= kRefreshMs) {
        if (generation() != listed_) refresh(nowMs);
        if (backupsChanged_.exchange(false)) {
            refreshedAtMs_ = nowMs;
            refreshBackups();
        }
    }
    std::vector<std::string> dirs;
    {
        tesser::MutexGuard g(pendingLock_);
        dirs.swap(pending_);
    }
    for (const std::string& d : dirs) changedNode_->emit(d);
}

void Files::factoryReset() {
    if (!mounted_) return;
    remove(kBackupDir);
    // Settings left there while this boot kept them in NVS (a marker that
    // couldn't be read): erased too, or the next boot would bring them back.
    if (!settings_) tesser::FileStorage(root_ + "/" + kSettingsDir).erase();
}

}  // namespace tesserkit
