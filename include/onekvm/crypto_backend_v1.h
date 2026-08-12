#ifndef ONEKVM_CRYPTO_BACKEND_V1_H
#define ONEKVM_CRYPTO_BACKEND_V1_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * OneKVM in-process SRTP TX crypto backend ABI v1.
 *
 * The ABI is intentionally independent from Go and the Linux kernel ioctl.
 * Structures are append-only and every request carries struct_size. Pointer
 * fields are integer process addresses so cgo callers can construct request
 * arrays without storing Go pointers in C-owned memory.
 */
#define ONEKVM_CRYPTO_BACKEND_ABI_V1 1u
#define ONEKVM_CRYPTO_BACKEND_QUERY_SYMBOL "onekvm_crypto_backend_query"

#if defined(__GNUC__)
#define ONEKVM_CRYPTO_EXPORT __attribute__((visibility("default")))
#else
#define ONEKVM_CRYPTO_EXPORT
#endif

#define ONEKVM_CRYPTO_FEATURE_AES_GCM_TX       (1ull << 0)
#define ONEKVM_CRYPTO_FEATURE_AES_GCM_TX_BATCH (1ull << 1)
/* Safe to run CryptoDMA concurrently with a machine H.265 video encoder. */
#define ONEKVM_CRYPTO_FEATURE_CONCURRENT_H265_VIDEO (1ull << 2)

struct onekvm_crypto_request_v1 {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t aad_ptr;
    uint64_t src_ptr;
    uint64_t dst_ptr;
    uint32_t aad_size;
    uint32_t src_size;
    uint32_t dst_capacity;
    uint32_t output_size;
    uint8_t nonce[12];
    uint32_t nonce_size;
};

struct onekvm_crypto_backend_v1 {
    uint32_t struct_size;
    uint32_t abi_version;
    const char *driver_id;
    const char *backend_name;
    uint64_t features;
    uint32_t max_batch;
    uint32_t max_input;
    uint32_t payload_alignment;
    uint32_t reserved;

    int32_t (*probe)(char *error, uint32_t error_capacity);
    int32_t (*session_create)(
        const uint8_t *key, uint32_t key_size, uint32_t auth_tag_size,
        void **session, char *error, uint32_t error_capacity);
    int32_t (*session_seal)(
        void *session, struct onekvm_crypto_request_v1 *request,
        char *error, uint32_t error_capacity);
    int32_t (*session_seal_batch)(
        void *session, struct onekvm_crypto_request_v1 *requests,
        uint32_t count, uint32_t *completed,
        char *error, uint32_t error_capacity);
    void (*session_destroy)(void *session);
};

typedef int32_t (*onekvm_crypto_backend_query_fn)(
    uint32_t requested_abi, const void **backend_api);

ONEKVM_CRYPTO_EXPORT int32_t onekvm_crypto_backend_query(
    uint32_t requested_abi, const void **backend_api);

#ifdef __cplusplus
}
#endif

#endif
