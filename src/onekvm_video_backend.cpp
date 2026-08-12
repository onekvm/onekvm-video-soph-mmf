#include "onekvm_video_backend_internal.hpp"

extern "C" ONEKVM_VIDEO_EXPORT int32_t onekvm_video_backend_query(
    uint32_t requested_abi, const void **backend_api)
{
    if (backend_api == nullptr) {
        return -1;
    }
    *backend_api = nullptr;
    if (requested_abi != ONEKVM_VIDEO_BACKEND_ABI_V1) {
        return -1;
    }
    *backend_api = &onekvm::video_backend::kBackend;
    return 0;
}

