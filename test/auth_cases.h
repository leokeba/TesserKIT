// Auth (docs/DESIGN.md section 5.7) and its crypto.
#pragma once

#include "rig.h"
#include "tesserkit/crypto.h"

namespace auth_cases {

using rig::at;
using rig::Op;
using rig::Status;
using tesser::Access;
namespace crypto = tesserkit::crypto;

std::string hex(const uint8_t* d, size_t n) { return crypto::toHex(d, n); }

TEST(crypto_sha256_vectors) {
    uint8_t h[32];
    crypto::sha256("abc", 3, h);
    CHECK_EQ(hex(h, 32), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    crypto::sha256("", 0, h);
    CHECK_EQ(hex(h, 32), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const char* two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    crypto::sha256(two, strlen(two), h);
    CHECK_EQ(hex(h, 32), "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    std::string million(1000000, 'a');
    crypto::Sha256 s;
    for (size_t i = 0; i < million.size(); i += 777) s.update(million.data() + i, std::min<size_t>(777, million.size() - i));
    s.finish(h);
    CHECK_EQ(hex(h, 32), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(crypto_hmac_and_pbkdf2_vectors) {
    uint8_t h[32];
    uint8_t key[20];
    memset(key, 0x0b, sizeof(key));
    crypto::hmacSha256(key, sizeof(key), "Hi There", 8, h);  // RFC 4231, case 1
    CHECK_EQ(hex(h, 32), "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    crypto::hmacSha256("Jefe", 4, "what do ya want for nothing?", 28, h);  // case 2
    CHECK_EQ(hex(h, 32), "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    uint8_t big[131];
    memset(big, 0xaa, sizeof(big));  // case 6: a key longer than a block
    const char* m = "Test Using Larger Than Block-Size Key - Hash Key First";
    crypto::hmacSha256(big, sizeof(big), m, strlen(m), h);
    CHECK_EQ(hex(h, 32), "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
    const uint8_t salt[] = {'s', 'a', 'l', 't'};
    crypto::pbkdf2Sha256("password", salt, 4, 1, h, 32);
    CHECK_EQ(hex(h, 32), "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    crypto::pbkdf2Sha256("password", salt, 4, 4096, h, 32);
    CHECK_EQ(hex(h, 32), "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");
    uint8_t long40[40];
    const uint8_t salt2[] = "saltSALTsaltSALTsaltSALTsaltSALTsalt";
    crypto::pbkdf2Sha256("passwordPASSWORDpassword", salt2, 36, 4096, long40, 40);
    CHECK_EQ(hex(long40, 40), "348c89dbcbd32b2f32d814b8116e84cf2b17347ebc1800181c4e2a1fb8dd53e1c635518c7dac47e9");
    uint8_t back[3];
    CHECK(crypto::fromHex("00ff7A", back, 3));
    CHECK_EQ(hex(back, 3), "00ff7a");
    CHECK(!crypto::fromHex("00ff7", back, 3));
    CHECK(!crypto::fromHex("00fg7a", back, 3));
    CHECK(crypto::equal("abc", "abc"));
    CHECK(!crypto::equal("abc", "abd"));
    CHECK(!crypto::equal("abc", "ab"));
}

struct AuthRig : rig::Rig {
    tesserkit::System system{dev};
    tesserkit::Time time{dev};
    tesserkit::Auth auth{dev};
    std::string login(const char* name, const char* password, Status* status = nullptr) {
        std::string body = std::string("{\"name\":\"") + name + "\",\"password\":\"" + password + "\"}";
        rig::Reply r = request(anon, Op::Set, "/sys/auth/login", body);
        if (status) *status = r.status;
        return r.status == Status::Ok ? at(r.body, "token").substr(1, at(r.body, "token").size() - 2) : "";
    }
};

TEST(auth_open_until_an_admin_exists) {
    AuthRig r;
    CHECK(r.dev.begin());
    CHECK(r.auth.open());
    CHECK_EQ(r.value("/sys/auth/open"), "true");
    CHECK_EQ(r.dev.api().tokenAccess(""), Access::Admin);
    // A user account doesn't close it.
    CHECK_EQ(r.set("/sys/auth/users", "[{\"name\":\"bob\",\"password\":\"bobpass\"}]").status, Status::Ok);
    CHECK(r.auth.open());
    CHECK_EQ(r.set("/sys/auth/users/bob/role", "\"admin\"").status, Status::Ok);
    CHECK(!r.auth.open());
    CHECK_EQ(r.dev.api().tokenAccess(""), Access::Public);
    r.step();
    CHECK_EQ(r.value("/sys/auth/open"), "false");
}

TEST(auth_passwords_are_hashed) {
    AuthRig r;
    CHECK(r.dev.begin());
    r.set("/sys/auth/users", "[{\"name\":\"ann\",\"password\":\"s3cret\",\"role\":\"admin\"}]");
    CHECK_EQ(r.value("/sys/auth/users/ann/password"), "null");  // never readable
    r.run(2500);
    const std::string& saved = r.storage.records["sys"];
    CHECK(saved.find("s3cret") == std::string::npos);
    CHECK(saved.find("$pbkdf2-sha256$2000$") != std::string::npos);
    CHECK_EQ(r.set("/sys/auth/users/ann/password", "\"\"").status, Status::InvalidValue);
}

TEST(auth_login_and_tokens) {
    AuthRig r;
    r.fake.clock.epoch = 1782907200000;  // the clock is set
    r.auth.defaultAdmin("admin", "hunter22");
    CHECK(r.dev.begin());
    CHECK(!r.auth.open());
    r.set("/sys/auth/users", "[{\"name\":\"admin\"},{\"name\":\"guest\",\"password\":\"guestpw\"}]");

    Status s;
    std::string admin = r.login("admin", "hunter22", &s);
    CHECK_EQ(s, Status::Ok);
    CHECK(admin.find("admin.1783511999") == std::string::npos);  // expiry: now + 168 h
    CHECK(admin.compare(0, 17, "admin.1783512000.") == 0);
    CHECK_EQ(r.dev.api().tokenAccess(admin), Access::Admin);
    std::string guest = r.login("guest", "guestpw");
    CHECK_EQ(r.dev.api().tokenAccess(guest), Access::User);

    // What each level may do.
    CHECK_EQ(r.asToken("", Op::Set, "/sys/restart").status, Status::Unauthorized);
    CHECK_EQ(r.asToken(guest, Op::Set, "/sys/restart").status, Status::Unauthorized);
    CHECK_EQ(r.asToken(admin, Op::Set, "/sys/restart").status, Status::Ok);
    CHECK_EQ(r.asToken(guest, Op::Get, "/sys/auth/users").status, Status::Unauthorized);
    CHECK_EQ(at(r.asToken("", Op::Get, "/sys/auth").body, "users"), "<missing>");
    CHECK_EQ(r.asToken("", Op::Get, "/sys/status/heap").status, Status::Ok);

    // Tampered or unknown tokens.
    std::string forged = guest;
    forged.replace(0, 5, "admin");
    CHECK_EQ(r.dev.api().tokenAccess(forged), Access::Public);
    CHECK_EQ(r.dev.api().tokenAccess("garbage"), Access::Public);
    CHECK_EQ(r.dev.api().tokenAccess("a.b.c"), Access::Public);

    // Expiry.
    r.fake.clock.epoch += 169LL * 3600 * 1000;
    CHECK_EQ(r.dev.api().tokenAccess(admin), Access::Public);
    r.fake.clock.epoch -= 169LL * 3600 * 1000;

    // A removed account's tokens stop working.
    r.set("/sys/auth/users", "[{\"name\":\"admin\"}]");
    CHECK_EQ(r.dev.api().tokenAccess(guest), Access::Public);
    // logoutAll revokes every token.
    CHECK_EQ(r.dev.api().tokenAccess(admin), Access::Admin);
    CHECK_EQ(r.set("/sys/auth/logoutAll").status, Status::Ok);
    CHECK_EQ(r.dev.api().tokenAccess(admin), Access::Public);
}

TEST(auth_tokens_survive_restarts) {
    std::map<std::string, std::string> saved;
    std::string token;
    {
        AuthRig r;
        r.auth.defaultAdmin("admin", "pw");
        CHECK(r.dev.begin());
        token = r.login("admin", "pw");
        r.set("/sys/auth/users/admin/password", "\"changed\"");
        r.run(2500);
        saved = r.storage.records;
    }
    // The same firmware: the same default account, whose changed password
    // was stored.
    AuthRig r;
    r.auth.defaultAdmin("admin", "pw");
    r.storage.records = saved;
    CHECK(r.dev.begin());
    CHECK_EQ(r.dev.api().tokenAccess(token), Access::Admin);
    CHECK(!r.auth.open());
    CHECK(r.login("admin", "pw").empty());  // the stored account, not the default
    CHECK_EQ(r.login("admin", "changed").empty(), false);
}

TEST(auth_brute_force_delay) {
    AuthRig r;
    r.auth.defaultAdmin("admin", "right");
    CHECK(r.dev.begin());
    Status s;
    for (int i = 0; i < 5; i++) {
        r.login("admin", "wrong", &s);
        CHECK_EQ(s, Status::Unauthorized);
    }
    r.login("admin", "right", &s);
    CHECK_EQ(s, Status::Busy);
    r.run(29000);
    r.login("admin", "right", &s);
    CHECK_EQ(s, Status::Busy);
    r.run(1100);
    r.login("admin", "right", &s);
    CHECK_EQ(s, Status::Ok);
    r.login("nobody", "x", &s);
    CHECK_EQ(s, Status::Unauthorized);
}

TEST(auth_whoami_and_schema) {
    AuthRig r;
    r.auth.defaultAdmin("admin", "pw");
    CHECK(r.dev.begin());
    rig::Reply w = r.request(r.anon, Op::Set, "/sys/auth/whoami");
    CHECK_EQ(w.body, "{\"access\":\"public\"}");
    w = r.request(r.admin, Op::Set, "/sys/auth/whoami");
    CHECK_EQ(w.body, "{\"access\":\"admin\"}");
    rig::Reply s = r.schema("/sys/auth");
    CHECK_EQ(at(s.body, "children/login/ui/auth"), "\"login\"");
    CHECK_EQ(at(s.body, "children/login/arg"), "\"object\"");
    CHECK_EQ(at(s.body, "children/login/params/children/password/secret"), "true");
    CHECK_EQ(at(s.body, "children/whoami/ui/auth"), "\"whoami\"");
    CHECK_EQ(at(s.body, "children/users/key"), "\"name\"");
    CHECK_EQ(at(s.body, "children/key/secret"), "true");
    CHECK_EQ(at(s.body, "children/openWarning/ui/style"), "\"warning\"");
    CHECK_EQ(at(s.body, "children/openWarning/ui/visibleWhen"), "\"open=true\"");
}

TEST(auth_backup_restores_hashes) {
    struct R : AuthRig {
        tesserkit::Backup backup{dev};
    };
    std::string saved;
    {
        R r;
        CHECK(r.dev.begin());
        r.set("/sys/auth/users", "[{\"name\":\"admin\",\"password\":\"kept-password\",\"role\":\"admin\"}]");
        saved = r.download("/sys/backup/settings").body;
    }
    R r;
    CHECK(r.dev.begin());
    CHECK(r.auth.open());
    CHECK_EQ(r.upload("/sys/backup/settings", saved).status, Status::Ok);
    CHECK(!r.auth.open());
    Status s;
    r.login("admin", "kept-password", &s);  // the hash was restored, not hashed again
    CHECK_EQ(s, Status::Ok);
}

}  // namespace auth_cases
