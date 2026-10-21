# Full-stack review: what remains open

The architecture review of 2026-10-14 covered TesserKIT, TesserAPI, TesserUI and NowTP. Most of its findings were fixed and released in TesserKIT v0.1.22 (2026-10-15), built on TesserAPI d1f4e6d, TesserUI 250ea7d and NowTP aca9321; Phase 1 of the roadmap followed in v0.1.26. This file lists what is still **not** fixed, or only partly, plus what the board runs found afterwards. Items fixed since are removed from the sections and recorded in the roadmap's status lines (last pruned 2026-10-21).

The [roadmap](#roadmap) at the end orders this work.

Line references are from the reviewed commits: TesserKIT c0e8b9f, TesserAPI 5842ba2, TesserUI 3385eb1, NowTP adb543b. The code has moved since, so search for the named function before trusting a line number. **Confirmed** means checked in the code or reproduced; **reasoned** means read from the code but not reproduced.

## Decisions waiting for Leo

1. **Applications build siblings' `main` unpinned.** tiny-heliostat and ballet-photophonique call `firmware.yml` with plain `owner/repo` dependencies. TesserKIT pins its siblings in `deps.lock`; these two follow whatever `main` holds when their release runs. TesserAPI's main was red for ESP-IDF 5.1 builds between ba3d44f and d1f4e6d. Option: give each a `deps.lock` and `dependencies-file: deps.lock`.
2. **The Ethernet variant of the full example is too big for a classic ESP32.** `examples/idf/full -DETHERNET_BOARD=gledopto` has about 5 KB of room in its slot (local IDF 6.2; CI's 5.5 images run about 15 KB larger, so it wouldn't fit in CI). With every service on, a Gledopto has 12 KB of free heap: HTTPS and backups are refused. With Bluetooth off it has 60 KB. Options: Bluetooth off by default on Ethernet boards, or fewer services in that variant. `release.yml`'s size gate doesn't build this variant.
3. ~~Memory tiers for gateways aren't applied.~~ **Answered by Phase 1 item 3:** each node advertises its message limit and peers send it no more (TesserAPI dd33e19, NowTP deadc54), so a gateway can take a smaller tier without the fleet changing. What's left is choosing the gateway profile's limits (Phase 2).
4. **Silent assertions in the full example** (258cced) saved 59 KB, so the release fits. A failed assert still panics with a backtrace and a core dump, but the console no longer prints the assert's file and line. Revert if you'd rather pay for the strings another way.
5. **NVS is too small for a fully configured device.** Every persisted string now has a length limit, and the worst-case `/sys` record (every service at its limits) is 16.7 KB; a 20 KB NVS partition can rewrite only about 6.7 KB of it (measured with ESP-IDF's real `nvs_flash` on the host, beside PHY calibration and NowTP's key). Since 1a4fd7a TesserKIT refuses a record NVS couldn't rewrite, keeping the stored one and warning in `/sys/status/settings`, rather than risking it. The fix is a 64 KB `nvs` partition for new boards: a partition change needs a serial flash. Leo's call. Since 2026-10-15 (Leo's decision), a device with Files keeps its settings on that filesystem instead, moved from NVS at boot (DESIGN §6.1): the boards on `tesserkit_4mb_files.csv` are covered without a new table. On TesserAPI 65ee017 (unbuffered FileStorage), checked on R1 and the Gledopto on 2026-10-16 (moved settings kept, saves, restart, factory reset). Still open for boards without Files: the 64 KB table.
6. **Ethernet takes the default route even with a static address and no router** (8326587, Leo's choice). Consequence, measured on a Gledopto: with a static cable to an isolated network and the station connected, internet traffic goes to the cable and fails (`server unreachable`). Since 580a52a an empty gateway is accepted and means "no gateway", but Ethernet keeps its route priority. An optional refinement: an empty gateway on Ethernet would also give up the default route and keep internet on Wi-Fi.

## Security

Several defaults are "open until someone sets it up", and the gaps add up across layers. Leo's policy (2026-10-15): most security features stay **opt-in**, so deployment stays simple on secure or low-stakes networks. The roadmap therefore makes them easy to switch on, rather than on by default; the few changes that cost nothing in deployment are the exception.

### TesserKIT defaults

- **Open access point.** The fallback AP defaults to `fallback` mode with an empty password (`network.h:138-140`). It comes up 15 s after the device goes offline, for example when a router reboots. Direction: a random per-device AP password (printed on serial, shown in the launcher), or AP fallback only while unprovisioned.
- **Release images are open.** Without `defaultAdmin` every client on every transport is Admin, OTA included. The release images CI publishes, which the launcher flashes onto new boards, are built without `secrets.h`, so they ship open. Direction: require claiming (first admin) on release images.
- **UDP `peerAccess` defaults to User** (`udp_api.h:76`) on every interface, the AP included. Spoofed datagrams can run User actions such as `/sequencer/play`. Direction: Public when Auth has an admin.
- **Tokens issued before the clock is set never expire** (`auth.cpp:203-207`). Show networks without SNTP get permanent bearer tokens, which travel in clear over HTTP and UDP. Direction: expire by uptime and boot count.
- **The Peers fleet password and the token key travel and persist widely.** Peers sends its fleet account password in clear over UDP to sign in (`peers.cpp:370-380`), stores it in every node's NVS and in backups. Backups also carry `/sys/auth/key`, which clones the token signing key. Direction: exclude `/sys/auth/key` from backups (or re-key on restore); a challenge rather than a password over UDP.
- **The login lockout is global** (`auth.cpp:224-232`): anyone can keep the admin locked out with five bad logins every 30 s. Direction: per client, or exponential per source.

### TesserAPI

- **Datagram transports trust source addresses** (`datagram.cpp:565-600`, `:711-770`, `remote.cpp:832-870`). A token is bound to the sender's IP or MAC, so spoofing an authenticated peer's address inherits its level. Writes need no reply, and notifications are accepted from the route's address, which lets forged state into mirrors. Call ids are sequential. Direction: per-session nonces and a MAC (HMAC with a fleet key), random call ids.
- **Spoofed advertisements compete for the peer caps.** Mounted nodes, routes and file sessions are capped and offline auto-mounted nodes evicted since b5a2c1c (Phase 1, so the heap is safe), but adverts with random names can still take the slots real peers would use (reasoned). Fixed by authenticated datagrams (Phase 3's fleet key).
- **`TokenCheck` sees only the token**, not the client (`api.h`). TesserKIT's "open until an account exists" therefore applies equally to HTTP on the soft-AP and to UDP, ESP-NOW and BLE from strangers. Direction: pass `const Client&`.
- **HTTP and WebSocket have no CSRF or DNS-rebinding defences** (`http_server.cpp:705-730`). Sets accept any Content-Type, so a web page can POST `text/plain` without a preflight. WebSocket checks no Origin, nothing checks the Host header, so any page the user visits can change an open device, and DNS rebinding lets it read. (Token comparison is constant-time since 4e383c7.) A WebSocket's level is fixed at the handshake, so a revoked token keeps working on an open socket. Direction: require `application/json` for sets, check Origin against the CORS allowlist and Host against known names, re-check tokens on revocation.
- **Tokens and the sign-in password travel in clear over relays** (and NowTP below).

### NowTP

- **Discovery is still spoofable without a key, and partly with one.**
  - A spoofed goodbye triggers `Lost`, which fails TesserAPI's pending calls and drops subscriptions.
  - A spoofed "moving" notice, or an anchor stating another channel (`channel.cpp:83-86`), herds every free node off-channel.
  - Discovery's peer list now evicts silent peers (bafbcde), but live spoofed announcements still compete for its 20 slots.
- **Content isn't encrypted, even with a key.** NowTP authenticates (integrity) but doesn't encrypt, so TesserAPI's sign-in tokens cross ESP-NOW in clear. The pairing already derives keys. Direction: document "integrity only" prominently, or encrypt.
- **The message id still isn't covered by the tag.** The replay fix (20b1ced) makes an id-flipped replay fail the sequence check, so this is defence in depth. Binding the id would change every message's tag, a larger wire break.
- **Downgrade of the new capabilities byte.** Stripping the challenge response's extension needs selective jamming of the real response plus injection. That's documented in PROTOCOL.md as residual risk.

### TesserUI

- **The launcher's origin runs node code where every token lives.** Tokens for every saved device sit in `localStorage` on `leokeba.github.io`, an origin shared by all of that account's Pages sites. f6b26fa stops foreign script URLs, but a node still serves its own `ui.script` path, and through the bridge every mDNS host becomes a live session without a click (`Workspace.svelte:238-241`). A malicious LAN host serving its own `/ext/x.js` gets code running next to every token. Direction: load node scripts only for nodes the user added or clicked; a dedicated origin for the launcher; or tokens out of the origin that runs node code.
- **Node identity is the URL, not anything authenticated.** A host that takes over the same IP is the node, and receives its token.
- **One Web Serial token for every port.** The key is always `tesserui:token:serial` (`session.svelte.ts:247`), so board A's token is sent to board B. The serial schema cache shares that key too.
- **The token travels in the WebSocket URL** (`?token=`, `session.svelte.ts:303`) and is forwarded in URLs by the bridge's proxy. Low risk.

### TesserUI's bridge (`bridge/bridge.mjs`)

- **No Host check, and requests without an Origin are allowed** (`:49-53`). A DNS-rebinding page sends same-origin GETs, which carry no Origin, so it can read `/nodes` and `/n/<id>/api/*`. Chrome's Local Network Access probably blocks this; Firefox and Safari don't (reasoned).
- **The allowlist is broad:** all of `https://leokeba.github.io` and any `http://localhost:*`.
- **`/s/<path>` opens any path**, not only the ports it lists, and takes over a port another page holds (`:289-316`).
- **mDNS targets are proxied whatever they are**, 127.0.0.1 included: a request-forgery path to local services. `describe()` fetches each target unprompted.
- Direction: check Host; restrict serial to listed ports; accept only private, non-loopback mDNS targets.

## Reliability and robustness

### TesserKIT

- **Policy in the platform layer** (`platform/esp/wifi_esp.cpp:208-262`). The DNS refill policy decides which interface's DNS wins, calls `dhcp_renew()` from inside a netif ext callback (re-entrancy inside lwIP reasoned, not verified), and keeps global `g_staticDns`; eth_esp.cpp depends on it. Services carry ESP-IDF specifics: disconnect reason codes (`network.cpp:86-106`, `Event::kReasonLeft = 8`) and DHCP/ACD timings. None of it is host-testable. Direction: lift DNS and default-route policy into Network, with `dnsServers()`, `setDns()`, `renewLease()`, `setDefault()` primitives; the platform translates reasons into a TesserKIT enum.
- **The network state machine is partly emergent.** The station has an explicit enum; its interaction with Ethernet and the AP is spread over predicates (`stationKeptAway`, `ethWaits`, `updateAp`'s `want`) and edge handlers. Network now re-reads interface state every second (6a48c46), which makes it robust, but not simpler.
- **NowTP forces `WIFI_PS_NONE`** (`espnow_transport.cpp:893-901`), which overrides `/sys/network/wifi/powerSave`. The setting has no effect while NowTP runs.
- **NowTP still runs its own task.** TesserKIT never sets NowTP's `runTask`; c3cebb9 removed only TesserAPI's worker. NowTP's polled mode has no wake hook or next-deadline query (see NowTP), so dropping the task (about 4 KB of stack) needs that first.

### TesserAPI

- **Complexity and scope** (dispatched to tesserapi-c0 on 2026-10-21, Phase 4.1). `RemoteNode` (`remote.cpp`, about 1,800 lines then, 2,250 now, several mutexes) combines forwarding, mirroring, shared upstream subscriptions, routes, failover and probing, ordered streamed sets, token presentation and file relays; `DatagramEndpoint` is similar. The library has grown from "a typed tree plus transports" into a mesh stack, with its own reliable UDP protocol duplicating NowTP's. Direction: split routing, upstream cache, streamer and relay out of `RemoteNode`. The lock-rank checker (fca51c8) now enforces lock order on host builds.
- **A renamed top-level node's record isn't migrated.** `persistVersion()`'s migration sees records by their stored name; renaming a top-level node loses its record. Rename below the top level only.
- **Untested on hardware:**
  - the `deferTimeoutMs` timeout of an async HTTP request (no firmware action leaves a reply pending, and a silent NowTP peer now fails fast first);
  - a large schema over BLE beside NowTP traffic (the test firmware has no Bluetooth);
  - a real 1 MB relayed image beside a live WebSocket (only 8 × 32 KB was relayed; e2e_transfers' relayed firmware passes since).
- **The main task's stack margin shrank:** `main_stack_free` 6,424 B, down from 7,944 B on the boot tests. Worth watching.

### NowTP

- **Channel and Wi-Fi coupling.**
  - The interface is chosen once at `begin()` (`espnow_transport.cpp:782`) and captured by `RadioLink` (`:297`). A device going from AP-only to station, or back, fails every send; whether TesserKIT restarts NowTP on mode changes is unverified.
  - A roaming station, or an AP changing channel, sends no move notice: followers notice after `peerTimeoutMs` (7 s) plus a channel search of about 3 s.
  - Nodes connected to APs on different channels are permanently partitioned. That's inherent to ESP-NOW, but NowTP knows each peer's channel and could report it.
  - The automatic power fallback lowers Wi-Fi transmit power for the rest of the boot (`:1315-1317`).
- **Callbacks run under NowTP's lock.** Receive, completion and peer-event callbacks hold NowTP's recursive mutex, so an application callback that takes a lock another task holds while calling NowTP can deadlock. TesserAPI avoids it by deferring; the contract is undocumented.
- **Polled mode has no wake hook.** `poll()` has no `nextDeadlineMs()` and no wake from `pushEvent`. To drop the `nowtp` task, add an `onEvent` hook (`xTaskNotifyGive` from the Wi-Fi task) and a next-deadline query.
- **Unauthenticated fragments can abort an authenticated reassembly.** An injected fragment with the same id but another count, or corrupt data, kills it (`engine.cpp:573-579`). No worse than jamming.
- **`LatestOnly` scope is per (peer, port)**, and TesserAPI multiplexes everything on port 84. Latent: TesserAPI doesn't use `LatestOnly` today.
- **Receiver credit.** nowtp-4f's recommendation for later: advertise free reassembly room in Busy and Missing acks, so senders wait before starting a message to a full receiver, and hold more than one message per destination only up to that credit. Today a refused message costs about one frame of airtime plus a resend.
- **Unicast Rejected acks to unauthenticated senders are kept by design.** They are how a sender with the wrong key gets `AuthFailed` quickly. With the peer table's eviction they can no longer fill it; rate-limiting them per sender remains an option.

### TesserUI

- **Hidden pages keep their push subscriptions.** Polls pause while `document.hidden` (df634a5); a background tab's subscriptions and events stream stay open.
- **The events-stream budget isn't shared across a launcher's connections.** Each opened connection holds one of a device's two streams.
- **Custom element versions collide.** The element registry is per page, so two devices shipping different versions of `x-sun-dial` both get the first one loaded (`Element.svelte:4-18`).
- **Optimistic writes on objects.** "Pending" is tracked per exact path (`store.svelte.ts:85`): a notification at a child of a pending object write (a form patch) overwrites the optimistic value.
- **HTTP transport edges.**
  - File transfers over HTTP use a raw XHR (`File.svelte:61-84`) with no `targetAddressSpace` and no timeout.
  - A bridged serial port is recognised by its API URL ending in `/s` (`session.svelte.ts:286`).
- **Versions and stubs.** Every package version says `0.1.0` while releases are past v0.1.8, so the `minVersion` check (`App.svelte:171`) can never trigger. DESIGN §9 promises stub pages "pin an exact version", but the README points stubs at the unversioned `leokeba.github.io/TesserUI/ui.js`, which changes with every push, with no SRI. Direction: versioned paths on Pages, SRI in `serveStub`, versions bumped at release.
- **The bundle budget was raised, not solved.** `ui.js.gz` is 61,955 of the new 66,560 bytes. The way out if it fills again: one wildcard WebUi handler serving gzipped chunks (WebUi has one handler per asset under a cap of 10, `web_ui.h:33-36`), so rarely used widgets load on demand.

## Packaging and process

- **Manifest versions stay at `0.1.0`** in TesserKIT's `library.json` and `idf_component.yml` (and the siblings'), while tags are past v0.1.22. `firmware.yml` treats `library.json`'s version as a floor, so it can lag, but consumers see nothing meaningful. Bumping it needs a commit per release, which the release workflow can't make.
- **`idf_component.yml` doesn't list TesserAPI or TesserUI.** ESP-IDF users add them by hand (the file says so).
- **Four mechanisms decide which features are built:** instantiation, builder opt-ins (`downloads()`, `github()`, `ethernet()`), Kconfig (mDNS, Home Assistant) and `-D`/sdkconfig detection. DESIGN §1 says "there are no feature flags". Direction: one documented scheme (Kconfig, mirrored as `-D` on Arduino).
- **The size budgets of DESIGN §11** (60 KB for the core services, 12 KB of RAM at rest, ...) are measured by hand. CI now only checks the room left in each released image's slot.
- **CI runs host tests and ESP-IDF builds only.** No Arduino build of TesserKIT, no hardware in the loop; the e2e scripts are run by hand. Direction: a nightly self-hosted runner with a devkit running e2e_device and e2e_udp against the pinned commits.

## Found during the board runs, not yet explained

- **One DEVKIT-V3 panic without a core dump** during the first e2e_udp run on the integration build (TesserAPI ba3d44f). Not seen again in three runs; ESP-IDF 6.1-dev's core dump writer sometimes fails to write (AGENTS.md).

- **Explained and fixed (2026-10-16): a relayed firmware lost the peer's upload at its last chunk** ("no upload in progress at this path", V3 gateway, R1 peer). NowTP declared the gateway lost after 7 s without discovery announcements, although its data kept arriving, and TesserAPI then dropped the peer's file session. NowTP 7586949 counts any accepted message or ack as heard; TesserAPI 65ee017 keeps a peer's calls and transfers through a link loss within 5 s of hearing it. Reproduced and checked by tesserapi-72 by silencing the gateway's announcements mid-upload.
- **The Gledopto as gateway with Bluetooth on runs out of memory** (2026-10-16, V3 browning out so the Gledopto took its role): lowest heap 92 bytes; the settings download, serial file and firmware transfers refused `low_memory`; HTTP stopped answering for a while. Built without Bluetooth it had 106 KB free (58 KB at the lowest) and passed every suite. Evidence for Phase 2's classic gateway profile.

## Never tried on hardware

From AGENTS.md and this round:

- File transfers over Bluetooth, and NowTP uploads from TesserKIT's services.
- A link-local Ethernet cable (needs a cable without DHCP).
- Ethernet as default route with the station on `always` and both on routers (route priority 8326587 was checked only with a dead static subnet).
- Firmware auto-confirmation through the access point (5ccaeb8; needs a phone on the AP for a minute).
- NowTP pairing between two TesserKIT nodes (NowTP's own pair test passed).
- The C3's GPIO wake-up, a real battery and charger pin, a real Home Assistant.
- Arduino builds with Files on hardware.
- The launcher's file buttons on TesserKIT's services in a browser.
- The deferred-reply timeout and BLE large replies (see TesserAPI).

## Roadmap

### Principles (Leo, 2026-10-15)

1. **Memory and stability come first.** No out-of-memory abort should be reachable from a client, and the reference boards should keep known headroom under load.
2. **Security stays opt-in.** Defaults stay open for secure or low-stakes networks; what matters is that one switch turns on a coherent secure mode. Changes that cost nothing in deployment are made by default anyway.
3. **Profiles instead of one board for everything.** A classic ESP32 can't hold every service long term, but it should hold as much as it can. The full set targets the ESP32-S3 or a board with PSRAM.

Owners: **KIT** TesserKIT, **API** TesserAPI, **UI** TesserUI, **NowTP**. Phases are in order, but items inside a phase are independent, and a later phase's item can start once its layer is free.

### Phase 1: stability under memory pressure

**Status (2026-10-15, released in TesserKIT v0.1.26):**
- Done: items 1 to 7. TesserAPI e74b0ff..5817cc9, NowTP a1b379f..fdd4449, TesserUI f91c706..fc29e6a, TesserKIT 580a52a..06586b1.
  - TesserKIT: static addresses without a gateway, factory reset in the device task, passwords hashed off the lock, bounded strings and the NVS guard, chaos tests, the allocator cap, `test/hil.py`, the UART handler in IRAM, network settings applied after the reply, and the log's lines in a ring.
  - Item 6: the UDP broadcast loss is the access point, not a regression (v0.1.21: 7 failed checks in 5 runs, v0.1.22: 1). The serial base64 corruption was bytes lost on the UART during flash writes: caught now, and made rare by the IRAM handler.
- `test/hil.py` on v0.1.26's commits: device 37/37, files 39/39, ethernet 16/16, transfers 18/18, udp 32/34. No crash or brownout. DEVKIT-V3's lowest heap through the gateway suites was 7.1 KB (3.3 KB before).
- After the release: V3's lowest heap in the files suite (1.4–1.7 KB) was traced by tesserapi-ae to HTTP downloads and Peers' status rebuild; TesserAPI 2f5c47a paces downloads by the heap, and TesserKIT e36f93e reserves the status first (dd698fd): 5.3 KB at the lowest since.
- Open:
  - The Gledopto with every service and Ethernet sits at 4–7 KB (decision 2, Phase 2's profiles).
  - Registering a nightly runner for `test/hil.py` is Leo's to do.


Goal: every allocation a client can trigger is reserved or refused, and the boards' headroom is measured continuously.

1. **Catch unguarded allocations on the host (API, KIT, NowTP).** Every abort found this round was found on hardware. An allocator cap in the host tests (a replacement `operator new` with a budget that fails like ESP-IDF) would find them earlier, as would a chaotic mode in TesserKIT's fake platform (delayed, reordered and dropped events).
2. **A hardware-in-the-loop run (KIT).** A nightly self-hosted runner with DEVKIT-V3, DEVKIT-R1 and a Gledopto running e2e_device, e2e_transfers, e2e_udp and the gateway stress against the pinned commits, recording each board's lowest heap and failing under a floor (8 KB to start). It also covers most of the "never tried on hardware" list over time.
3. **Per-device message limits instead of fleet-wide tiers (API, NowTP, KIT).** Each node advertises its largest message and reply in its advertisements and `view=info` (a protocol feature), and gateways send each peer no more than it takes. A classic ESP32 gateway can then use the 30–50 KB tier without every other device changing. This answers decision 3.
4. **Download chunks sized by the heap (API).** `maxResponse` taken from the byte budget at the time of the request, since the network stack's copies of a reply in flight scale with it and no reservation sees them.
5. **Caps on what peers can make a node hold (API).** Mounted remote nodes, routes and per-peer file sessions are uncapped today. That is a memory problem before it is a security one.
6. **Explain the open board findings (KIT, API, NowTP).**
   - The 10 ms rebuild of a refused 5–6 KB notification on R1.
   - The extra UDP broadcast loss: rerun e2e_udp on v0.1.21 and v0.1.22 on the same boards; if it is NowTP's traffic on the shared channel, pace discovery and link checks.
   - The base64 corruption seen once on the serial line.
7. **Stability bugs already identified.**
   - NowTP: duplicate suppression per sender; refuse a message at the heap floor before acknowledging it Complete.
   - KIT: a static IP without a gateway; the static DNS default; `g_portalUrl`; factory reset from a transport task; password changes hashing under the lock; the worst-case `/sys` record against the 20 KB NVS partition.
   - API: the queued-mode race; raw `RemoteNode*` outside the lock.
   - UI: Bluetooth write errors; log lines read as replies; `close()`'s retry loop; BigInt values; the launcher reopening USB ports by itself, which resets boards, a running motor controller included.

### Phase 2: profiles and footprint

Goal: documented service combinations per target, each with flash and heap headroom checked in CI and on the boards.

1. **Define the profiles (KIT).** For example:
   - **classic node:** classic ESP32 4 MB without PSRAM. Network, Time, Firmware, Backup, Auth, and one of UDP or NowTP. Bluetooth off on Ethernet boards (decision 2).
   - **classic gateway:** the same plus Peers and both transports, with the gateway memory settings, and no Bluetooth or MQTT TLS.
   - **full:** ESP32-S3 or PSRAM, every service.

   Each profile gets an example and a CI build under the size gate, the Ethernet variant included, plus a heap floor on the hardware run.
2. **One way to select features (KIT).** Kconfig on ESP-IDF, mirrored as `-D` on Arduino, so a profile is a configuration file rather than code.
3. **Use PSRAM where it exists (API, NowTP, KIT).** Large buffers come from PSRAM when the chip has it: replies, reassembly, file relays, TLS. The accountant then counts internal RAM and PSRAM separately, so S3 boards don't run into internal-RAM limits.
4. **Reclaim flash and RAM.**
   - UI: lazy widget chunks through one wildcard WebUi handler.
   - NowTP: a wake hook and next-deadline query, so TesserKIT can drop the `nowtp` task (about 4 KB of stack).
   - KIT: partition tables per profile (larger app slots where LittleFS isn't needed). Changing a table needs a serial flash, so it applies to new boards.
   - Revisit silent assertions once profiles leave room (decision 4).
5. **Policy out of the platform layer (KIT).** DNS refill and default-route policy move into Network, with platform primitives and a TesserKIT enum for disconnect reasons, so they're host-tested.

### Phase 3: security as an opt-in mode

**Status (2026-10-21):** constant-time token comparison done (TesserAPI 4e383c7, pinned since a3821f5). Everything else open.

Goal: defaults stay open; one switch (per device, or per fleet) turns on a coherent secure mode, and its threat model is documented.

1. **Free hardening, made by default.**
   - UI bridge: Host check; serial only on listed ports; no loopback mDNS targets.
   - API: constant-time token comparison.
   - UI: one Web Serial token per port. The launcher loads a node's own scripts only for nodes the user added or opened, not for every node the bridge finds.
2. **The secure mode bundle**, switched on with the first admin account or one setting (KIT, API):
   - an AP password;
   - UDP `peerAccess` Public;
   - clockless tokens expired by uptime and boot count;
   - backups without the token key (or re-keyed on restore);
   - lockout per client;
   - HTTP sets only as JSON, with Origin and Host checks;
   - WebSocket levels re-checked when a token is revoked;
   - `TokenCheck` given the client.
3. **A fleet key** (API, NowTP, KIT):
   - HMAC on UDP datagrams, with random call ids;
   - encrypted NowTP content once paired;
   - a challenge instead of a password for Peers' sign-in.
4. **Claiming in the launcher (UI, KIT):** an optional first-admin step after flashing a release image.
5. **A threat model** per mode in DESIGN (KIT), including NowTP's spoofable discovery and the downgrade risk of its capabilities byte.

### Phase 4: architecture, protocol and packaging

**Status (2026-10-21):**
- Done: per-Api remote-node hooks (TesserAPI 82c45b6; TesserKIT's Peers on them in a3821f5); remembered file probe below remote nodes (6640d46); gateways send `"v":2` with forwarded options (fca7629, tested end to end in 0440024).
- In progress: the `RemoteNode`/`DatagramEndpoint` split, with tesserapi-c0.

1. **TesserAPI's structure (API):** split `RemoteNode` (routing, upstream cache, streamer, relay); per-instance hooks instead of process-wide statics (TesserKIT's Peers follows); cache remote schemas so requests below a remote node skip the probe.
2. **Protocol (API, UI):** gateways send `"v":2` with forwarded options; TesserUI does once an option needs it.
3. **NowTP (NowTP):**
   - callbacks outside its lock, or the contract documented;
   - the interface re-bound on Wi-Fi mode changes;
   - move notices when a station roams;
   - peers' channels reported;
   - receiver credit;
   - Wi-Fi power save left to the application (`/sys/network/wifi/powerSave`), not forced to `WIFI_PS_NONE`.
4. **TesserUI's client (UI):**
   - hidden pages drop their push subscriptions;
   - one events-stream budget across a launcher's connections;
   - custom element version collisions;
   - optimistic writes on objects;
   - versioned stub paths with SRI, and version bumps at release.
5. **Packaging (KIT and apps):**
   - applications pinned with `deps.lock` (decision 1);
   - manifest versions that mean something;
   - siblings listed in `idf_component.yml`;
   - an Arduino build of TesserKIT in CI.
