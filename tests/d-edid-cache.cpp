// Compile the actual EDID implementation so cache regressions cannot be
// hidden by a test replacement for lt6911_edid_read.
#include "../src/lt6911_edid.cpp"
#include <cassert>

static int cached_chip = ONEKVM_LT6911_D;
static unsigned open_calls;
static bool locked;
extern "C" {
int onekvm_lt6911_cached_chip(void) { return cached_chip; }
void onekvm_lt6911_cache_chip(int chip) { cached_chip = chip; }
void onekvm_lt6911_i2c_lock(void) { assert(!locked); locked = true; }
void onekvm_lt6911_i2c_unlock(void) { assert(locked); locked = false; }
int __wrap_open(const char *, int, ...) { ++open_calls; errno = EIO; return -1; }
}
int main() {
    using namespace onekvm::video_backend;
    uint8_t data[256]{};
    g_active_valid = false;
    assert(lt6911_edid_read(data, sizeof(data)) == -EAGAIN && !open_calls);
    for (unsigned i=0;i<256;++i) g_active_edid[i]=i;
    g_active_valid = true;
    assert(!lt6911_edid_read(data, sizeof(data)) && !open_calls);
    assert(!std::memcmp(data, g_active_edid, sizeof(data)));
    invalidate_edid_after_reset(true);
    assert(g_active_valid); // A successful D reset loads its verified Flash.
    invalidate_edid_after_reset(false);
    assert(!g_active_valid);
    assert(lt6911_edid_read(data, sizeof(data)) == -EAGAIN && !open_calls);
    cached_chip = ONEKVM_LT6911_UXC; g_active_valid = true;
    invalidate_edid_after_reset(true);
    assert(!g_active_valid); // UXC may have had a temporary RAM override.
    cached_chip = ONEKVM_LT6911_C; g_active_valid = true;
    invalidate_edid_after_reset(true);
    assert(!g_active_valid);
    puts("D EDID GET: cache hit and miss perform no device open; reset invalidation follows silicon");
}
