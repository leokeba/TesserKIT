#pragma once

#include <stddef.h>
#include <stdint.h>

#include "tesserkit/platform.h"

namespace tesserkit {

// Bytes of an app image needed to check it: the image header and the first
// segment's header, then the app description's magic word.
constexpr size_t kImageHeadSize = 36;

// Checks the start of an app image (ESP-IDF's esp_image_header_t, then
// esp_app_desc_t) for the chip whose image id is `chipId`.
tesser::Check checkImageHead(const uint8_t* head, size_t len, uint16_t chipId);

// Streams an app image to the next OTA slot: holds back the first bytes
// until the header can be checked, so a wrong image is refused before
// anything is written. Destroyed unfinished, it aborts the update.
class ImageWriter {
public:
    ImageWriter(Ota& ota, uint16_t chipId) : ota_(ota), chipId_(chipId) {}
    ~ImageWriter();
    ImageWriter(const ImageWriter&) = delete;
    ImageWriter& operator=(const ImageWriter&) = delete;

    // `size` of the image, 0 if unknown.
    void begin(uint32_t size) { size_ = size; }
    tesser::Check write(const uint8_t* data, size_t len);
    // Verifies the image and makes it boot next.
    tesser::Check finish();
    uint32_t received() const { return received_; }
    uint32_t size() const { return size_; }

private:
    tesser::Check flushHead();

    Ota& ota_;
    uint16_t chipId_;
    uint32_t size_ = 0;
    uint32_t received_ = 0;
    uint8_t head_[kImageHeadSize] = {};
    size_t headLen_ = 0;
    bool open_ = false;
    bool done_ = false;
};

}  // namespace tesserkit
