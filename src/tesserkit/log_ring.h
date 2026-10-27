#pragma once

#include <stddef.h>
#include <string.h>

namespace tesserkit {

// The log's lines in one ring of bytes, each ending with a newline (docs/
// DESIGN.md section 5.14): written as the firmware logs them, taken once by
// the Log service (next()), and kept after that as the recent history until
// newer lines need the room. One buffer for both, allocated once by the
// platform's LogOutput, so keeping a line never allocates. Not thread-safe:
// the platform calls it under its own lock.
class LogRing {
public:
    void attach(char* buf, size_t cap) {
        buf_ = buf;
        cap_ = cap;
        start_ = used_ = unread_ = 0;
    }

    // Appends a line (without its newline), dropping the oldest lines
    // already taken to make room. False if the lines not taken yet leave no
    // room: the line is lost. A line longer than the ring is cut to fit.
    bool push(const char* text, size_t n) {
        if (cap_ < 2) return false;
        if (n > cap_ - 1) n = cap_ - 1;
        size_t need = n + 1;
        while (cap_ - used_ < need && used_ > unread_) dropOldest();
        if (cap_ - used_ < need) return false;
        size_t at = (start_ + used_) % cap_;
        size_t first = n < cap_ - at ? n : cap_ - at;
        memcpy(buf_ + at, text, first);
        memcpy(buf_, text + first, n - first);
        buf_[(at + n) % cap_] = '\n';
        used_ += need;
        unread_ += need;
        return true;
    }

    // The oldest line not taken yet, without its newline: up to `cap` of its
    // bytes into `out` (the rest is skipped), their count in `n`. False if
    // there is none.
    bool next(char* out, size_t cap, size_t& n) {
        n = 0;
        if (!unread_) return false;
        size_t at = (start_ + used_ - unread_) % cap_;
        while (unread_) {
            char c = buf_[at];
            at = (at + 1) % cap_;
            unread_--;
            if (c == '\n') break;
            if (n < cap) out[n++] = c;
        }
        return true;
    }

    // Bytes kept, lines not taken yet included.
    size_t size() const { return used_; }

    // The lines kept, oldest first, into `out`: the newest `cap` bytes if
    // there are more. Returns the bytes copied.
    size_t copy(char* out, size_t cap) const {
        size_t n = used_ < cap ? used_ : cap;
        if (!n) return 0;
        size_t from = (start_ + used_ - n) % cap_;
        size_t first = n < cap_ - from ? n : cap_ - from;
        memcpy(out, buf_ + from, first);
        memcpy(out + first, buf_, n - first);
        return n;
    }

private:
    // The oldest line, taken already, goes whole.
    void dropOldest() {
        size_t taken = used_ - unread_;
        size_t i = 0;
        while (i < taken && buf_[(start_ + i) % cap_] != '\n') i++;
        i = i < taken ? i + 1 : taken;
        start_ = (start_ + i) % cap_;
        used_ -= i;
    }

    char* buf_ = nullptr;
    size_t cap_ = 0;
    size_t start_ = 0;   // the oldest byte
    size_t used_ = 0;    // bytes kept, newlines included
    size_t unread_ = 0;  // the newest of them, not taken by next() yet
};

}  // namespace tesserkit
