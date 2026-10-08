#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace onekvm {

// Returned packets can outlive their encoder. Keep this pool in the packet's
// shared owner, and retain the full writable size separately from JPEG length.
class PacketBufferPool {
public:
    explicit PacketBufferPool(size_t size) : size_(size) {}

    std::vector<uint8_t> take() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (count_ != 0)
                return std::move(buffers_[--count_]);
        }
        return std::vector<uint8_t>(size_);
    }

    void put(std::vector<uint8_t> buffer) {
        if (buffer.size() != size_)
            return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (count_ < buffers_.size())
            buffers_[count_++] = std::move(buffer);
    }

private:
    const size_t size_;
    std::mutex mutex_;
    std::array<std::vector<uint8_t>, 3> buffers_;
    size_t count_ = 0;
};

// H26x access units have varying lengths. Preserve the previous length on
// reuse, and bound retained capacity independently of that valid length.
class EncodedPacketPool {
public:
    explicit EncodedPacketPool(size_t maximum_capacity)
        : maximum_capacity_(maximum_capacity) {}

    std::vector<uint8_t> take() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (count_ == 0)
            return {};
        return std::move(buffers_[--count_]);
    }

    void put(std::vector<uint8_t> buffer) {
        if (buffer.capacity() == 0 || buffer.capacity() > maximum_capacity_)
            return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (count_ < buffers_.size())
            buffers_[count_++] = std::move(buffer);
    }

private:
    const size_t maximum_capacity_;
    std::mutex mutex_;
    std::array<std::vector<uint8_t>, 3> buffers_;
    size_t count_ = 0;
};

} // namespace onekvm
