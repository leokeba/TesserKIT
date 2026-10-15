#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "tesserkit/device.h"

namespace tesserkit {

// Why a write to the filesystem failed, from its errno: ENOSPC is a full
// filesystem, ENOMEM a heap too short for the filesystem's buffers (busy:
// try again), anything else an error of its own.
tesser::Check fileWriteError(int err);

class Backup;
class StoredBackups;

// An entry of a directory, as /sys/files/entries and /sys/backup/stored list
// them.
struct FileEntry {
    std::string name;
    uint32_t size = 0;
    bool isDir = false;
    int64_t modified = 0;  // epoch seconds, 0 if unknown
};

// /sys/files: a LittleFS partition, browsed one directory at a time
// (docs/DESIGN.md section 5.12). The application reads and writes its files
// through stdio below root(), and other services (Backup) through the
// helpers here.
class Files : public Service {
public:
    static constexpr size_t kMaxEntries = 64;  // listed per directory
    static constexpr size_t kMaxName = 32;     // characters of a name
    static constexpr size_t kMaxPath = 63;     // characters of a path below the root

    explicit Files(Device& dev);
    ~Files() override;

    // Options, before Device::begin().
    // The partition's label (default: "files", else "spiffs", the label of
    // Arduino's tables).
    Files& partition(const char* label);
    // The mount point (default "/files"). An application that mounts the
    // partition itself (Arduino's LittleFS.begin()) gives its own.
    Files& root(const char* path);
    // The free space kept for the settings when they live here: uploads and
    // writes that would leave less are refused (default kSettingsReserve).
    Files& settingsReserve(size_t bytes);

    void describe(tesser::Object& o) override;

    // Mounts the partition, once: before the settings load if they live
    // here (settingsStorage()), else as the service starts. True if it is
    // mounted.
    bool mount();
    // The settings' storage on this filesystem (docs/DESIGN.md section 6.1):
    // mounts it, moves the records named `keys` there from `nvs` (null if
    // there is none) at the first boot, and keeps them in kSettingsDir from
    // then on, with room kept for them and `format` sparing them. Null if
    // they can't live here, `why` saying why ("the filesystem didn't
    // mount"). Virtual, so a firmware without Files doesn't link it.
    virtual tesser::Storage* settingsStorage(tesser::Storage* nvs, const std::vector<std::string>& keys,
                                             std::string& why);
    bool holdsSettings() const { return settings_; }
    // The settings' directory at the root: hidden, so no name Files accepts
    // reaches it (names can't start with a dot), and no upload's temporary
    // file is named so (theirs end in ".part").
    static constexpr const char* kSettingsDir = ".settings";
    // The worst case of TesserKIT's records in 4 KB LittleFS blocks: sys
    // (16.7 KB: 5 blocks) and sequencer (10.3 KB: 3), the temporary copy of
    // sys while it is rewritten (5), the directory's metadata pair (2), and
    // one block for LittleFS to move metadata to (docs/DESIGN.md section 6.1).
    static constexpr size_t kSettingsReserve = 64 * 1024;

    bool mounted() const { return mounted_; }
    const std::string& root() const { return root_; }
    // The absolute path of `rel` ("backups/a.json", leading '/' optional),
    // or "" if it isn't a valid path below the root.
    std::string path(std::string_view rel) const;
    // Whether `name` can name an entry: 1 to kMaxName characters, no '/',
    // no control characters, not "." or "..".
    static tesser::Check checkName(std::string_view name);

    // What the application changed: the listings are read again. Thread-safe.
    void changed() { generation_.fetch_add(1); }
    // Raised by every change, here or through changed(). Thread-safe.
    uint32_t generation() const { return generation_.load(); }

    // Helpers, for any task. Paths are relative to the root.
    // The entries of `dir`, directories first, then by name; at most `max`,
    // with `truncated` set if there were more.
    std::vector<FileEntry> list(std::string_view dir, size_t max = kMaxEntries, bool* truncated = nullptr) const;
    // Writes a whole file through a temporary one, so a power cut leaves the
    // old or the new file; creates the directories on the way.
    tesser::Check write(std::string_view rel, std::string_view data);
    // Reads a whole file of at most `max` bytes. Its buffer is reserved from
    // TesserAPI's byte budget first (busy if refused), and held in `held`
    // while the caller keeps it, if given.
    tesser::Check read(std::string_view rel, std::string& out, size_t max, tesser::Reservation* held = nullptr);
    // Removes a file, or a directory with what it holds.
    tesser::Check remove(std::string_view rel);
    tesser::Check rename(std::string_view from, std::string_view to);
    tesser::Check mkdir(std::string_view rel);
    // Free bytes (0 if not mounted).
    uint64_t freeBytes() const;
    // What a file may take: the free bytes, less the settings' reserve when
    // they live here.
    uint64_t roomBytes() const;
    // An upload's sink (written to a temporary file, renamed by finish()),
    // and a download's source; null after `request.refuse()`.
    std::unique_ptr<tesser::FileSink> sink(std::string_view rel, tesser::FileRequest& request);
    std::unique_ptr<tesser::FileSource> source(std::string_view rel, tesser::FileRequest& request);

    // Backup's stored backups (docs/DESIGN.md section 5.5), in kBackupDir.
    // Virtual, so a firmware without Files doesn't link them: Backup reaches
    // them through the device's Files only.
    static constexpr const char* kBackupDir = "backups";
    // What a restore stores first, so it can be undone. Not counted by keep.
    static constexpr const char* kBeforeRestore = "before-restore";
    virtual void describeBackups(Backup& backup, tesser::Object& o);
    virtual tesser::Check storeBackup(Backup& backup, const std::string& name, std::string* saved);

protected:
    void start() override;
    void tick(uint32_t nowMs) override;
    // Stored backups hold the settings, secrets included: they go with them.
    void factoryReset() override;
    Files* asFiles() override { return this; }

private:
    void refresh();
    void refreshBackups();  // stored_backups.cpp
    std::string relative(std::string_view name) const;  // name in dir_
    tesser::Check setDir(const std::string& dir);

    const char* label_ = nullptr;
    std::string root_ = "/files";
    // Erases every file and directory but the settings'.
    bool eraseFiles();

    Filesystem* fs_ = nullptr;
    bool mountTried_ = false;
    bool mounted_ = false;
    bool settings_ = false;  // the settings live here
    size_t reserve_ = kSettingsReserve;
    std::unique_ptr<tesser::FileStorage> settingsFiles_;
    uint64_t total_ = 0, used_ = 0;
    std::string dir_ = "/";
    std::vector<FileEntry> entries_;
    bool truncated_ = false;
    std::atomic<uint32_t> generation_{0};
    uint32_t listed_ = 0;  // the generation entries_ shows
    tesser::Node* entriesNode_ = nullptr;
    tesser::Node* dirNode_ = nullptr;
    tesser::FileNode* uploadNode_ = nullptr;
    tesser::Object* object_ = nullptr;
    StoredBackups* backups_ = nullptr;  // with a Backup service; owned
};

}  // namespace tesserkit
