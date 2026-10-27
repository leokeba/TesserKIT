// Sequencer (docs/DESIGN.md section 5.14).
#pragma once

#include "rig.h"

namespace sequencer_cases {

using rig::at;
using rig::Op;
using rig::Status;
using tesserkit::Wait;

struct SeqRig : rig::Rig {
    tesserkit::Sequencer seq{dev};
    bool on = false;
    int presses = 0;
    int moves = 0;
    tesser::Pending move;
    SeqRig() {
        auto& led = dev.api().object("led");
        led.value("on", on);
        led.action("press", [this] { presses++; });
        led.action("move", [this](tesser::Call& call) {  // replies when the test says the move ended
            moves++;
            move = call.defer();
        });
    }
    static tesserkit::Step mk(const char* name, const char* path, const char* value, Wait wait, uint32_t ms,
                                const char* next) {
        tesserkit::Step s;
        snprintf(s.name, sizeof(s.name), "%s", name);
        s.path = path;
        s.value = value;
        s.wait = wait;
        s.ms = ms;
        s.next = next;
        return s;
    }
    Status play(const char* step) { return set("/sequencer/play", std::string("\"") + step + "\"").status; }
};

TEST(sequencer_plays_steps_in_turn) {
    SeqRig r;
    r.seq.add(r.mk("on", "/led/on", "true", Wait::Time, 100, "+1"))
        .add(r.mk("press", "/led/press", "null", Wait::Time, 100, ""))
        .finally("/led/on", "false");
    CHECK(r.dev.begin());
    CHECK_EQ(r.play(""), Status::Ok);  // from the first step
    r.step();
    CHECK(r.on);
    CHECK_EQ(r.presses, 0);
    CHECK_EQ(r.value("/sequencer/status/name"), "\"on\"");
    CHECK_EQ(r.value("/sequencer/status/duration"), "100");
    r.run(60);
    CHECK_EQ(r.presses, 0);
    r.run(60);
    CHECK_EQ(r.presses, 1);
    CHECK(r.on);
    CHECK_EQ(r.value("/sequencer/status/step"), "1");
    r.run(120);  // the end: finally
    CHECK(!r.on);
    CHECK_EQ(r.value("/sequencer/status/running"), "false");
    CHECK_EQ(r.value("/sequencer/status/step"), "-1");
}

// A step's write and finally's aren't saved (DESIGN 5.13): a looping
// sequence on a persisted node rewrote its record every 2 s.
TEST(sequencer_writes_arent_saved) {
    SeqRig r;
    int level = 0;
    r.dev.api().object("dim").value("level", level).range(0, 255).persist();
    r.seq.add(r.mk("up", "/dim/level", "200", Wait::Time, 100, "")).finally("/dim/level", "201");
    CHECK(r.dev.begin());
    CHECK_EQ(r.play(""), Status::Ok);
    r.run(200);
    CHECK_EQ(level, 201);
    r.run(3000);  // past TesserAPI's 2 s debounce
    std::string stored;
    bool kept = !r.storage.load("dim", stored);
    CHECK(kept || (stored.find("200") == std::string::npos && stored.find("201") == std::string::npos));
    CHECK_EQ(r.set("/dim/level", "7").status, Status::Ok);
    r.run(3000);
    CHECK(r.storage.load("dim", stored));
    CHECK(stored.find("7") != std::string::npos);
}

TEST(sequencer_waits_for_replies) {
    SeqRig r;
    r.seq.add(r.mk("move", "/led/move", "null", Wait::Reply, 0, "+1"))
        .add(r.mk("press", "/led/press", "null", Wait::None, 0, ""));
    CHECK(r.dev.begin());
    CHECK_EQ(r.play("move"), Status::Ok);
    r.run(500);
    CHECK_EQ(r.moves, 1);
    CHECK_EQ(r.presses, 0);
    CHECK_EQ(r.value("/sequencer/status/running"), "true");
    r.move.reply();  // the move ended
    r.run(40);
    CHECK_EQ(r.presses, 1);
    CHECK_EQ(r.value("/sequencer/status/running"), "false");
}

TEST(sequencer_reply_timeouts_go_on) {
    SeqRig r;
    r.seq.add(r.mk("move", "/led/move", "null", Wait::Reply, 100, "+1"))
        .add(r.mk("press", "/led/press", "null", Wait::Time, 0, ""));
    CHECK(r.dev.begin());
    rig::Events ev(r, "/sequencer/failed");
    CHECK_EQ(r.play("move"), Status::Ok);
    r.run(60);
    CHECK_EQ(r.presses, 0);
    r.run(80);
    CHECK_EQ(r.presses, 1);
    std::vector<std::string> got = ev.take();
    CHECK_EQ(got.size(), size_t(1));
    if (!got.empty()) CHECK_EQ(got[0], "\"move: no reply in 100 ms\"");
    CHECK_EQ(r.value("/sequencer/status/error"), "\"move: no reply in 100 ms\"");
    r.move.reply();  // late: the sequence has moved on
    r.run(100);
    CHECK_EQ(r.value("/sequencer/status/name"), "\"press\"");
    CHECK_EQ(ev.take().size(), size_t(0));
}

TEST(sequencer_timed_steps_dont_drift) {
    // Ten 50 ms steps over periods of 20 ms: each starts at the previous
    // one's deadline, not at the period that noticed it (which took 580 ms).
    for (uint32_t period : {10u, 20u}) {
        struct Count : SeqRig {
            int n = 0;
            Count() {
                dev.api().value("count", [this] { return n; }, [this](int v) { n = v; });
                for (int i = 0; i < 10; i++) {
                    std::string v = std::to_string(i);
                    seq.add(mk("", "/count", v.c_str(), Wait::Time, 50, i < 9 ? "+1" : ""));
                }
            }
        } r;
        CHECK(r.dev.begin());
        r.settle();
        CHECK_EQ(r.play(""), Status::Ok);
        uint32_t t = 0, start = 0;
        bool started = false;
        for (; t < 2000; t += period) {
            r.step(period);
            if (!started && r.seq.current() == 0) {
                started = true;
                start = t;
            }
            if (started && !r.seq.running()) break;
        }
        CHECK_EQ(r.n, 9);
        CHECK(t - start >= 500 && t - start <= 500 + period);
    }
    // A deadline noticed long after (a stalled task) isn't caught up: the
    // next step gets its whole time.
    SeqRig r;
    r.seq.add(r.mk("on", "/led/on", "true", Wait::Time, 50, "+1"))
        .add(r.mk("press", "/led/press", "null", Wait::Time, 100, ""));
    CHECK(r.dev.begin());
    CHECK_EQ(r.play(""), Status::Ok);
    r.step();
    r.step(500);  // the task stalled
    CHECK_EQ(r.presses, 1);
    r.run(80);
    CHECK(r.seq.running());
    r.run(40);
    CHECK(!r.seq.running());
}

TEST(sequencer_executes_one_step) {
    SeqRig r;
    r.seq.add(r.mk("on", "/led/on", "true", Wait::Time, 100, "+1"))
        .add(r.mk("press", "/led/press", "null", Wait::None, 0, ""))
        .finally("/led/on", "false");
    CHECK(r.dev.begin());
    CHECK_EQ(r.set("/sequencer/execute", "\"0\"").status, Status::Ok);
    r.step();
    CHECK(r.on);
    r.run(120);
    CHECK_EQ(r.presses, 0);  // next isn't followed
    CHECK(!r.on);            // finally
    CHECK_EQ(r.value("/sequencer/status/running"), "false");
}

TEST(sequencer_holds_until_stopped) {
    SeqRig r;
    r.seq.add(r.mk("on", "/led/on", "true", Wait::Time, 0, "+1"))
        .add(r.mk("press", "/led/press", "null", Wait::None, 0, ""))
        .finally("/led/on", "false");
    CHECK(r.dev.begin());
    CHECK_EQ(r.play("on"), Status::Ok);
    r.run(5000);
    CHECK(r.on);
    CHECK_EQ(r.value("/sequencer/status/running"), "true");
    CHECK_EQ(r.set("/sequencer/stop").status, Status::Ok);
    r.step();
    CHECK(!r.on);
    CHECK_EQ(r.presses, 0);
    CHECK_EQ(r.value("/sequencer/status/running"), "false");
}

TEST(sequencer_play_takes_over) {
    SeqRig r;
    r.seq.add(r.mk("on", "/led/on", "true", Wait::Time, 0, ""))
        .add(r.mk("press", "/led/press", "null", Wait::Time, 0, ""))
        .finally("/led/on", "false");
    CHECK(r.dev.begin());
    CHECK_EQ(r.play("on"), Status::Ok);
    r.run(100);
    CHECK_EQ(r.play("press"), Status::Ok);
    r.step();
    CHECK_EQ(r.presses, 1);
    CHECK(r.on);  // no finally between the two
    CHECK_EQ(r.value("/sequencer/status/step"), "1");
    // A stop and a play in one period: in that order.
    CHECK_EQ(r.set("/sequencer/stop").status, Status::Ok);
    CHECK_EQ(r.play("on"), Status::Ok);
    r.on = true;
    r.step();
    CHECK(r.on);
    CHECK_EQ(r.value("/sequencer/status/step"), "0");
    // A play and then a stop: stopped.
    CHECK_EQ(r.play("press"), Status::Ok);
    CHECK_EQ(r.set("/sequencer/stop").status, Status::Ok);
    r.step();
    CHECK_EQ(r.presses, 1);
    CHECK_EQ(r.value("/sequencer/status/running"), "false");
}

TEST(sequencer_steps_by_name_index_and_offset) {
    SeqRig r;
    r.seq.add(r.mk("a", "/led/press", "null", Wait::None, 0, "2"))
        .add(r.mk("b", "/led/press", "null", Wait::None, 0, ""))
        .add(r.mk("", "/led/press", "null", Wait::None, 0, "-1"));
    CHECK(r.dev.begin());
    CHECK_EQ(r.seq.find("b"), 1);
    CHECK_EQ(r.seq.find("2"), 2);
    CHECK_EQ(r.seq.find("+1", 0), 1);
    CHECK_EQ(r.seq.find("-1", 0), -1);
    CHECK_EQ(r.seq.find("+1"), -1);  // relative to nothing
    CHECK_EQ(r.seq.find("3"), -1);
    CHECK_EQ(r.seq.find("nope"), -1);
    CHECK_EQ(r.play("a"), Status::Ok);
    r.step();  // a, #2, b: none waits
    CHECK_EQ(r.presses, 3);
    CHECK_EQ(r.play("nope"), Status::NotFound);
    CHECK_EQ(r.play("7"), Status::NotFound);
    CHECK_EQ(r.set("/sequencer/play", "3").status, Status::InvalidValue);  // a string, even for an index
}

TEST(sequencer_breaks_loops_that_never_wait) {
    SeqRig r;
    r.seq.add(r.mk("a", "/led/press", "null", Wait::None, 0, "+1"))
        .add(r.mk("b", "/led/press", "null", Wait::Reply, 0, "a"));  // the press replies at once
    CHECK(r.dev.begin());
    CHECK_EQ(r.play("a"), Status::Ok);
    r.step();
    CHECK(r.presses < 10);
    CHECK_EQ(r.value("/sequencer/status/running"), "false");
    CHECK_EQ(r.value("/sequencer/status/error"), "\"b: the steps loop without waiting\"");
}

TEST(sequencer_failures_are_events) {
    SeqRig r;
    r.seq.add(r.mk("bad", "/led/on", "\"yes\"", Wait::Reply, 0, "+1"))
        .add(r.mk("good", "/led/press", "null", Wait::None, 0, "nope"));
    CHECK(r.dev.begin());
    rig::Events ev(r, "/sequencer/failed");
    CHECK_EQ(at(r.schema("/sequencer/failed").body, "ui/toast"), "\"warn\"");
    CHECK_EQ(r.play("bad"), Status::Ok);
    r.step();
    CHECK_EQ(r.presses, 1);  // a failed step doesn't stop the sequence
    CHECK_EQ(r.value("/sequencer/status/running"), "false");  // an unknown next does
    std::vector<std::string> got = ev.take();
    CHECK_EQ(got.size(), size_t(2));
    if (got.size() == 2) {
        CHECK_EQ(got[0], "\"bad: invalid_value: expected boolean\"");
        CHECK_EQ(got[1], "\"good: no step \\\"nope\\\"\"");
    }
}

TEST(sequencer_plays_at_boot) {
    std::map<std::string, std::string> saved;
    {
        SeqRig r;
        CHECK(r.dev.begin());
        CHECK_EQ(r.set("/sequencer",
                       "{\"steps\":[{\"path\":\"/led/press\",\"wait\":\"none\"},"
                       "{\"name\":\"show\",\"path\":\"/led/on\",\"value\":\"true\",\"wait\":\"time\"}],"
                       "\"boot\":\"show\",\"finally\":{\"path\":\"/led/on\",\"value\":\"false\"}}")
                     .status,
                 Status::Ok);
        r.run(2500);
        CHECK_EQ(r.presses, 0);  // boot is for the next boot
        saved = r.storage.records;
    }
    SeqRig r;
    r.storage.records = saved;
    CHECK(r.dev.begin());
    r.step();
    CHECK(r.on);
    CHECK_EQ(r.presses, 0);
    CHECK_EQ(r.value("/sequencer/status/name"), "\"show\"");
    CHECK_EQ(r.set("/sequencer/stop").status, Status::Ok);
    r.step();
    CHECK(!r.on);
}

TEST(sequencer_writes_are_checked) {
    SeqRig r;
    CHECK(r.dev.begin());
    auto steps = [&r](const char* item) { return r.set("/sequencer/steps", std::string("[") + item + "]").status; };
    CHECK_EQ(steps("{\"name\":\"1st\"}"), Status::InvalidValue);
    CHECK_EQ(steps("{\"name\":\"+a\"}"), Status::InvalidValue);
    CHECK_EQ(steps("{\"next\":\"+x\"}"), Status::InvalidValue);
    CHECK_EQ(steps("{\"next\":\"1a\"}"), Status::InvalidValue);
    CHECK_EQ(steps("{\"path\":\"/led/nope\"}"), Status::NotFound);
    CHECK_EQ(steps("{\"value\":\"{oops\"}"), Status::InvalidValue);
    CHECK_EQ(steps("{\"wait\":\"later\"}"), Status::InvalidValue);
    CHECK_EQ(steps("{\"ms\":86400001}"), Status::InvalidValue);
    CHECK_EQ(steps("{\"name\":\"intro\",\"path\":\"/led/on\",\"value\":\"true\",\"wait\":\"time\",\"ms\":500,"
                   "\"next\":\"+1\"},{\"path\":\"/led/press\",\"next\":\"intro\"},{\"next\":\"0\"}"),
             Status::Ok);
    // A `*` segment stands for every child: checked by the API when it runs.
    CHECK_EQ(steps("{\"path\":\"/*/press\"}"), Status::Ok);
    CHECK(tesserkit::resolves(r.dev.api(), "/led/*/x"));
    CHECK(!tesserkit::resolves(r.dev.api(), "/nope/*"));
    CHECK_EQ(r.set("/sequencer/boot", "\"+1\"").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sequencer/boot", "\"2\"").status, Status::Ok);
    CHECK_EQ(r.set("/sequencer/finally/path", "\"/led/nope\"").status, Status::NotFound);
    CHECK_EQ(r.set("/sequencer/finally/path", "\"\"").status, Status::Ok);
    CHECK_EQ(r.set("/sequencer/status/step", "1").status, Status::ReadOnly);
    CHECK_EQ(r.request(r.anon, Op::Set, "/sequencer/steps", "[]").status, Status::Unauthorized);
    CHECK_EQ(r.request(r.anon, Op::Set, "/sequencer/play", "\"\"").status, Status::Unauthorized);
    CHECK_EQ(r.as(tesser::Access::User, Op::Set, "/sequencer/play", "\"intro\"").status, Status::Ok);
    CHECK_EQ(r.as(tesser::Access::User, Op::Set, "/sequencer/steps", "[]").status, Status::Unauthorized);
    rig::Reply s = r.schema("/sequencer/steps");
    CHECK_EQ(at(s.body, "items/children/wait/enum"), "[\"reply\",\"time\",\"none\"]");
    CHECK_EQ(at(s.body, "maxSize"), "32");
}

// A step on every child (TesserAPI's `*`), as a master on its peers: a
// reply waits for each one's.
TEST(sequencer_steps_fan_out) {
    struct Node {
        bool on = false;
        int presses = 0;
        tesser::Pending move;
    };
    SeqRig r;
    Node nodes[2];
    auto& all = r.dev.api().object("nodes");
    const char* names[] = {"a", "b"};
    for (int i = 0; i < 2; i++) {
        Node& n = nodes[i];
        auto& o = all.object(names[i]);
        o.value("on", n.on);
        o.action("press", [&n] { n.presses++; });
        o.action("move", [&n](tesser::Call& call) { n.move = call.defer(); });
    }
    all.object("c").value("on", r.on);  // no move nor press: skipped by those
    r.seq.add(r.mk("go", "/nodes/*/move", "null", Wait::Reply, 0, "+1"))
        .add(r.mk("press", "/nodes/*/press", "null", Wait::None, 0, "+1"))
        .add(r.mk("bad", "/nodes/*/on", "\"yes\"", Wait::None, 0, "+1"))
        .add(r.mk("none", "/nodes/*/nope", "null", Wait::None, 0, ""));
    CHECK(r.dev.begin());
    rig::Events ev(r, "/sequencer/failed");
    CHECK_EQ(r.play("go"), Status::Ok);
    r.run(200);
    CHECK(nodes[0].move && nodes[1].move);
    nodes[0].move.reply();
    r.run(200);
    CHECK_EQ(nodes[0].presses, 0);  // b is still moving
    nodes[1].move.reply();
    r.run(40);
    CHECK_EQ(nodes[0].presses, 1);
    CHECK_EQ(nodes[1].presses, 1);
    std::vector<std::string> got = ev.take();
    CHECK_EQ(got.size(), size_t(2));
    if (got.size() == 2) {
        CHECK_EQ(got[0], "\"bad: invalid_value: 3 of 3 failed: a: expected boolean\"");
        CHECK_EQ(got[1], "\"none: not_found: no match\"");
    }
}

TEST(sequencer_several_by_name) {
    SeqRig r;
    tesserkit::Sequencer lights(r.dev, "lights");
    lights.add(r.mk("", "/led/press", "null", Wait::None, 0, ""));
    CHECK(r.dev.begin());
    CHECK_EQ(r.set("/lights/play", "\"0\"").status, Status::Ok);
    r.step();
    CHECK_EQ(r.presses, 1);
    CHECK_EQ(r.value("/sequencer/status/running"), "false");
}

}  // namespace sequencer_cases
