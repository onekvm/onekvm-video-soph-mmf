#include "packet-buffer-pool.hpp"

#include <memory>

int main()
{
    auto encoder_pool = std::make_shared<onekvm::PacketBufferPool>(4096);
    auto retained_owner = encoder_pool;
    auto first = encoder_pool->take();
    first[0] = 0x42;
    auto *address = first.data();
    auto concurrent = encoder_pool->take();
    if (concurrent.data() == address || first[0] != 0x42)
        return 1;
    encoder_pool->put(std::move(concurrent));
    encoder_pool->put(std::move(first));
    auto reused = encoder_pool->take();
    if (reused.data() != address || reused.size() != 4096 || reused[0] != 0x42)
        return 2;

    // A downstream packet may be freed after the encoder was destroyed.
    encoder_pool.reset();
    retained_owner->put(std::move(reused));
    auto after_teardown = retained_owner->take();
    if (after_teardown.data() != address || after_teardown[0] != 0x42)
        return 3;

    // A short JPEG payload must never reduce the next writable buffer size.
    after_teardown.resize(7);
    retained_owner->put(std::move(after_teardown));
    if (retained_owner->take().size() != 4096)
        return 4;

    auto reader_pool = std::make_shared<onekvm::EncodedPacketPool>(4096);
    auto downstream_pool = reader_pool;
    auto access_unit = reader_pool->take();
    access_unit.resize(1500, 0x73);
    auto *access_unit_address = access_unit.data();
    // Simulate Core retaining one AU while the reader produces another.
    auto next_access_unit = reader_pool->take();
    next_access_unit.resize(900, 0x51);
    if (next_access_unit.data() == access_unit_address || access_unit[1499] != 0x73)
        return 5;
    reader_pool->put(std::move(next_access_unit));
    // Sending completes after the reader/encoder is destroyed.
    reader_pool.reset();
    downstream_pool->put(std::move(access_unit));
    auto next = downstream_pool->take();
    if (next.data() != access_unit_address || next.size() != 1500 || next[1499] != 0x73)
        return 6;
    next.resize(100);
    downstream_pool->put(std::move(next));
    if (downstream_pool->take().size() != 100)
        return 7;
    // Do not retain unusually large parameter-prefixed packets indefinitely.
    downstream_pool->put(std::vector<uint8_t>(8192));
    downstream_pool->take(); // the earlier 900-byte AU
    if (!downstream_pool->take().empty())
        return 8;
    return 0;
}
