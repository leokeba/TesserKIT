// Where the settings live (docs/DESIGN.md section 6.1): files in /.settings
// on the Files service's filesystem when it mounts, moved there from NVS at
// the first boot; NVS otherwise.
#pragma once

#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <map>

#include "rig.h"

namespace settings_cases {

using rig::Status;

// A device with Files and the default storage: fake.nvs stands for NVS.
struct SettingsRig : rig::Rig {
    tesserkit::System system{dev};
    tesserkit::Time time{dev};
    tesserkit::Files files{dev};
    tesserkit::Backup backup{dev};
    int level = 3;
    std::string note;
    // `dir`: a filesystem kept from an earlier boot (keepDir()).
    explicit SettingsRig(const std::string& dir = "") : rig::Rig(true, false) {
        if (!dir.empty()) {
            rmdir(fake.fs.dir.c_str());
            fake.fs.dir = dir;
        }
        files.root(fake.fs.dir.c_str());
        auto& app = dev.api().object("app");
        app.value("level", level).range(0, 10).persist();
        app.value("note", note).persist();
    }
    // The filesystem's directory, kept for the next boot's rig, which
    // removes it.
    std::string keepDir() {
        std::string d = fake.fs.dir;
        fake.fs.dir.clear();
        return d;
    }
    std::string path(const std::string& rel) const { return fake.fs.dir + "/" + rel; }
    bool onDisk(const std::string& rel) const {
        struct stat st;
        return stat(path(rel).c_str(), &st) == 0;
    }
    std::string file(const std::string& rel) const {
        std::string out;
        if (FILE* f = fopen(path(rel).c_str(), "rb")) {
            char buf[256];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
            fclose(f);
        }
        return out;
    }
    // The records in /.settings, as "name,name" (without .json).
    std::string records() const {
        std::vector<std::string> names;
        if (DIR* d = opendir(path(".settings").c_str())) {
            while (struct dirent* e = readdir(d)) {
                std::string n = e->d_name;
                if (n.size() > 5 && n.compare(n.size() - 5, 5, ".json") == 0) names.push_back(n.substr(0, n.size() - 5));
            }
            closedir(d);
        }
        std::sort(names.begin(), names.end());
        std::string out;
        for (const std::string& n : names) out += (out.empty() ? "" : ",") + n;
        return out;
    }
};

TEST(settings_on_the_filesystem) {
    SettingsRig r;
    CHECK(r.dev.begin());
    CHECK(r.dev.settingsOnFiles());
    CHECK(r.files.holdsSettings());
    CHECK_EQ(r.value("/sys/status/settings"), "\"\"");
    CHECK_EQ(r.set("/sys/time/tz", "\"EST5EDT\"").status, Status::Ok);
    CHECK_EQ(r.set("/app/level", "7").status, Status::Ok);
    r.run(2500);
    CHECK(r.file(".settings/sys.json").find("EST5EDT") != std::string::npos);
    CHECK_EQ(r.file(".settings/app.json"), "{\"level\":7}");
    CHECK_EQ(r.records(), "@moved,@version,app,sys");
    CHECK(r.fake.nvs.records.empty());
    CHECK_EQ(r.fake.nvs.saves, 0);
    std::string dir = r.keepDir();

    // The next boot loads them from there.
    SettingsRig next(dir);
    CHECK(next.dev.begin());
    CHECK_EQ(next.value("/sys/time/tz"), "\"EST5EDT\"");
    CHECK_EQ(next.value("/app/level"), "7");
}

// The first boot with a filesystem moves NVS's records there. NVS keeps its
// copy, no longer written: the device's fallback if the filesystem ever
// doesn't mount.
TEST(settings_moved_from_nvs) {
    std::map<std::string, std::string> nvs = {
        {"@version", "1"},
        {"sys", "{\"time\":{\"tz\":\"EST5EDT\"}}"},
        {"app", "{\"level\":4}"},
        {"gone", "{\"x\":1}"},  // no such node any more: not TesserKIT's to touch
    };
    std::string dir;
    {
        SettingsRig r;
        r.fake.nvs.records = nvs;
        CHECK(r.dev.begin());
        CHECK(r.dev.settingsOnFiles());
        CHECK_EQ(r.value("/sys/time/tz"), "\"EST5EDT\"");
        CHECK_EQ(r.value("/app/level"), "4");
        CHECK_EQ(r.records(), "@moved,@version,app,sys");
        CHECK_EQ(r.file(".settings/sys.json"), nvs["sys"]);
        CHECK_EQ(r.file(".settings/app.json"), nvs["app"]);
        CHECK(r.fake.nvs.records == nvs);  // kept, as they were
        CHECK_EQ(r.value("/sys/status/settings"), "\"\"");
        // Saved to the filesystem from now on, not to NVS.
        CHECK_EQ(r.set("/sys/time/tz", "\"PST8PDT\"").status, Status::Ok);
        r.run(2500);
        CHECK(r.file(".settings/sys.json").find("PST8PDT") != std::string::npos);
        CHECK(r.fake.nvs.records == nvs);
        dir = r.keepDir();
    }
    // Moved before: the filesystem's copy is the one used, whatever NVS holds.
    SettingsRig next(dir);
    next.fake.nvs.records = nvs;
    CHECK(next.dev.begin());
    CHECK(next.dev.settingsOnFiles());
    CHECK_EQ(next.value("/sys/time/tz"), "\"PST8PDT\"");
    CHECK(next.fake.nvs.records == nvs);
}

// Moved, then the filesystem doesn't mount one day: the device starts from
// the copy NVS kept (its network among them), not from defaults.
TEST(settings_fall_back_to_nvs_copy) {
    std::map<std::string, std::string> nvs = {
        {"@version", "1"},
        {"sys", "{\"time\":{\"tz\":\"EST5EDT\"}}"},
    };
    std::string dir;
    {
        SettingsRig r;
        r.fake.nvs.records = nvs;
        CHECK(r.dev.begin());
        CHECK(r.dev.settingsOnFiles());
        nvs = r.fake.nvs.records;  // what NVS holds after the move
        dir = r.keepDir();
    }
    SettingsRig next(dir);
    next.fake.nvs.records = nvs;
    next.fake.fs.present = false;
    CHECK(next.dev.begin());
    CHECK(!next.dev.settingsOnFiles());
    CHECK_EQ(next.value("/sys/time/tz"), "\"EST5EDT\"");
    CHECK_EQ(next.value("/sys/status/settings"), "\"in NVS: the filesystem didn't mount\"");
}

// A move that fails midway leaves NVS as it was and the filesystem empty:
// the settings stay in NVS for this boot, and move at the next.
TEST(settings_move_failing_midway_keeps_nvs) {
    std::map<std::string, std::string> nvs = {
        {"@version", "1"},
        {"sys", "{\"time\":{\"tz\":\"EST5EDT\"}}"},
        {"app", "{\"note\":\"" + std::string(2000, 'n') + "\"}"},
    };
    std::string dir;
    {
        SettingsRig r;
        r.fake.nvs.records = nvs;
        {
            // Room for the small records' copies, not for app's.
            rig::BudgetLimit limit(tesserkit::kFileWriteBytes + 1000);
            CHECK(r.dev.begin());
        }
        CHECK(!r.dev.settingsOnFiles());
        CHECK(!r.files.holdsSettings());
        CHECK(r.fake.nvs.records == nvs);
        CHECK_EQ(r.records(), "");
        CHECK_EQ(r.value("/sys/time/tz"), "\"EST5EDT\"");  // loaded from NVS
        CHECK_EQ(r.value("/sys/status/settings"),
                 "\"in NVS: moving them to the filesystem failed (no memory to move \\\"app\\\")\"");
        // Saved to NVS meanwhile.
        CHECK_EQ(r.set("/app/level", "5").status, Status::Ok);
        r.run(2500);
        CHECK_EQ(r.records(), "");
        CHECK(r.fake.nvs.records["app"].find("\"level\":5") != std::string::npos);
        nvs = r.fake.nvs.records;
        dir = r.keepDir();
    }
    SettingsRig next(dir);
    next.fake.nvs.records = nvs;
    CHECK(next.dev.begin());
    CHECK(next.dev.settingsOnFiles());
    CHECK_EQ(next.records(), "@moved,@version,app,sys");
    CHECK_EQ(next.value("/app/level"), "5");
    CHECK(next.fake.nvs.records == nvs);  // NVS keeps its copy
}

TEST(settings_in_nvs_without_a_filesystem) {
    SettingsRig r;
    r.fake.fs.present = false;
    r.fake.nvs.records = {{"sys", "{\"time\":{\"tz\":\"EST5EDT\"}}"}};
    CHECK(r.dev.begin());
    CHECK(!r.dev.settingsOnFiles());
    CHECK_EQ(r.value("/sys/time/tz"), "\"EST5EDT\"");
    CHECK_EQ(r.value("/sys/status/settings"), "\"in NVS: the filesystem didn't mount\"");
    CHECK_EQ(r.set("/app/level", "6").status, Status::Ok);
    r.run(2500);
    CHECK_EQ(r.fake.nvs.records["app"], "{\"level\":6}");

    // Without Files, NVS is where they belong: nothing to say.
    struct Plain : rig::Rig {
        tesserkit::System system{dev};
        tesserkit::Time time{dev};
        Plain() : rig::Rig(true, false) {}
    } p;
    CHECK(p.dev.begin());
    CHECK(!p.dev.settingsOnFiles());
    CHECK_EQ(p.value("/sys/status/settings"), "\"\"");
    CHECK_EQ(p.set("/sys/time/tz", "\"EST5EDT\"").status, Status::Ok);
    p.run(2500);
    CHECK(p.fake.nvs.records["sys"].find("EST5EDT") != std::string::npos);
}

// /.settings is out of reach of /sys/files and of Files' helpers.
TEST(settings_hidden_from_files) {
    SettingsRig r;
    CHECK(r.dev.begin());
    CHECK_EQ(r.set("/sys/time/tz", "\"EST5EDT\"").status, Status::Ok);
    r.run(2500);
    std::string sys = r.file(".settings/sys.json");
    CHECK(!sys.empty());
    CHECK_EQ(r.uploadNamed("/sys/files/upload", "a.txt", "A").status, Status::Ok);
    r.step();
    std::string listed = r.value("/sys/files/entries");
    CHECK(listed.find("a.txt") != std::string::npos);
    CHECK_EQ(listed.find("settings"), std::string::npos);
    CHECK_EQ(rig::at(listed, "1"), "<missing>");
    CHECK_EQ(r.uploadNamed("/sys/files/upload", ".settings", "x").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/files/dir", "\"/.settings\"").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/files/mkdir", "\".settings\"").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/files/entries/a.txt/rename", "\".settings\"").status, Status::InvalidValue);
    CHECK_EQ(r.download("/sys/files/entries/.settings/content").status, Status::NotFound);
    std::string text;
    CHECK_EQ(r.files.read(".settings/sys.json", text, 100000).status, Status::NotFound);
    CHECK_EQ(r.files.write(".settings/sys.json", "{}").status, Status::InvalidValue);
    CHECK_EQ(r.files.remove(".settings").status, Status::InvalidValue);
    CHECK_EQ(r.files.rename(".settings", "s").status, Status::InvalidValue);
    // A file named "settings": its upload's temporary file isn't the
    // settings' directory.
    CHECK_EQ(r.uploadNamed("/sys/files/upload", "settings", "mine").status, Status::Ok);
    CHECK(r.files.write("settings", "again").isOk());
    CHECK_EQ(r.file("settings"), "again");
    CHECK_EQ(r.file(".settings/sys.json"), sys);
}

// Formatting erases every file but the settings.
TEST(settings_kept_by_format) {
    SettingsRig r;
    CHECK(r.dev.begin());
    CHECK_EQ(r.set("/sys/time/tz", "\"EST5EDT\"").status, Status::Ok);
    r.run(2500);
    std::string sys = r.file(".settings/sys.json");
    r.set("/sys/files/mkdir", "\"logs\"");
    r.set("/sys/files/dir", "\"/logs\"");
    r.uploadNamed("/sys/files/upload", "x.log", "x");
    r.set("/sys/files/dir", "\"/\"");
    r.uploadNamed("/sys/files/upload", "a.txt", "A");
    CHECK_EQ(r.set("/sys/files/format").status, Status::Ok);
    CHECK_EQ(r.fake.fs.formats, 0);
    CHECK(!r.onDisk("logs"));
    CHECK(!r.onDisk("a.txt"));
    CHECK_EQ(r.file(".settings/sys.json"), sys);
    CHECK_EQ(r.records(), "@moved,@version,sys");
    r.step();
    CHECK_EQ(r.value("/sys/files/entries"), "[]");
    // Still saved there.
    CHECK_EQ(r.set("/app/level", "8").status, Status::Ok);
    r.run(2500);
    CHECK_EQ(r.file(".settings/app.json"), "{\"level\":8}");
}

// Files keeps room for the settings: an upload or a write that would leave
// less than the reserve is refused.
TEST(settings_reserve) {
    SettingsRig r;
    CHECK(r.dev.begin());
    r.run(2500);
    uint64_t room = r.files.freeBytes() - tesserkit::Files::kSettingsReserve;
    CHECK_EQ(r.files.roomBytes(), room);
    rig::Reply u = r.uploadNamed("/sys/files/upload", "big.bin", std::string(room + 1, 'b'));
    CHECK_EQ(u.status, Status::TooLarge);
    CHECK(u.body.find("kept for the settings") != std::string::npos);
    CHECK(!r.onDisk("big.bin"));
    CHECK_EQ(r.files.write("big.bin", std::string(room + 1, 'b')).status, Status::TooLarge);
    // An upload that announces less than it sends is stopped as it writes.
    tesser::Client c;
    tesser::FileRequest req(c);
    std::unique_ptr<tesser::FileSink> s = r.files.sink("sneaky.bin", req);
    CHECK(s != nullptr);
    std::string chunk(4096, 's');
    tesser::Check last = tesser::Check::ok();
    for (uint64_t sent = 0; last.isOk() && sent <= room; sent += chunk.size()) {
        last = s->write(reinterpret_cast<const uint8_t*>(chunk.data()), chunk.size());
    }
    CHECK_EQ(last.status, Status::TooLarge);
    s.reset();
    CHECK(!r.onDisk("sneaky.bin"));
    // Within the room: fine.
    CHECK_EQ(r.uploadNamed("/sys/files/upload", "fits.bin", std::string(room - 4096, 'f')).status, Status::Ok);
}

TEST(settings_factory_reset_erases_them) {
    SettingsRig r;
    CHECK(r.dev.begin());
    CHECK_EQ(r.set("/sys/time/tz", "\"EST5EDT\"").status, Status::Ok);
    CHECK_EQ(r.set("/app/level", "9").status, Status::Ok);
    r.run(2500);
    CHECK_EQ(r.records(), "@moved,@version,app,sys");
    r.uploadNamed("/sys/files/upload", "keep.txt", "data");
    r.fake.nvs.records["sys"] = "{\"time\":{\"tz\":\"EST5EDT\"}}";  // a copy a move left behind
    r.dev.factoryReset();
    r.step();
    CHECK_EQ(r.records(), "");
    CHECK(r.fake.nvs.records.empty());
    CHECK(r.onDisk("keep.txt"));  // files are data: they stay
    r.run(2500);
    CHECK_EQ(r.records(), "");  // nothing saved after the reset
    std::string dir = r.keepDir();

    SettingsRig next(dir);
    CHECK(next.dev.begin());
    CHECK(next.dev.settingsOnFiles());
    CHECK_EQ(next.value("/sys/time/tz"), "\"UTC0\"");
    CHECK_EQ(next.value("/app/level"), "3");
}

// A save without the heap for the filesystem's buffers waits: tried again
// until there is.
TEST(settings_save_waits_for_memory) {
    SettingsRig r;
    CHECK(r.dev.begin());
    r.run(2500);
    CHECK_EQ(r.set("/sys/time/tz", "\"EST5EDT\"").status, Status::Ok);
    {
        rig::BudgetLimit limit(512);  // under the write's buffers
        r.run(2500);
        CHECK(r.file(".settings/sys.json").find("EST5EDT") == std::string::npos);
        // Told, not silent: a change not saved yet is lost at a restart.
        CHECK_EQ(r.value("/sys/status/settings"), "\"\\\"sys\\\" waiting to be saved: not enough free memory\"");
    }
    r.run(200);
    CHECK(r.file(".settings/sys.json").find("EST5EDT") != std::string::npos);
    CHECK_EQ(r.value("/sys/status/settings"), "\"\"");
}

// A restore whose save waits for memory is applied all the same: ok, with
// "saved": false, no restart (which would lose it), and saved once memory
// allows (seen on DEVKIT-V3 over its serial line, 2026-10-15).
TEST(settings_restore_waits_for_memory) {
    SettingsRig r;
    CHECK(r.dev.begin());
    CHECK_EQ(r.set("/sys/time/tz", "\"EST5EDT\"").status, Status::Ok);
    r.run(2500);
    rig::Reply d = r.download("/sys/backup/settings");
    CHECK_EQ(d.status, Status::Ok);
    CHECK_EQ(r.set("/sys/time/tz", "\"CET-1CEST\"").status, Status::Ok);
    r.run(2500);
    rig::Reply u;
    {
        // Room for the restore's document, not for the save's write.
        rig::BudgetLimit limit(2 * d.body.size() + 256);
        u = r.upload("/sys/backup/settings", d.body);
        CHECK_EQ(u.status, Status::Ok);
        CHECK(u.body.find("\"saved\":false") != std::string::npos);
        CHECK(u.body.find("\"restart\":false") != std::string::npos);
        CHECK_EQ(r.value("/sys/time/tz"), "\"EST5EDT\"");  // applied
        CHECK(r.value("/sys/status/settings").find("waiting to be saved") != std::string::npos);
    }
    r.run(2500);  // TesserAPI's debounce
    CHECK(r.file(".settings/sys.json").find("EST5EDT") != std::string::npos);
    CHECK_EQ(r.value("/sys/status/settings"), "\"\"");
    CHECK_EQ(r.fake.core.restarts, 0);
}

// Backups work the same with the settings on the filesystem.
TEST(settings_backup_and_restore_on_files) {
    SettingsRig r;
    CHECK(r.dev.begin());
    CHECK_EQ(r.set("/sys/time/tz", "\"EST5EDT\"").status, Status::Ok);
    CHECK_EQ(r.set("/sys/backup/restartAfter", "false").status, Status::Ok);
    r.run(2500);
    rig::Reply d = r.download("/sys/backup/settings");
    CHECK_EQ(d.status, Status::Ok);
    CHECK_EQ(r.set("/sys/time/tz", "\"CET-1CEST\"").status, Status::Ok);
    r.run(2500);
    CHECK(r.file(".settings/sys.json").find("CET-1CEST") != std::string::npos);
    CHECK_EQ(r.upload("/sys/backup/settings", d.body).status, Status::Ok);
    CHECK(r.file(".settings/sys.json").find("EST5EDT") != std::string::npos);  // saved at once
    CHECK_EQ(r.value("/sys/time/tz"), "\"EST5EDT\"");
    // Stored backups sit beside them, as files of their own.
    CHECK_EQ(r.set("/sys/backup/save", "\"one\"").status, Status::Ok);
    CHECK(r.onDisk("backups/one.json"));
}

}  // namespace settings_cases
