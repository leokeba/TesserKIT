#include "tesserkit/image.h"

#include <string.h>

namespace tesserkit {

using tesser::Check;
using tesser::Status;

namespace {
constexpr uint8_t kImageMagic = 0xE9;            // esp_image_header_t.magic
constexpr size_t kChipIdOffset = 12;             // esp_image_header_t.chip_id
constexpr size_t kAppDescOffset = 32;            // 24-byte image header + 8-byte segment header
constexpr uint32_t kAppDescMagic = 0xABCD5432;   // esp_app_desc_t.magic_word
}  // namespace

Check checkImageHead(const uint8_t* head, size_t len, uint16_t chipId) {
    if (len < kImageHeadSize || head[0] != kImageMagic) return Check::fail(Status::InvalidValue, "not an app image");
    uint16_t id = static_cast<uint16_t>(head[kChipIdOffset] | (head[kChipIdOffset + 1] << 8));
    if (id != chipId) return Check::fail(Status::InvalidValue, "image built for another chip");
    const uint8_t* m = head + kAppDescOffset;
    uint32_t magic = static_cast<uint32_t>(m[0]) | static_cast<uint32_t>(m[1]) << 8 |
                     static_cast<uint32_t>(m[2]) << 16 | static_cast<uint32_t>(m[3]) << 24;
    if (magic != kAppDescMagic) return Check::fail(Status::InvalidValue, "not an app image");
    return Check::ok();
}

ImageWriter::~ImageWriter() {
    if (open_ && !done_) ota_.abort();
}

Check ImageWriter::flushHead() {
    Check c = checkImageHead(head_, headLen_, chipId_);
    if (!c.isOk()) return c;
    c = ota_.open(size_);
    if (!c.isOk()) return c;
    open_ = true;
    return ota_.write(head_, headLen_);
}

Check ImageWriter::write(const uint8_t* data, size_t len) {
    if (done_) return Check::fail(Status::Internal, "image already finished");
    if (size_ && received_ + len > size_) return Check::fail(Status::TooLarge, "more bytes than announced");
    received_ += static_cast<uint32_t>(len);
    if (!open_) {
        size_t take = kImageHeadSize - headLen_;
        if (take > len) take = len;
        memcpy(head_ + headLen_, data, take);
        headLen_ += take;
        data += take;
        len -= take;
        if (headLen_ < kImageHeadSize) return Check::ok();
        Check c = flushHead();
        if (!c.isOk()) return c;
    }
    return len ? ota_.write(data, len) : Check::ok();
}

Check ImageWriter::finish() {
    if (!open_) return Check::fail(Status::InvalidValue, "not an app image");
    if (size_ && received_ != size_) return Check::fail(Status::InvalidValue, "image cut short");
    done_ = true;
    return ota_.finish();
}

}  // namespace tesserkit
