#pragma once

#include <atomic>
#include <string>
#include <string_view>
#include <vector>

#include "tesserkit/device.h"

namespace tesserkit {

enum class Role : uint8_t { User, Admin };

// An account: an element of /sys/auth/users. `password` holds the salted
// hash ("$pbkdf2-sha256$<iterations>$<salt>$<hash>", hex); writing a
// password stores its hash, writing a hash (a restored backup) stores it.
struct User {
    char name[25] = "";
    std::string password;
    Role role = Role::User;
    // Not described (so not persisted): a new password whose hash is being
    // computed on TesserAPI's background task, which is the account's
    // password meanwhile, and the write `password` or `pending` came from.
    std::string pending;
    uint32_t write = 0;
};

// What /sys/auth/login takes.
struct Credentials {
    std::string name;
    std::string password;
};
void describe(tesser::Object& o, Credentials& c);

// /sys/auth: users, roles, sign-in with stateless signed tokens
// (docs/DESIGN.md section 5.7). Until an admin account exists, the device
// stays open: every client is Admin.
class Auth : public Service {
public:
    explicit Auth(Device& dev) : Service(dev, "auth") {}
    // Waits for its jobs on TesserAPI's background task.
    ~Auth() override;

    // A factory default account (role admin), so a device shipped to others
    // has no open window. Before Device::begin().
    Auth& defaultAdmin(const char* name, const char* password);

    void describe(tesser::Object& o) override;

    // The level a token gives (Api::authenticate()). Thread-safe.
    tesser::Access check(std::string_view token);
    // True while no admin account exists.
    bool open() const;
    // A token for `name`, valid sessionHours (no expiry while the clock
    // isn't set).
    std::string token(const std::string& name);

    static constexpr uint32_t kIterations = 2000;   // PBKDF2 rounds
    static constexpr int kMaxFailures = 5;          // then login is busy...
    static constexpr uint32_t kLockoutMs = 30000;   // ...this long

    // A stored password hash for `password`, with a random salt.
    std::string hash(std::string_view password);
    bool verify(std::string_view password, const std::string& stored) const;
    static bool isHash(std::string_view s);

protected:
    void start() override;
    void tick(uint32_t nowMs) override;

private:
    void login(const Credentials& c, tesser::Call& call);
    void setPassword(User& u, const std::string& v);
    std::string hashWith(std::string_view password, const uint8_t* salt) const;
    std::string sign(std::string_view payload) const;
    void newKey();
    const User* find(std::string_view name) const;
    int64_t nowSeconds() const;

    std::vector<User> users_;
    std::string key_;  // hex, signs tokens
    int32_t sessionHours_ = 168;
    tesser::Node* keyNode_ = nullptr;
    tesser::Node* usersNode_ = nullptr;
    uint32_t writes_ = 0;           // password writes, under the API lock
    std::atomic<int> jobs_{0};      // on the background task
    tesser::Node* openNode_ = nullptr;
    bool wasOpen_ = true;
    int failures_ = 0;
    int checking_ = 0;  // sign-ins on TesserAPI's background task
    uint32_t lockedUntilMs_ = 0;
    const char* defaultName_ = nullptr;
    const char* defaultPassword_ = nullptr;
};

}  // namespace tesserkit
