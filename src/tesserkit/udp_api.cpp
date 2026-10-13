#include "tesserkit/udp_api.h"

#include "tesser/call.h"
#include "tesser/udp_link.h"
#include "tesserkit/peers.h"

namespace tesserkit {

using tesser::Access;
using tesser::Check;
using tesser::Status;

namespace {

constexpr uint32_t kPollMs = 1000;     // the link's counters
constexpr uint32_t kSettleMs = 20000;  // at the latest, without an address: start anyway

bool jsonOk(const std::string& v) {
    if (v.empty()) return true;
    JsonDocument doc;
    return !deserializeJson(doc, v);
}

}  // namespace

void describe(tesser::Object& o, UdpSend& s) {
    o.value("path", s.path).ui("placeholder", "/sequencer/play");
    o.value("value", s.value).doc("JSON: true, 42, \"intro\"; empty for an action without argument").ui(
        "placeholder", "\"intro\"");
    o.value("to", s.to).label("To").doc("A node's address; empty: every node of the local networks at once").ui(
        "placeholder", "every node");
}

UdpApi& UdpApi::enable(bool on) {
    enabled_ = on;
    return *this;
}

UdpApi& UdpApi::port(uint16_t p) {
    port_ = p;
    return *this;
}

UdpApi& UdpApi::peerAccess(Access level) {
    peerAccess_ = static_cast<Level>(level);
    return *this;
}

UdpApi& UdpApi::interfaces(uint8_t mask) {
    interfaces_ = mask & kAllInterfaces ? mask & kAllInterfaces : kAllInterfaces;
    return *this;
}

UdpApi& UdpApi::ownTask(bool own) {
    ownTask_ = own;
    return *this;
}

UdpApi& UdpApi::maxCalls(size_t n) {
    maxCalls_ = n;
    return *this;
}

UdpApi& UdpApi::mountPeers(bool on) {
    mountPeers_ = on;
    return *this;
}

void UdpApi::describe(tesser::Object& o) {
    o.label("UDP")
        .ui("icon", "bolt")
        .doc("The nodes on the local networks, over Ethernet or Wi-Fi: their pages here, writes to all at once")
        .writeAccess(Access::Admin);
    enabledNode_ = &o.value("enabled", enabled_).persist();
    portNode_ = &o.value("port", port_).range(1024, 65535).persist().ui("widget", "number").ui("advanced");
    interfacesNode_ = &o.value("interfaces", interfaces_)
                           .range(1, 7)
                           .persist()
                           .label("Networks")
                           .doc("The interfaces UDP serves and reaches nodes on; nothing goes by the others")
                           .ui("widget", "flags")
                           .ui("labels", "Ethernet,Wi-Fi,Access point")
                           .ui("advanced");
    accessNode_ = &o.value("peerAccess", peerAccess_, {"public", "user", "admin"})
                       .persist()
                       .label("Nodes' level")
                       .doc("What a node on the network may do without signing in: UDP carries no proof of who "
                            "sends it");
    mountNode_ = &o.value("mountPeers", mountPeers_)
                      .persist()
                      .label("Show nodes")
                      .doc("Mounts the TesserAPI nodes found on the networks under /peers, each with its page");
    restartNode_ = &o.value("restartNeeded", [this] { return restart_; }).ui("hidden");
    o.value("restartWarning", "Some changes apply after a restart.")
        .label("Restart needed")
        .ui("style", "warning")
        .ui("visibleWhen", "restartNeeded=true");
    o.action("send",
             [this](const UdpSend& s, tesser::Call& call) {
                 if (s.to.empty()) {
                     Check c = send(s.path, s.value, s.to);
                     if (!c.isOk()) call.fail(c.status, c.message);
                     return;
                 }
                 tesser::Pending reply = call.defer();
                 Check c = send(s.path, s.value, s.to, &reply);
                 if (!c.isOk()) reply.fail(c.status, c.message);
             })
        .label("Send")
        .doc("Writes a value, or calls an action, on every node of the networks at once (one broadcast, "
             "unanswered), or on one node");

    auto& st = o.object("status").label("Status");
    statusNode_ = &st;
    st.value("running", static_cast<const bool&>(running_)).label("Running");
    st.value("sent", static_cast<const uint32_t&>(sent_)).label("Datagrams sent").ui("advanced");
    st.value("received", static_cast<const uint32_t&>(received_)).label("Datagrams received").ui("advanced");
    st.value("retransmits", static_cast<const uint32_t&>(retransmits_)).label("Sent again").ui("advanced");
    st.value("failed", static_cast<const uint32_t&>(failed_))
        .label("Messages failed")
        .doc("Never acknowledged: the node went away")
        .ui("advanced");
}

void UdpApi::start() {
    Platform& p = dev_.platform();
    if (!p.udpApi) p.udpApi = espUdpApi();
    transport_ = p.udpApi;
    if (!transport_) tesser::logError("tesserkit: UdpApi has no UDP transport in this build");
    if (transport_ && !ownTask_) {
        UdpApiTransport* t = transport_;
        dev_.poller([t] { t->poll(); });
    }
    enabledGen_ = enabledNode_->generation();
    portGen_ = portNode_->generation();
    interfacesGen_ = interfacesNode_->generation();
    accessGen_ = accessNode_->generation();
    mountGen_ = mountNode_->generation();
    // Beside NowTP's nodes: a device reached both ways is mounted once
    // (TesserAPI's DESIGN.md section 14.1).
    if (mountPeers_) peersObject_ = &dev_.api().object("peers").label("Nodes nearby").ui("icon", "target");
    startMs_ = static_cast<uint32_t>(dev_.core().uptimeMs());
}

void UdpApi::begin() {
    if (!transport_ || running_) return;
    bool had = transport_->link() != nullptr;
    Device& dev = dev_;
    address_ = dev_.address();
    running_ = transport_->begin(dev_.api(), port_, dev_.hostname(), maxCalls_, ownTask_, [&dev] { dev.wake(); },
                                 interfaces_);
    if (!had && transport_->link()) Peers::watch(dev_, transport_->link()->endpoint());
    if (!running_) {
        tesser::logError("tesserkit: UDP didn't start on port %u", static_cast<unsigned>(port_));
        statusNode_->changed();
        return;
    }
    tesser::UdpLink* link = transport_->link();
    link->endpoint().setPeerAccess(static_cast<Access>(peerAccess_));
    link->advertise();
    if (peersObject_ && !mounted_) {
        mounted_ = true;
        link->mountPeers(*peersObject_);
    }
    statusNode_->changed();
}

void UdpApi::end() {
    if (!running_) return;
    transport_->end();
    running_ = false;
    statusNode_->changed();
}

void UdpApi::refresh() {
    tesser::UdpLink* link = transport_ ? transport_->link() : nullptr;
    if (!link) return;
    tesser::UdpLink::Stats s = link->stats();
    if (s.sent == sent_ && s.received == received_ && s.retransmits == retransmits_ && s.failed == failed_) return;
    sent_ = s.sent;
    received_ = s.received;
    retransmits_ = s.retransmits;
    failed_ = s.failed;
    statusNode_->changed();
}

void UdpApi::tick(uint32_t nowMs) {
    // The first start waits for an address (Network's), so the first query
    // for nodes reaches a network.
    if (enabled_ && !running_ && !begun_ && (dev_.address() || nowMs - startMs_ >= kSettleMs)) {
        begun_ = true;  // once: a socket that won't open isn't retried every period
        begin();
    }
    uint16_t gen = enabledNode_->generation();
    if (gen != enabledGen_) {
        enabledGen_ = gen;
        if (enabled_) {
            begun_ = true;
            begin();
        } else {
            end();
        }
    }
    if ((gen = portNode_->generation()) != portGen_) {
        portGen_ = gen;
        if (running_) {
            end();
            begin();
        }
    }
    // Kept to one interface, the socket is bound to it at begin(), if it is
    // up then: an address that changes (it came up, or another) binds again.
    bool single = interfaces_ == 1 || interfaces_ == 2 || interfaces_ == 4;
    if ((gen = interfacesNode_->generation()) != interfacesGen_ || (single && running_ && dev_.address() != address_)) {
        interfacesGen_ = gen;
        if (running_) {
            end();
            begin();
        }
    }
    if ((gen = accessNode_->generation()) != accessGen_) {
        accessGen_ = gen;
        if (tesser::UdpLink* link = transport_ ? transport_->link() : nullptr) {
            link->endpoint().setPeerAccess(static_cast<Access>(peerAccess_));
        }
    }
    if ((gen = mountNode_->generation()) != mountGen_) {
        mountGen_ = gen;
        if (!restart_) {
            restart_ = true;
            restartNode_->changed();
        }
    }
    if (nowMs - polledAtMs_ >= kPollMs) {
        polledAtMs_ = nowMs;
        refresh();
    }
}

Check UdpApi::send(const std::string& path, const std::string& valueJson, const std::string& to,
                   tesser::Pending* reply) {
    if (path.empty() || path[0] != '/') return Check::fail(Status::InvalidValue, "a path from the root: /a/b");
    if (!jsonOk(valueJson)) return Check::fail(Status::InvalidValue, "the value isn't JSON");
    tesser::UdpLink* link = transport_ ? transport_->link() : nullptr;
    if (!running_ || !link) return Check::fail(Status::NotAllowed, "UDP is off");
    const std::string body = valueJson.empty() ? "null" : valueJson;
    if (to.empty()) {
        // Every local network: the transport sends to each interface's
        // broadcast address.
        return link->broadcastSet(tesser::PeerAddress(), path, body) ? Check::ok()
                                                                      : Check::fail(Status::Internal, "not sent");
    }
    tesser::PeerAddress addr;
    if (!tesser::parseUdpAddress(to, port_, addr)) {
        return Check::fail(Status::InvalidValue, "an address: 192.168.1.59 or 192.168.1.59:8484");
    }
    tesser::DatagramEndpoint::ResponseHandler done;
    if (reply) {
        tesser::Pending pending = *reply;
        done = [pending](Status s, JsonVariantConst b) mutable {
            if (s == Status::Ok) return pending.reply(b);
            std::string message = b["message"] | "";
            pending.fail(s, message.empty() ? nullptr : message.c_str());
        };
    } else {
        done = [](Status, JsonVariantConst) {};
    }
    if (!link->endpoint().request(addr, tesser::Op::Set, path, body, std::move(done))) {
        return Check::fail(Status::Busy, "not sent");
    }
    return Check::ok();
}

}  // namespace tesserkit
