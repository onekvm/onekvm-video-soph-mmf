#include <onekvm/crypto_backend_v1.h>
#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <sys/ioctl.h>
#include <unistd.h>

static int kernel_caps;
static int queries;
static const char *counter_mode;
extern "C" int __wrap_open(const char *path, int, ...) {
    if (std::strcmp(path, "/dev/onekvm-crypto-offload") == 0) return 123;
    assert(std::strcmp(path, "/sys/module/cvitek_spacc/parameters/ctr_explicit_counters") == 0);
    if (std::strcmp(counter_mode, "missing") == 0) { errno = ENOENT; return -1; }
    return 124;
}
extern "C" int __wrap_ioctl(int fd, unsigned long request, ...) {
    assert(fd == 123 && request == _IOR(0xb7, 0x04, uint32_t));
    ++queries;
    if (kernel_caps < 0) { errno = ENOTTY; return -1; }
    va_list args; va_start(args, request);
    *va_arg(args, uint32_t *) = static_cast<uint32_t>(kernel_caps);
    va_end(args);
    return 0;
}
extern "C" ssize_t __wrap_read(int fd, void *buffer, size_t count) {
    assert(fd == 124 && count == 1);
    if (std::strcmp(counter_mode, "error") == 0) { errno = EIO; return -1; }
    if (std::strcmp(counter_mode, "empty") == 0) return 0;
    *static_cast<char *>(buffer) = counter_mode[0];
    return 1;
}
extern "C" int __wrap_close(int fd) { assert(fd == 123 || fd == 124); return 0; }
int main(int argc, char **argv) {
    assert(argc == 3);
    counter_mode = argv[2];
    kernel_caps = std::atoi(argv[1]);
    const void *raw = nullptr;
    assert(onekvm_crypto_backend_query(ONEKVM_CRYPTO_BACKEND_ABI_V1, &raw) == 0);
    const auto *api = static_cast<const onekvm_crypto_backend_v1 *>(raw);
    assert(queries == 1);
    const bool concurrent = (api->features & ONEKVM_CRYPTO_FEATURE_CONCURRENT_H265_VIDEO) != 0;
    assert(concurrent == (kernel_caps == 15 && std::strcmp(counter_mode, "Y") == 0));
    assert(api->features & ONEKVM_CRYPTO_FEATURE_AES_GCM_TX);
    if (kernel_caps == 15 || kernel_caps == 11) {
        assert(api->features & ONEKVM_CRYPTO_FEATURE_AES_CTR);
        assert(api->features & ONEKVM_CRYPTO_FEATURE_HMAC_SHA1);
        assert(api->features & ONEKVM_CRYPTO_FEATURE_AES_CM_TX_BATCH);
    } else {
        assert(!(api->features & (ONEKVM_CRYPTO_FEATURE_AES_CTR |
            ONEKVM_CRYPTO_FEATURE_HMAC_SHA1 | ONEKVM_CRYPTO_FEATURE_AES_CM_TX_BATCH)));
    }
}
