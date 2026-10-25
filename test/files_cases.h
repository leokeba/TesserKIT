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
    // The names in a list value, or in a listing's entries.
    static std::string namesIn(const std::string& json) {
        std::string out;
        for (size_t i = 0;; i++) {
            std::string n = at(json, (std::to_string(i) + "/name").c_str());
            if (n == "<missing>") break;
            out += (out.empty() ? "" : ",") + n.substr(1, n.size() - 2);
        }
        return out;
    }
    std::string names(const char* list) { return namesIn(value(list)); }
    // /sys/files/list of `dir`: the reply.
    rig::Reply ls(const std::string& dir) { return set("/sys/files/list", "\"" + dir + "\""); }
    std::string lsNames(const std::string& dir) { return namesIn(at(ls(dir).body, "entries")); }
    rig::Reply put(const std::string& path, const std::string& data) { return uploadNamed("/sys/files/file", path, data); }
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
    CHECK_EQ(at(r.schema("/sys/files").body, "ui/widget"), "\"files\"");
    CHECK_EQ(at(r.schema("/sys/files/file").body, "maxSize"), "262144");  // refused before it is sent
    CHECK_EQ(r.ls("/").body, "{\"path\":\"/\",\"entries\":[],\"truncated\":false}");
    CHECK_EQ(r.get("/sys/files", tesser::Query(), false).status, Status::Unauthorized);
}

TEST(files_unmounted) {
    FilesRig r;
    r.fake.fs.present = false;
    CHECK(r.dev.begin());
    CHECK(!r.files.mounted());
    CHECK_EQ(r.value("/sys/files/mounted"), "false");
    CHECK_EQ(r.ls("/").status, Status::NotAllowed);
    CHECK_EQ(r.set("/sys/files/mkdir", "\"/logs\"").status, Status::NotAllowed);
    CHECK_EQ(r.put("/a.txt", "hello").status, Status::NotAllowed);
    CHECK_EQ(r.set("/sys/backup/save", "\"x\"").status, Status::NotAllowed);
}

// Stateless: every transfer and action names its path, so clients browse
// on their own.
TEST(files_upload_and_list_by_path) {
    FilesRig r;
    CHECK(r.dev.begin());
    std::string data(5000, 'x');
    CHECK_EQ(r.put("/notes.txt", data).status, Status::Ok);
    CHECK(r.onDisk("notes.txt"));
    CHECK(!r.onDisk(".notes.txt.part"));  // the temporary file was renamed
    rig::Reply l = r.ls("/");
    CHECK_EQ(l.status, Status::Ok);
    CHECK_EQ(at(l.body, "path"), "\"/\"");
    CHECK_EQ(at(l.body, "entries/0/name"), "\"notes.txt\"");
    CHECK_EQ(at(l.body, "entries/0/size"), "5000");
    CHECK_EQ(at(l.body, "entries/0/dir"), "false");
    CHECK_EQ(at(l.body, "truncated"), "false");
    r.step();
    CHECK_EQ(r.value("/sys/files/used"), "5000");
    // Replaced by an upload to the same path; "notes.txt" is the same path.
    CHECK_EQ(r.put("notes.txt", "short").status, Status::Ok);
    std::string text;
    CHECK(r.files.read("notes.txt", text, 100).isOk());
    CHECK_EQ(text, "short");
    // Without a path, nothing.
    CHECK_EQ(r.upload("/sys/files/file", "x").status, Status::InvalidValue);
    CHECK_EQ(r.download("/sys/files/file").status, Status::InvalidValue);
    CHECK_EQ(r.ls("/notes.txt").status, Status::NotFound);  // not a directory
    CHECK_EQ(r.ls("/nowhere").status, Status::NotFound);
    CHECK_EQ(r.as(tesser::Access::User, tesser::Op::Set, "/sys/files/list", "\"/\"").status, Status::Unauthorized);
}

TEST(files_refuses_names) {
    FilesRig r;
    CHECK(r.dev.begin());
    for (const char* bad : {"/", "/.hidden", "/..", "/../escape", "/tab\there", "/.settings/sys"}) {
        CHECK_EQ(r.put(bad, "x").status, Status::InvalidValue);
    }
    CHECK_EQ(r.put("/" + std::string(33, 'n'), "x").status, Status::InvalidValue);
    CHECK_EQ(r.put("/" + std::string(32, 'n'), "x").status, Status::Ok);
    CHECK_EQ(r.put("/été.txt", "x").status, Status::Ok);  // UTF-8 is fine
    CHECK_EQ(r.put("/nowhere/a.txt", "x").status, Status::NotFound);  // folders aren't made by uploads
    CHECK_EQ(r.ls("/../..").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/files/mkdir", "\"/.hidden\"").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/files/delete", "\"/\"").status, Status::InvalidValue);  // not the root
    CHECK_EQ(r.files.path("../etc/passwd"), "");
    CHECK_EQ(r.files.path("/a//b/"), r.fake.fs.dir + "/a/b");
}

TEST(files_space) {
    FilesRig r;
    r.fake.fs.total = 4096;
    CHECK(r.dev.begin());
    CHECK_EQ(r.put("/big.bin", std::string(5000, 'b')).status, Status::TooLarge);
    CHECK(!r.onDisk("big.bin"));
    // An upload cut short leaves nothing behind.
    rig::Reply u = r.upload("/sys/files/file", std::string(3000, 'c'), true, 1000, 2000, "/cut.bin");
    CHECK(!u.done);
    CHECK(!r.onDisk("cut.bin"));
    CHECK(!r.onDisk(".cut.bin.part"));
}

TEST(files_folders) {
    FilesRig r;
    CHECK(r.dev.begin());
    CHECK_EQ(r.set("/sys/files/mkdir", "\"/logs\"").status, Status::Ok);
    CHECK_EQ(r.set("/sys/files/mkdir", "\"/logs\"").status, Status::InvalidValue);  // taken
    CHECK_EQ(r.set("/sys/files/mkdir", "\"/www/img\"").status, Status::Ok);        // with the folders on the way
    CHECK_EQ(r.put("/top.txt", "t").status, Status::Ok);
    CHECK_EQ(r.set("/sys/files/mkdir", "\"/top.txt\"").status, Status::InvalidValue);
    CHECK_EQ(r.lsNames("/"), "logs,www,top.txt");  // folders first
    CHECK_EQ(at(r.ls("/").body, "entries/0/dir"), "true");
    CHECK_EQ(r.put("/logs/day1.log", "one").status, Status::Ok);
    CHECK(r.onDisk("logs/day1.log"));
    CHECK_EQ(r.lsNames("/logs/"), "day1.log");  // a trailing slash is the same folder
    CHECK_EQ(at(r.ls("/logs/").body, "path"), "\"/logs\"");
    CHECK_EQ(r.lsNames("/www"), "img");
    CHECK_EQ(r.upload("/sys/files/file", "x", true, 2048, SIZE_MAX, "/logs").status, Status::InvalidValue);
    // Deleting a folder deletes what it holds.
    CHECK_EQ(r.set("/sys/files/delete", "\"/logs\"").status, Status::Ok);
    CHECK(!r.onDisk("logs"));
    CHECK_EQ(r.set("/sys/files/delete", "\"/logs\"").status, Status::NotFound);
    CHECK_EQ(r.lsNames("/"), "www,top.txt");
}

TEST(files_move) {
    FilesRig r;
    CHECK(r.dev.begin());
    r.put("/a.txt", "A");
    r.put("/b.txt", "B");
    r.set("/sys/files/mkdir", "\"/www\"");
    auto move = [&r](const char* from, const char* to) {
        return r.set("/sys/files/move", std::string("{\"from\":\"") + from + "\",\"to\":\"" + to + "\"}").status;
    };
    CHECK_EQ(move("/a.txt", "/b.txt"), Status::InvalidValue);  // taken
    CHECK_EQ(move("/a.txt", "/../c.txt"), Status::InvalidValue);
    CHECK_EQ(move("/gone.txt", "/c.txt"), Status::NotFound);
    CHECK_EQ(move("/a.txt", "/nowhere/a.txt"), Status::NotFound);
    CHECK_EQ(move("/www", "/www/inner"), Status::InvalidValue);  // into itself
    CHECK_EQ(move("/a.txt", "/c.txt"), Status::Ok);              // renamed
    CHECK_EQ(move("/c.txt", "/www/c.txt"), Status::Ok);          // moved
    CHECK_EQ(r.lsNames("/"), "www,b.txt");
    CHECK_EQ(r.lsNames("/www"), "c.txt");
    CHECK_EQ(move("/www", "/site"), Status::Ok);  // a folder, with what it holds
    CHECK(r.onDisk("site/c.txt"));
}

// Each change names the folder whose entries changed, so open browsers
// list it again.
TEST(files_changed_events) {
    FilesRig r;
    CHECK(r.dev.begin());
    rig::Events ev(r, "/sys/files/changed");
    auto took = [&ev] {
        std::string out;
        for (const std::string& e : ev.take()) out += (out.empty() ? "" : " ") + e;
        return out;
    };
    r.put("/a.txt", "A");
    r.step();
    CHECK_EQ(took(), "\"/\"");
    r.set("/sys/files/mkdir", "\"/www/img\"");
    r.put("/www/img/logo.svg", "<svg/>");
    r.step();
    CHECK_EQ(took(), "\"/\" \"/www/img\"");  // www, made in /, then logo.svg
    r.set("/sys/files/move", "{\"from\":\"/a.txt\",\"to\":\"/www/a.txt\"}");
    r.step();
    CHECK_EQ(took(), "\"/\" \"/www\"");
    r.set("/sys/files/delete", "\"/www/img\"");
    r.step();
    CHECK_EQ(took(), "\"/www\"");
    // The application's own writes: which folder isn't known.
    r.files.changed();
    r.step();
    CHECK_EQ(took(), "\"\"");
    r.step();
    CHECK_EQ(took(), "");
}

TEST(files_application_writes) {
    FilesRig r;
    CHECK(r.dev.begin());
    CHECK(r.files.write("data/cal.json", "{\"k\":1}").isOk());
    CHECK_EQ(r.lsNames("/"), "data");
    std::string text;
    CHECK(r.files.read("data/cal.json", text, 100).isOk());
    CHECK_EQ(text, "{\"k\":1}");
    CHECK_EQ(r.files.read("data/cal.json", text, 3).status, Status::TooLarge);
    // Through stdio: listed at once, since a listing reads the folder.
    FILE* f = fopen(r.files.path("own.txt").c_str(), "w");
    CHECK(f != nullptr);
    fputs("mine", f);
    fclose(f);
    CHECK_EQ(r.lsNames("/"), "data,own.txt");
}

TEST(files_format) {
    FilesRig r;
    CHECK(r.dev.begin());
    r.set("/sys/files/mkdir", "\"/logs\"");
    r.put("/logs/x.log", "x");
    CHECK_EQ(r.set("/sys/files/format").status, Status::Ok);
    CHECK_EQ(r.fake.fs.formats, 1);
    CHECK(!r.onDisk("logs"));
    CHECK_EQ(r.lsNames("/"), "");
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
    r.put("/other.txt", "o");
    CHECK_EQ(r.set("/sys/factoryReset").status, Status::Ok);
    r.step();
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
