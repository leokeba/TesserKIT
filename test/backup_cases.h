// Backup (docs/DESIGN.md section 5.5).
#pragma once

#include "rig.h"

namespace backup_cases {

using rig::at;
using rig::Status;

struct BackupRig : rig::Rig {
    tesserkit::System system{dev};
    tesserkit::Network net{dev};
    tesserkit::Time time{dev};
    tesserkit::Backup backup{dev};
    int level = 3;
    BackupRig() { dev.api().object("app").value("level", level).range(0, 10).persist(); }
};

TEST(backup_download) {
    BackupRig r;
    r.net.defaultNetwork("Home", "hunter22");
    CHECK(r.dev.begin());
    std::string name;
    rig::Reply d = r.download("/sys/backup/settings", true, &name);
    CHECK_EQ(d.status, Status::Ok);
    CHECK_EQ(name, "test-device-a67c-settings.json");
    CHECK_EQ(at(d.body, "app/level"), "3");
    CHECK_EQ(at(d.body, "sys/network/wifi/networks/0/password"), "\"hunter22\"");  // secrets included
    CHECK_EQ(at(d.body, "sys/time/tz"), "\"UTC0\"");
    CHECK_EQ(r.download("/sys/backup/settings", false).status, Status::Unauthorized);
}

TEST(backup_restore) {
    BackupRig r;
    CHECK(r.dev.begin());
    rig::Reply u = r.upload("/sys/backup/settings",
                            "{\"app\":{\"level\":7,\"gone\":1},\"sys\":{\"time\":{\"tz\":\"EST5EDT\"},"
                            "\"network\":{\"wifi\":{\"networks\":[{\"ssid\":\"Cabin\",\"password\":\"woodstove\"}]}}}}");
    CHECK_EQ(u.status, Status::Ok);
    CHECK_EQ(u.body, "{\"skipped\":[\"/app/gone\"],\"restart\":true}");
    CHECK_EQ(r.level, 7);
    CHECK_EQ(r.value("/sys/time/tz"), "\"EST5EDT\"");
    CHECK_EQ(r.storage.records.count("app"), 1u);  // saved at once
    CHECK(r.storage.records["sys"].find("woodstove") != std::string::npos);
    r.run(900);
    CHECK_EQ(r.fake.core.restarts, 0);
    r.run(200);
    CHECK_EQ(r.fake.core.restarts, 1);
}

TEST(backup_restore_without_restart) {
    BackupRig r;
    CHECK(r.dev.begin());
    CHECK_EQ(r.set("/sys/backup/restartAfter", "false").status, Status::Ok);
    rig::Reply u = r.upload("/sys/backup/settings", "{\"app\":{\"level\":9}}");
    CHECK_EQ(u.status, Status::Ok);
    CHECK_EQ(at(u.body, "restart"), "false");
    r.run(3000);
    CHECK_EQ(r.fake.core.restarts, 0);
    CHECK_EQ(r.level, 9);
}

TEST(backup_restore_refuses_garbage) {
    BackupRig r;
    CHECK(r.dev.begin());
    rig::Reply u = r.upload("/sys/backup/settings", "not json");
    CHECK_EQ(u.status, Status::BadRequest);
    CHECK_EQ(r.upload("/sys/backup/settings", "{\"app\":{\"level\":1}}", false).status, Status::Unauthorized);
    std::string huge = "{\"app\":{\"level\":1,\"pad\":\"" + std::string(20000, 'x') + "\"}}";
    CHECK_EQ(r.upload("/sys/backup/settings", huge).status, Status::TooLarge);
    CHECK_EQ(r.level, 3);
    r.run(2000);
    CHECK_EQ(r.fake.core.restarts, 0);
}

TEST(backup_round_trip) {
    std::string saved;
    {
        BackupRig r;
        CHECK(r.dev.begin());
        r.set("/app/level", "5");
        r.set("/sys/network/ap/password", "\"apsecret1\"");
        saved = r.download("/sys/backup/settings").body;
    }
    BackupRig r;
    CHECK(r.dev.begin());
    CHECK_EQ(r.upload("/sys/backup/settings", saved).body, "{\"skipped\":[],\"restart\":true}");
    CHECK_EQ(r.level, 5);
    r.settle();
    CHECK_EQ(r.fake.wifi.ap.password, "apsecret1");  // the AP restarted with it
}

}  // namespace backup_cases
