// Files (docs/DESIGN.md section 5.12) and the backups stored in it (5.5).
#pragma once

#include <errno.h>

#include <stdio.h>
#include <sys/stat.h>

#include "rig.h"

namespace files_cases {

using rig::at;
using rig::Status;

struct FilesRig : rig::Rig {
    tesserkit::System system{dev};
    tesserkit::Time time{dev};
    tesserkit::Files files{dev};
    tesserkit::Backup backup{dev};
    int level = 3;
    FilesRig() {
        files.root(fake.fs.dir.c_str());
        dev.api().object("app").value("level", level).range(0, 10).persist();
    }
    bool onDisk(const std::string& rel) {
        struct stat st;
        return stat((fake.fs.dir + "/" + rel).c_str(), &st) == 0;
    }
    std::string names(const char* list) {
        std::string json = value(list), out;
        for (size_t i = 0;; i++) {
            std::string n = at(json, (std::to_string(i) + "/name").c_str());
            if (n == "<missing>") break;
            out += (out.empty() ? "" : ",") + n.substr(1, n.size() - 2);
        }
        return out;
    }
};

// A failed write says why: a full filesystem, or memory to try again for.
TEST(files_write_errors_say_why) {
    CHECK_EQ(tesserkit::fileWriteError(ENOSPC).status, rig::Status::TooLarge);
    CHECK_EQ(tesserkit::fileWriteError(ENOMEM).status, rig::Status::Busy);
    CHECK_EQ(tesserkit::fileWriteError(EIO).status, rig::Status::Internal);
}

TEST(files_mount) {
    FilesRig r;
    CHECK(r.dev.begin());
    CHECK_EQ(r.fake.fs.root, r.fake.fs.dir);
    CHECK_EQ(r.fake.fs.label, "");  // the default: "files", else "spiffs"
    CHECK(r.files.mounted());
    CHECK_EQ(r.value("/sys/files/mounted"), "true");
    CHECK_EQ(r.value("/sys/files/total"), "262144");
    CHECK_EQ(at(r.schema("/sys/files/upload").body, "maxSize"), "262144");  // refused before it is sent
    CHECK_EQ(r.value("/sys/files/dir"), "\"/\"");
    CHECK_EQ(r.value("/sys/files/entries"), "[]");
    CHECK_EQ(r.get("/sys/files", tesser::Query(), false).status, Status::Unauthorized);
}

TEST(files_unmounted) {
    FilesRig r;
    r.fake.fs.present = false;
    CHECK(r.dev.begin());
    CHECK(!r.files.mounted());
    CHECK_EQ(r.value("/sys/files/mounted"), "false");
    CHECK_EQ(r.set("/sys/files/mkdir", "\"logs\"").status, Status::NotAllowed);
    CHECK_EQ(r.uploadNamed("/sys/files/upload", "a.txt", "hello").status, Status::NotAllowed);
    CHECK_EQ(r.set("/sys/backup/save", "\"x\"").status, Status::NotAllowed);
}

TEST(files_upload_list_download) {
    FilesRig r;
    CHECK(r.dev.begin());
    std::string data(5000, 'x');
    CHECK_EQ(r.uploadNamed("/sys/files/upload", "notes.txt", data).status, Status::Ok);
    CHECK(r.onDisk("notes.txt"));
    CHECK(!r.onDisk(".notes.txt"));  // the temporary file was renamed
    r.step();
    CHECK_EQ(r.names("/sys/files/entries"), "notes.txt");
    CHECK_EQ(r.value("/sys/files/entries/notes.txt/size"), "5000");
    CHECK_EQ(r.value("/sys/files/entries/notes.txt/isDir"), "false");
    CHECK_EQ(r.value("/sys/files/used"), "5000");
    std::string name;
    rig::Reply d = r.download("/sys/files/entries/notes.txt/content", true, &name);
    CHECK_EQ(d.status, Status::Ok);
    CHECK_EQ(d.body, data);
    CHECK_EQ(name, "notes.txt");
    // Replaced through the entry, or by an upload of the same name.
    CHECK_EQ(r.upload("/sys/files/entries/notes.txt/content", "short").status, Status::Ok);
    CHECK_EQ(r.download("/sys/files/entries/notes.txt/content").body, "short");
    CHECK_EQ(r.uploadNamed("/sys/files/upload", "notes.txt", "again").status, Status::Ok);
    CHECK_EQ(r.download("/sys/files/entries/notes.txt/content").body, "again");
    CHECK_EQ(r.download("/sys/files/entries/gone.txt/content").status, Status::NotFound);
    CHECK_EQ(r.download("/sys/files/entries/notes.txt/content", false).status, Status::Unauthorized);
}

TEST(files_refuses_names) {
    FilesRig r;
    CHECK(r.dev.begin());
    for (const char* bad : {"", ".hidden", "..", "a/b", "../escape", "tab\there"}) {
        CHECK_EQ(r.uploadNamed("/sys/files/upload", bad, "x").status, Status::InvalidValue);
    }
    CHECK_EQ(r.uploadNamed("/sys/files/upload", std::string(33, 'n'), "x").status, Status::InvalidValue);
    CHECK_EQ(r.uploadNamed("/sys/files/upload", std::string(32, 'n'), "x").status, Status::Ok);
    CHECK_EQ(r.uploadNamed("/sys/files/upload", "été.txt", "x").status, Status::Ok);  // UTF-8 is fine
    CHECK_EQ(r.set("/sys/files/dir", "\"/../..\"").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/files/mkdir", "\"a/b\"").status, Status::InvalidValue);
    CHECK_EQ(r.files.path("../etc/passwd"), "");
    CHECK_EQ(r.files.path("/a//b/"), r.fake.fs.dir + "/a/b");
}

TEST(files_space) {
    FilesRig r;
    r.fake.fs.total = 4096;
    CHECK(r.dev.begin());
    CHECK_EQ(r.uploadNamed("/sys/files/upload", "big.bin", std::string(5000, 'b')).status, Status::TooLarge);
    CHECK(!r.onDisk("big.bin"));
    // An upload cut short leaves nothing behind.
    rig::Reply u = r.upload("/sys/files/upload", std::string(3000, 'c'), true, 1000, 2000, "cut.bin");
    CHECK(!u.done);
    CHECK(!r.onDisk("cut.bin"));
    CHECK(!r.onDisk(".cut.bin"));
}

TEST(files_directories) {
    FilesRig r;
    CHECK(r.dev.begin());
    CHECK_EQ(r.set("/sys/files/mkdir", "\"logs\"").status, Status::Ok);
    CHECK_EQ(r.uploadNamed("/sys/files/upload", "top.txt", "t").status, Status::Ok);
    r.step();
    CHECK_EQ(r.names("/sys/files/entries"), "logs,top.txt");  // directories first
    CHECK_EQ(r.value("/sys/files/entries/logs/isDir"), "true");
    CHECK_EQ(r.download("/sys/files/entries/logs/content").status, Status::NotAllowed);
    CHECK_EQ(r.set("/sys/files/entries/top.txt/open").status, Status::NotAllowed);
    CHECK_EQ(r.set("/sys/files/entries/logs/open").status, Status::Ok);
    CHECK_EQ(r.value("/sys/files/dir"), "\"/logs\"");
    CHECK_EQ(r.value("/sys/files/entries"), "[]");
    CHECK_EQ(r.uploadNamed("/sys/files/upload", "day1.log", "one").status, Status::Ok);
    r.step();
    CHECK(r.onDisk("logs/day1.log"));
    CHECK_EQ(r.names("/sys/files/entries"), "day1.log");
    CHECK_EQ(r.set("/sys/files/up").status, Status::Ok);
    CHECK_EQ(r.value("/sys/files/dir"), "\"/\"");
    CHECK_EQ(r.set("/sys/files/dir", "\"/logs/\"").status, Status::Ok);
    CHECK_EQ(r.value("/sys/files/dir"), "\"/logs\"");
    CHECK_EQ(r.set("/sys/files/dir", "\"/nowhere\"").status, Status::NotFound);
    r.set("/sys/files/up");
    // Deleting a directory deletes what it holds.
    CHECK_EQ(r.set("/sys/files/entries/logs/delete").status, Status::Ok);
    CHECK(!r.onDisk("logs"));
    CHECK_EQ(r.names("/sys/files/entries"), "top.txt");
}

TEST(files_rename) {
    FilesRig r;
    CHECK(r.dev.begin());
    r.uploadNamed("/sys/files/upload", "a.txt", "A");
    r.uploadNamed("/sys/files/upload", "b.txt", "B");
    r.step();
    CHECK_EQ(r.set("/sys/files/entries/a.txt/rename", "\"b.txt\"").status, Status::InvalidValue);  // taken
    CHECK_EQ(r.set("/sys/files/entries/a.txt/rename", "\"../c.txt\"").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/files/entries/a.txt/rename", "\"c.txt\"").status, Status::Ok);
    CHECK_EQ(r.names("/sys/files/entries"), "b.txt,c.txt");
    CHECK_EQ(r.download("/sys/files/entries/c.txt/content").body, "A");
}

TEST(files_application_writes) {
    FilesRig r;
    CHECK(r.dev.begin());
    // Through the helpers: listed at once.
    CHECK(r.files.write("data/cal.json", "{\"k\":1}").isOk());
    r.step();
    CHECK_EQ(r.names("/sys/files/entries"), "data");
    std::string text;
    CHECK(r.files.read("data/cal.json", text, 100).isOk());
    CHECK_EQ(text, "{\"k\":1}");
    CHECK_EQ(r.files.read("data/cal.json", text, 3).status, Status::TooLarge);
    // Through stdio: listed after changed().
    FILE* f = fopen(r.files.path("own.txt").c_str(), "w");
    CHECK(f != nullptr);
    fputs("mine", f);
    fclose(f);
    r.step();
    CHECK_EQ(r.names("/sys/files/entries"), "data");
    r.files.changed();
    r.step();
    CHECK_EQ(r.names("/sys/files/entries"), "data,own.txt");
}

TEST(files_format) {
    FilesRig r;
    CHECK(r.dev.begin());
    r.set("/sys/files/mkdir", "\"logs\"");
    r.set("/sys/files/dir", "\"/logs\"");
    r.uploadNamed("/sys/files/upload", "x.log", "x");
    CHECK_EQ(r.set("/sys/files/format").status, Status::Ok);
    CHECK_EQ(r.fake.fs.formats, 1);
    CHECK(!r.onDisk("logs"));
    CHECK_EQ(r.value("/sys/files/dir"), "\"/\"");
    CHECK_EQ(r.value("/sys/files/entries"), "[]");
}

TEST(backup_store_and_restore) {
    FilesRig r;
    CHECK(r.dev.begin());
    CHECK_EQ(r.set("/sys/backup/restartAfter", "false").status, Status::Ok);
    r.set("/app/level", "5");  // backups hold what differs from the defaults
    rig::Reply s = r.set("/sys/backup/save", "\"first\"");
    CHECK_EQ(s.status, Status::Ok);
    CHECK_EQ(s.body, "\"first\"");
    CHECK(r.onDisk("backups/first.json"));
    CHECK_EQ(r.names("/sys/backup/stored"), "first");
    r.set("/app/level", "8");
    std::string name;
    rig::Reply d = r.download("/sys/backup/stored/first/content", true, &name);
    CHECK_EQ(at(d.body, "app/level"), "5");
    CHECK_EQ(name, "test-device-a67c-first.json");
    rig::Reply back = r.set("/sys/backup/stored/first/restore");
    CHECK_EQ(back.status, Status::Ok);
    CHECK_EQ(back.body, "{\"skipped\":[],\"restart\":false}");
    CHECK_EQ(r.level, 5);
    // What it replaced was stored first.
    std::string before;
    CHECK(r.files.read("backups/before-restore.json", before, 16384).isOk());
    CHECK_EQ(at(before, "app/level"), "8");
    r.step();
    CHECK(r.names("/sys/backup/stored").find("before-restore") != std::string::npos);
    CHECK_EQ(r.set("/sys/backup/stored/first/delete").status, Status::Ok);
    CHECK(!r.onDisk("backups/first.json"));
    CHECK_EQ(r.set("/sys/backup/stored/first/restore").status, Status::NotFound);
    CHECK_EQ(r.as(tesser::Access::User, tesser::Op::Set, "/sys/backup/save", "\"x\"").status, Status::Unauthorized);
}

TEST(backup_stored_waits_for_memory) {
    // Storing builds the settings' text, restoring reads the file whole:
    // both from TesserAPI's byte budget, busy when it refuses.
    FilesRig r;
    CHECK(r.dev.begin());
    CHECK_EQ(r.set("/sys/backup/restartAfter", "false").status, Status::Ok);
    r.set("/app/level", "7");
    CHECK_EQ(r.set("/sys/backup/save", "\"kept\"").status, Status::Ok);
    r.set("/app/level", "1");
    std::string kept;
    CHECK(r.files.read("backups/kept.json", kept, 16384).isOk());
    {
        rig::BudgetLimit low(kept.size() / 2);  // less than the text, or the file
        rig::Reply s = r.set("/sys/backup/save", "\"other\"");
        CHECK_EQ(s.status, Status::Busy);
        CHECK(s.body.find("memory") != std::string::npos);
        CHECK(!r.onDisk("backups/other.json"));
        CHECK_EQ(r.set("/sys/backup/stored/kept/restore").status, Status::Busy);
        CHECK_EQ(r.level, 1);
    }
    CHECK_EQ(r.set("/sys/backup/stored/kept/restore").status, Status::Ok);
    CHECK_EQ(r.level, 7);
}

TEST(backup_restore_upload_keeps_previous) {
    FilesRig r;
    CHECK(r.dev.begin());
    r.set("/app/level", "4");
    CHECK_EQ(r.upload("/sys/backup/settings", "{\"app\":{\"level\":9}}").status, Status::Ok);
    CHECK_EQ(r.level, 9);
    std::string before;
    CHECK(r.files.read("backups/before-restore.json", before, 16384).isOk());
    CHECK_EQ(at(before, "app/level"), "4");
}

TEST(backup_default_names) {
    FilesRig r;
    CHECK(r.dev.begin());
    CHECK_EQ(r.set("/sys/backup/save", "\"\"").body, "\"backup-001\"");  // no time yet
    CHECK_EQ(r.set("/sys/backup/save", "\"\"").body, "\"backup-002\"");
    r.fake.clock.epoch = 1791800000000LL;  // 2026-10-12, local time UTC
    std::string named = r.set("/sys/backup/save", "\"\"").body;
    CHECK_EQ(named.size(), 17u);  // "YYYY-MM-DD-HHMM"
    CHECK_EQ(named.substr(0, 4), "\"202");
    std::string again = r.set("/sys/backup/save", "\"\"").body;
    CHECK_EQ(again, named.substr(0, 16) + "-2\"");
    CHECK_EQ(r.set("/sys/backup/save", "\"" + std::string(28, 'n') + "\"").status, Status::InvalidValue);
}

TEST(backup_keep) {
    FilesRig r;
    r.backup.keep(2);
    CHECK(r.dev.begin());
    r.set("/sys/backup/save", "\"a\"");
    r.set("/sys/backup/save", "\"b\"");
    r.set("/sys/backup/save", "\"c\"");  // the oldest goes (same second here: by name)
    CHECK(!r.onDisk("backups/a.json"));
    CHECK(r.onDisk("backups/b.json"));
    CHECK(r.onDisk("backups/c.json"));
    r.upload("/sys/backup/settings", "{\"app\":{\"level\":4}}");  // before-restore isn't counted
    CHECK(r.onDisk("backups/b.json"));
    CHECK(r.onDisk("backups/before-restore.json"));
}

TEST(backup_factory_reset_removes_stored) {
    FilesRig r;
    CHECK(r.dev.begin());
    r.set("/sys/backup/save", "\"keepme\"");
    r.uploadNamed("/sys/files/upload", "other.txt", "o");
    CHECK_EQ(r.set("/sys/factoryReset").status, Status::Ok);
    CHECK(!r.onDisk("backups"));  // they hold secrets
    CHECK(r.onDisk("other.txt"));
}

TEST(backup_without_files) {
    rig::Rig r;
    tesserkit::Backup backup{r.dev};
    CHECK(r.dev.begin());
    CHECK_EQ(r.get("/sys/backup/save").status, Status::NotFound);
    CHECK_EQ(r.get("/sys/backup/stored").status, Status::NotFound);
}

}  // namespace files_cases
