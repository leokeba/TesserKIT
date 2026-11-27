// Minimal host test harness: ./tesserkit_tests [-v] [filter]
//
// Quiet by default: what a case prints (the device's log lines, mostly) is
// captured and shown only when the case fails, so a passing run is one line.
// A whole run used to print 155 KB, more than a small agent's context can
// spare for every test run.
//   FAIL <name>: <first failed check>   then the last lines of its log
//   CRASH <name>                        an abort or sanitizer exit, then its log
//   DONE pass=<n> fail=<n>
// A sanitizer report in a case's log fails that case, since UBSan doesn't
// stop the run and the report would otherwise go unseen.
// `-v` (or CHECK_VERBOSE=1) prints every case as it runs, with its log, and
// "PASS <name>" lines.
#pragma once

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define CHECK_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(CHECK_ASAN)
#define CHECK_ASAN 1
#endif
#ifdef CHECK_ASAN
#include <sanitizer/common_interface_defs.h>
#endif

#include <string>
#include <type_traits>
#include <vector>

namespace check {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int g_caseFailures = 0;
inline std::string g_firstFailure;

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline void fail(const char* file, int line, const std::string& what) {
    if (g_caseFailures++ == 0) {
        const char* base = strrchr(file, '/');
        g_firstFailure = std::string(base ? base + 1 : file) + ":" + std::to_string(line) + ": " + what;
    }
}

inline bool expect(bool ok, const char* expr, const char* file, int line) {
    if (!ok) fail(file, line, expr);
    return ok;
}

inline std::string show(const std::string& s) { return "\"" + s + "\""; }
inline std::string show(const char* s) { return s ? show(std::string(s)) : "null"; }
inline std::string show(bool v) { return v ? "true" : "false"; }
template <class T>
std::string show(const T& v) {
    if constexpr (std::is_enum<T>::value) {
        return std::to_string(static_cast<long long>(v));
    } else {
        return std::to_string(v);
    }
}

template <class A, class B>
bool expectEq(const A& a, const B& b, const char* ea, const char* eb, const char* file, int line) {
    bool ok;
    if constexpr (std::is_convertible<A, std::string>::value && std::is_convertible<B, std::string>::value) {
        ok = std::string(a) == std::string(b);
    } else {
        ok = a == b;
    }
    if (!ok) fail(file, line, std::string(ea) + " == " + eb + " (got " + show(a) + " vs " + show(b) + ")");
    return ok;
}

namespace detail {

inline int g_capture = -1;            // the file a running case writes to
inline int g_out = -1;                // the real stdout
inline int g_err = -1;                // the real stderr
inline const char* g_running = nullptr;
constexpr long kFailLines = 60;       // log lines shown under a FAIL
constexpr long kCrashLines = 200;     // under a CRASH: a sanitizer report is long

// What follows runs from signal handlers too: read, write and pread only.
inline void say(int fd, const char* s) { (void)!write(fd, s, strlen(s)); }

inline void sayNumber(int fd, long n) {
    char buf[24];
    int at = sizeof(buf);
    buf[--at] = '\0';
    do {
        buf[--at] = static_cast<char>('0' + n % 10);
        n /= 10;
    } while (n > 0 && at > 0);
    say(fd, buf + at);
}

// Writes the capture's last `lines` lines to `fd`, indented.
inline void showTail(int fd, long lines) {
    static char buf[4096];
    long total = 0;
    off_t at = 0;
    ssize_t n;
    while ((n = pread(g_capture, buf, sizeof(buf), at)) > 0) {
        for (ssize_t i = 0; i < n; i++) total += buf[i] == '\n';
        at += n;
    }
    long skip = total > lines ? total - lines : 0;
    if (skip > 0) {
        say(fd, "  ... ");
        sayNumber(fd, skip);
        say(fd, " earlier lines (-v shows them)\n");
    }
    bool lineStart = true;
    at = 0;
    while ((n = pread(g_capture, buf, sizeof(buf), at)) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            if (skip > 0) {
                skip -= buf[i] == '\n';
                continue;
            }
            if (lineStart) say(fd, "  ");
            (void)!write(fd, buf + i, 1);
            lineStart = buf[i] == '\n';
        }
        at += n;
    }
    if (!lineStart) say(fd, "\n");
}

inline std::string captured() {
    std::string out;
    char buf[4096];
    off_t at = 0;
    ssize_t n;
    while ((n = pread(g_capture, buf, sizeof(buf), at)) > 0) {
        out.append(buf, static_cast<size_t>(n));
        at += n;
    }
    return out;
}

// The run is ending inside a case: put the real outputs back and show what
// the case printed, the sanitizer's report included.
inline void onDeath() {
    if (g_capture < 0 || !g_running) return;
    dup2(g_out, 1);
    dup2(g_err, 2);
    say(1, "CRASH ");
    say(1, g_running);
    say(1, "\n");
    showTail(1, kCrashLines);
    g_running = nullptr;
}

inline void onSignal(int sig) {
    onDeath();
    signal(sig, SIG_DFL);
    raise(sig);
}

inline bool startCapture() {
    FILE* f = tmpfile();
    if (!f) return false;
    g_capture = fileno(f);
    g_out = dup(1);
    g_err = dup(2);
    signal(SIGABRT, onSignal);
#ifdef CHECK_ASAN
    // ASan reports SIGSEGV itself, then exits through this callback.
    __sanitizer_set_death_callback(onDeath);
#else
    signal(SIGSEGV, onSignal);
    signal(SIGBUS, onSignal);
    signal(SIGFPE, onSignal);
    signal(SIGILL, onSignal);
#endif
    return true;
}

inline void beginCase(const char* name) {
    fflush(stdout);
    fflush(stderr);
    (void)!ftruncate(g_capture, 0);
    lseek(g_capture, 0, SEEK_SET);
    g_running = name;
    dup2(g_capture, 1);
    dup2(g_capture, 2);
}

inline void endCase() {
    fflush(stdout);
    fflush(stderr);
    dup2(g_out, 1);
    dup2(g_err, 2);
    g_running = nullptr;
}

inline bool sanitizerReport(const std::string& log) {
    return log.find("runtime error:") != std::string::npos || log.find("Sanitizer") != std::string::npos;
}

}  // namespace detail

// Runs every case whose name contains `filter` (all when null or empty).
inline int run(const char* filter, bool verbose) {
    bool quiet = !verbose && detail::startCapture();
    int pass = 0, failed = 0;
    for (const Case& c : registry()) {
        if (filter && *filter && !strstr(c.name, filter)) continue;
        g_caseFailures = 0;
        g_firstFailure.clear();
        if (quiet) detail::beginCase(c.name);
        c.fn();
        if (quiet) {
            detail::endCase();
            if (g_caseFailures == 0 && detail::sanitizerReport(detail::captured())) {
                g_caseFailures = 1;
                g_firstFailure = "sanitizer report in its log";
            }
        }
        if (g_caseFailures == 0) {
            pass++;
            if (!quiet) printf("PASS %s\n", c.name);
        } else {
            failed++;
            printf("FAIL %s: %s\n", c.name, g_firstFailure.c_str());
            if (quiet) {
                fflush(stdout);
                detail::showTail(1, detail::kFailLines);
            }
        }
    }
    if (pass + failed == 0 && filter && *filter) {
        printf("no case name contains \"%s\"\n", filter);
        failed = 1;
    }
    printf("DONE pass=%d fail=%d\n", pass, failed);
    return failed;
}

}  // namespace check

#define TEST(name)                                                    \
    static void test_##name();                                        \
    static check::Registrar registrar_##name(#name, &test_##name);    \
    static void test_##name()

#define CHECK(cond) check::expect((cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ(a, b) check::expectEq((a), (b), #a, #b, __FILE__, __LINE__)
