#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "tesserkit/device.h"

namespace tesserkit {

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

    void describe(tesser::Object& o) override;

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
    // Reads a whole file of at most `max` bytes.
    tesser::Check read(std::string_view rel, std::string& out, size_t max);
    // Removes a file, or a directory with what it holds.
    tesser::Check remove(std::string_view rel);
    tesser::Check rename(std::string_view from, std::string_view to);
    tesser::Check mkdir(std::string_view rel);
    // Free bytes (0 if not mounted).
    uint64_t freeBytes() const;
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
    Filesystem* fs_ = nullptr;
    bool mounted_ = false;
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
