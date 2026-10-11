// A device on the fake platform, and request helpers, for the host tests.
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <ArduinoJson.h>

#include "TesserKIT.h"
#include "check.h"
#include "fake_platform.h"

namespace rig {

using tesser::Op;
using tesser::Status;

struct Reply {
    Status status = Status::Internal;
    std::string body;
    bool done = false;
};

// A device whose services the derived rig declares, then begin()s.
struct Rig {
    fake::Platform fake;
    tesser::MemoryStorage storage;
    tesserkit::Device dev{"Test Device"};
    tesser::LocalClient admin{dev.api()};
    tesser::LocalClient anon{dev.api()};

    explicit Rig(bool coredump = true) {
        fake.install(dev, coredump);
        dev.task(false).storage(storage);
        anon.setAuthenticated(false);
    }

    void step(uint32_t ms = 20) {
        fake.core.now += ms;
        dev.loop();
    }
    void run(uint32_t ms) {
        for (uint32_t t = 0; t < ms; t += 20) step(20);
    }
    // Long enough for platform events to be handled (they arrive in the
    // device's next period).
    void settle() { run(100); }

    // Sends a request; `wait` keeps the device running until the reply.
    Reply request(tesser::LocalClient& c, Op op, std::string_view path, std::string_view body = {},
                  const tesser::Query& q = tesser::Query(), uint32_t waitMs = 0) {
        // Shared: a deferred reply may come after this returns.
        auto r = std::make_shared<Reply>();
        c.request(op, path, body,
                  [r](Status s, JsonVariantConst b) {
                      r->status = s;
                      r->body.clear();
                      serializeJson(b, r->body);
                      r->done = true;
                  },
                  q);
        c.process();
        for (uint32_t t = 0; !r->done && t < waitMs; t += 20) {
            step(20);
            c.process();
        }
        return *r;
    }
    // Reads go straight through Api::handle(): no client in between to
    // parse (and limit) the reply.
    Reply get(std::string_view path, tesser::Query q = tesser::Query(), bool asAdmin = true) {
        tesser::Request req;
        req.op = Op::Get;
        req.path = path;
        req.query = q;
        req.client.authenticated = asAdmin;
        tesser::StringReply reply;
        dev.api().handle(req, reply);
        Reply out;
        out.status = reply.status;
        out.body = reply.body;
        out.done = reply.ended;
        return out;
    }
    // A request from a client at `level`, as HTTP gives one presenting a
    // token (Api::tokenAccess()). Not for deferred actions.
    Reply as(tesser::Access level, Op op, std::string_view path, std::string_view body = {}) {
        JsonDocument doc;
        if (!body.empty()) deserializeJson(doc, body.data(), body.size());
        tesser::Request req;
        req.op = op;
        req.path = path;
        req.body = doc.as<JsonVariantConst>();
        req.client.access = level;
        tesser::StringReply reply;
        dev.api().handle(req, reply);
        Reply out;
        out.status = reply.status;
        out.body = reply.body;
        out.done = reply.ended;
        return out;
    }
    Reply asToken(std::string_view token, Op op, std::string_view path, std::string_view body = {}) {
        return as(dev.api().tokenAccess(token), op, path, body);
    }
    Reply schema(std::string_view path) {
        tesser::Query q;
        q.view = tesser::View::Schema;
        return get(path, q);
    }
    Reply set(std::string_view path, std::string_view body = {}) { return request(admin, Op::Set, path, body); }
    std::string value(std::string_view path) { return get(path).body; }

    // A file transfer over the core (HTTP's job on the device).
    Reply upload(std::string_view path, const std::string& data, bool asAdmin = true, size_t chunk = 2048,
                 size_t stopAfter = SIZE_MAX, std::string_view filename = {}) {
        tesser::FileTransfer t(dev.api());
        tesser::Client c;
        c.authenticated = asAdmin;
        tesser::StringReply reply;
        Reply out;
        if (t.openUpload(path, c, data.size(), reply, filename) != tesser::FileTransfer::Open::Ok) {
            out.status = reply.status;
            out.body = reply.body;
            return out;
        }
        for (size_t off = 0; off < data.size() && off < stopAfter; off += chunk) {
            size_t n = data.size() - off < chunk ? data.size() - off : chunk;
            if (!t.write(reinterpret_cast<const uint8_t*>(data.data() + off), n, reply)) {
                out.status = reply.status;
                out.body = reply.body;
                return out;
            }
        }
        if (stopAfter != SIZE_MAX) return out;  // cut short: the transfer is destroyed unfinished
        t.finish(reply);
        out.status = reply.status;
        out.body = reply.body;
        out.done = true;
        return out;
    }
    // An upload under the name the client gives (HTTP's ?filename=).
    Reply uploadNamed(std::string_view path, std::string_view filename, const std::string& data) {
        return upload(path, data, true, 2048, SIZE_MAX, filename);
    }
    Reply download(std::string_view path, bool asAdmin = true, std::string* filename = nullptr) {
        tesser::FileTransfer t(dev.api());
        tesser::Client c;
        c.authenticated = asAdmin;
        tesser::StringReply reply;
        Reply out;
        if (t.openDownload(path, c, reply) != tesser::FileTransfer::Open::Ok) {
            out.status = reply.status;
            out.body = reply.body;
            return out;
        }
        if (filename) *filename = t.filename();
        uint8_t buf[700];
        int n;
        while ((n = t.read(buf, sizeof(buf))) > 0) out.body.append(reinterpret_cast<char*>(buf), static_cast<size_t>(n));
        out.status = n < 0 ? Status::Internal : Status::Ok;
        out.done = true;
        return out;
    }
};

// A client subscribed to an event, as a UI is: the payloads it receives, as
// JSON. Subscribe after Device::begin().
struct Events {
    tesser::LocalClient client;
    std::vector<std::string> got;
    Events(Rig& r, std::string_view path) : client(r.dev.api()) {
        client.onNotification([this](JsonObjectConst envelope) {
            if (std::string(envelope["op"] | "") != "event") return;
            std::string body;
            serializeJson(envelope["body"], body);
            got.push_back(body);
        });
        bool ok = false;
        client.subscribe(path, [&ok](Status s, JsonVariantConst) { ok = s == Status::Ok; });
        client.process();
        CHECK(ok);
    }
    // What arrived since the last call.
    std::vector<std::string> take() {
        client.process();
        std::vector<std::string> out;
        out.swap(got);
        return out;
    }
};

// The JSON at `path` in `json`: keys and array indexes separated by '/'.
inline std::string at(const std::string& json, const char* path) {
    JsonDocument doc;
    if (deserializeJson(doc, json, DeserializationOption::NestingLimit(64))) return "<bad json>";
    JsonVariantConst v = doc.as<JsonVariantConst>();
    std::string p(path);
    size_t start = 0;
    while (start < p.size()) {
        size_t end = p.find('/', start);
        if (end == std::string::npos) end = p.size();
        std::string k = p.substr(start, end - start);
        start = end + 1;
        if (v.is<JsonArrayConst>()) {
            JsonArrayConst a = v.as<JsonArrayConst>();
            size_t i = static_cast<size_t>(atoi(k.c_str()));
            if (i >= a.size()) return "<missing>";
            v = a[i];
            continue;
        }
        if (!v.is<JsonObjectConst>()) return "<missing>";
        bool found = false;
        for (JsonPairConst kv : v.as<JsonObjectConst>()) {
            if (k == kv.key().c_str()) {
                v = kv.value();
                found = true;
                break;
            }
        }
        if (!found) return "<missing>";
    }
    std::string out;
    serializeJson(v, out);
    return out;
}

}  // namespace rig
