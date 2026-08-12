#include <onekvm/crypto_backend_v1.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <new>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

constexpr const char *kDevicePath = "/dev/onekvm-crypto-offload";
constexpr uint32_t kKernelABIVersion = 1;
constexpr uint32_t kAuthTagSize = 16;
constexpr uint32_t kMaxBatch = 128;
constexpr uint32_t kMaxInput = 1024 * 1024;
constexpr uint32_t kPayloadAlignment = 16;

struct KernelKeyConfig {
    uint32_t version;
    uint32_t key_len;
    uint32_t auth_size;
    uint32_t reserved;
    uint8_t key[32];
};

struct KernelEncryptRequest {
    uint32_t version;
    uint32_t flags;
    uint32_t aad_len;
    uint32_t data_len;
    uint64_t aad_ptr;
    uint64_t src_ptr;
    uint64_t dst_ptr;
    uint8_t iv[12];
    uint8_t reserved[4];
};

struct KernelBatchRequest {
    uint32_t version;
    uint32_t flags;
    uint32_t count;
    uint32_t completed;
    uint64_t requests_ptr;
};

static_assert(sizeof(KernelKeyConfig) == 48, "unexpected key ioctl layout");
static_assert(sizeof(KernelEncryptRequest) == 56, "unexpected encrypt ioctl layout");
static_assert(sizeof(KernelBatchRequest) == 24, "unexpected batch ioctl layout");

/* glibc declares ioctl's request as unsigned long, while musl (used on the
   SG2002 target) declares it as int. Preserve the same 32-bit ioctl bit
   pattern without relying on a warning-prone implicit signed conversion. */
#if defined(__GLIBC__)
using IoctlRequest = unsigned long;
#else
using IoctlRequest = int;
#endif

constexpr IoctlRequest kSetKey =
    static_cast<IoctlRequest>(_IOW(0xb7, 0x01, KernelKeyConfig));
constexpr IoctlRequest kEncrypt =
    static_cast<IoctlRequest>(_IOWR(0xb7, 0x02, KernelEncryptRequest));
constexpr IoctlRequest kEncryptBatch =
    static_cast<IoctlRequest>(_IOWR(0xb7, 0x03, KernelBatchRequest));

struct Session {
    int fd = -1;
    uint32_t auth_tag_size = kAuthTagSize;
};

void set_error(char *error, uint32_t capacity, const char *operation, int code) {
    if (error == nullptr || capacity == 0) return;
    const int positive = code < 0 ? -code : code;
    std::snprintf(error, capacity, "%s: %s", operation, std::strerror(positive));
    error[capacity - 1] = '\0';
}

int32_t fail_errno(char *error, uint32_t capacity, const char *operation) {
    const int code = errno == 0 ? EIO : errno;
    set_error(error, capacity, operation, code);
    return -code;
}

int32_t validate_request(const Session *session, onekvm_crypto_request_v1 *request,
                         char *error, uint32_t error_capacity) {
    if (session == nullptr || request == nullptr ||
        request->struct_size < sizeof(*request) || request->flags != 0) {
        set_error(error, error_capacity, "invalid crypto request", EINVAL);
        return -EINVAL;
    }
    if (request->nonce_size != sizeof(request->nonce) ||
        request->src_size > kMaxInput || request->aad_size > kMaxInput ||
        request->src_size + request->aad_size > kMaxInput ||
        request->dst_capacity < request->src_size + session->auth_tag_size ||
        (request->aad_size != 0 && request->aad_ptr == 0) ||
        (request->src_size != 0 && request->src_ptr == 0) || request->dst_ptr == 0) {
        set_error(error, error_capacity, "unsupported crypto request", EINVAL);
        return -EINVAL;
    }
    request->output_size = 0;
    return 0;
}

KernelEncryptRequest kernel_request(const onekvm_crypto_request_v1 &request) {
    KernelEncryptRequest kernel{};
    kernel.version = kKernelABIVersion;
    kernel.aad_len = request.aad_size;
    kernel.data_len = request.src_size;
    kernel.aad_ptr = request.aad_ptr;
    kernel.src_ptr = request.src_ptr;
    kernel.dst_ptr = request.dst_ptr;
    std::memcpy(kernel.iv, request.nonce, sizeof(kernel.iv));
    return kernel;
}

int32_t probe(char *error, uint32_t error_capacity) {
    const int fd = ::open(kDevicePath, O_RDWR | O_CLOEXEC);
    if (fd < 0) return fail_errno(error, error_capacity, "open crypto offload device");
    if (::close(fd) != 0) return fail_errno(error, error_capacity, "close crypto offload device");
    return 0;
}

int32_t session_create(const uint8_t *key, uint32_t key_size,
                       uint32_t auth_tag_size, void **result,
                       char *error, uint32_t error_capacity) {
    if (result != nullptr) *result = nullptr;
    if (result == nullptr || key == nullptr ||
        (key_size != 16 && key_size != 32) || auth_tag_size != kAuthTagSize) {
        set_error(error, error_capacity, "invalid AES-GCM session", EINVAL);
        return -EINVAL;
    }
    auto *session = new (std::nothrow) Session();
    if (session == nullptr) {
        set_error(error, error_capacity, "allocate AES-GCM session", ENOMEM);
        return -ENOMEM;
    }
    session->fd = ::open(kDevicePath, O_RDWR | O_CLOEXEC);
    if (session->fd < 0) {
        const int result_code = fail_errno(error, error_capacity, "open crypto offload device");
        delete session;
        return result_code;
    }
    KernelKeyConfig config{};
    config.version = kKernelABIVersion;
    config.key_len = key_size;
    config.auth_size = auth_tag_size;
    std::memcpy(config.key, key, key_size);
    if (::ioctl(session->fd, kSetKey, &config) != 0) {
        std::memset(config.key, 0, sizeof(config.key));
        const int result_code = fail_errno(error, error_capacity, "configure crypto offload key");
        ::close(session->fd);
        delete session;
        return result_code;
    }
    std::memset(config.key, 0, sizeof(config.key));
    *result = session;
    return 0;
}

int32_t session_seal(void *opaque, onekvm_crypto_request_v1 *request,
                     char *error, uint32_t error_capacity) {
    auto *session = static_cast<Session *>(opaque);
    const int32_t valid = validate_request(session, request, error, error_capacity);
    if (valid != 0) return valid;
    KernelEncryptRequest kernel = kernel_request(*request);
    if (::ioctl(session->fd, kEncrypt, &kernel) != 0)
        return fail_errno(error, error_capacity, "AES-GCM offload");
    request->output_size = request->src_size + session->auth_tag_size;
    return 0;
}

int32_t session_seal_batch(void *opaque, onekvm_crypto_request_v1 *requests,
                           uint32_t count, uint32_t *completed,
                           char *error, uint32_t error_capacity) {
    auto *session = static_cast<Session *>(opaque);
    if (completed == nullptr || requests == nullptr || count == 0 || count > kMaxBatch) {
        set_error(error, error_capacity, "invalid AES-GCM batch", EINVAL);
        return -EINVAL;
    }
    *completed = 0;
    /* The kernel consumes the array synchronously. A fixed stack buffer avoids
       a heap allocation on every SRTP batch and cannot throw across the C ABI. */
    std::array<KernelEncryptRequest, kMaxBatch> kernel{};
    for (uint32_t index = 0; index < count; ++index) {
        const int32_t valid = validate_request(session, &requests[index], error, error_capacity);
        if (valid != 0) return valid;
        kernel[index] = kernel_request(requests[index]);
    }
    KernelBatchRequest batch{};
    batch.version = kKernelABIVersion;
    batch.count = count;
    batch.requests_ptr = reinterpret_cast<uint64_t>(kernel.data());
    if (::ioctl(session->fd, kEncryptBatch, &batch) != 0) {
        *completed = batch.completed;
        return fail_errno(error, error_capacity, "AES-GCM batch offload");
    }
    *completed = batch.completed;
    for (uint32_t index = 0; index < batch.completed && index < count; ++index)
        requests[index].output_size = requests[index].src_size + session->auth_tag_size;
    if (batch.completed != count) {
        set_error(error, error_capacity, "incomplete AES-GCM batch", EIO);
        return -EIO;
    }
    return 0;
}

void session_destroy(void *opaque) {
    auto *session = static_cast<Session *>(opaque);
    if (session == nullptr) return;
    if (session->fd >= 0) ::close(session->fd);
    delete session;
}

const onekvm_crypto_backend_v1 kBackend = {
    sizeof(onekvm_crypto_backend_v1),
    ONEKVM_CRYPTO_BACKEND_ABI_V1,
    "nanokvm-mmf",
    "nanokvm CryptoDMA AES-GCM",
    // Do not advertise CONCURRENT_H265_VIDEO. SG2002 currently hard-locks
    // under concurrent H.265 VENC and CryptoDMA load, so Core must use its
    // generic AES-GCM path for H.265 sessions.
    ONEKVM_CRYPTO_FEATURE_AES_GCM_TX | ONEKVM_CRYPTO_FEATURE_AES_GCM_TX_BATCH,
    kMaxBatch,
    kMaxInput,
    kPayloadAlignment,
    0,
    probe,
    session_create,
    session_seal,
    session_seal_batch,
    session_destroy,
};

} // namespace

extern "C" ONEKVM_CRYPTO_EXPORT int32_t onekvm_crypto_backend_query(
    uint32_t requested_abi, const void **backend_api) {
    if (backend_api == nullptr)
        return -EINVAL;
    *backend_api = nullptr;
    if (requested_abi != ONEKVM_CRYPTO_BACKEND_ABI_V1)
        return -EINVAL;
    *backend_api = &kBackend;
    return 0;
}
