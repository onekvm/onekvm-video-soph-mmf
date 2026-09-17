#include "mmf_internal.hpp"

namespace onekvm::mmf {
namespace {

constexpr int kMinJpegQuality = 51;
constexpr int kMaxJpegQuality = 100;

int set_jpeg_quality(int channel, int quality)
{
	VENC_JPEG_PARAM_S parameters{};
	CVI_S32 result = CVI_VENC_GetJpegParam(channel, &parameters);
	if (result != CVI_SUCCESS)
		return result;
	parameters.u32Qfactor = quality;
	result = CVI_VENC_SetJpegParam(channel, &parameters);
	if (result == CVI_SUCCESS)
		g_runtime.jpeg_quality = quality;
	return result;
}

void destroy_jpeg_channel(int channel)
{
	CVI_VENC_StopRecvFrame(channel);
	CVI_VENC_ResetChn(channel);
	CVI_VENC_DestroyChn(channel);
}

int create_jpeg_channel(int channel, int width, int height, int quality)
{
	VENC_CHN_ATTR_S attributes{};
	attributes.stVencAttr.enType = PT_JPEG;
	attributes.stVencAttr.u32MaxPicWidth = width;
	attributes.stVencAttr.u32MaxPicHeight = height;
	attributes.stVencAttr.u32PicWidth = width;
	attributes.stVencAttr.u32PicHeight = height;
	attributes.stVencAttr.bEsBufQueueEn = CVI_FALSE;
	attributes.stVencAttr.bIsoSendFrmEn = CVI_FALSE;
	attributes.stVencAttr.bByFrame = CVI_TRUE;
	attributes.stRcAttr.enRcMode = VENC_RC_MODE_MJPEGFIXQP;

	/* Keep the vendor reset/recreate sequence used by NanoKVM. Without it,
	 * some SG2002 driver revisions occasionally leave JPEG channel 0 stale
	 * after a process restart. */
	CVI_S32 result = CVI_VENC_CreateChn(channel, &attributes);
	if (result != CVI_SUCCESS)
		return result;
	result = CVI_VENC_ResetChn(channel);
	if (result != CVI_SUCCESS) {
		CVI_VENC_DestroyChn(channel);
		return result;
	}
	result = CVI_VENC_DestroyChn(channel);
	if (result != CVI_SUCCESS)
		return result;
	result = CVI_VENC_CreateChn(channel, &attributes);
	if (result != CVI_SUCCESS)
		return result;

	result = set_jpeg_quality(channel, quality);
	if (result != CVI_SUCCESS) {
		CVI_VENC_DestroyChn(channel);
		return result;
	}

	VENC_RECV_PIC_PARAM_S receive_parameters{};
	receive_parameters.s32RecvPicNum = -1;
	result = CVI_VENC_StartRecvFrame(channel, &receive_parameters);
	if (result != CVI_SUCCESS) {
		destroy_jpeg_channel(channel);
		return result;
	}
	return CVI_SUCCESS;
}

int ensure_jpeg_staging_frame(int width, int height)
{
	if (g_runtime.jpeg_staging_frame != nullptr)
		return 0;

	const uint32_t size = COMMON_GetPicBufferSize(
		width, height, PIXEL_FORMAT_NV21, DATA_BITWIDTH_8,
		COMPRESS_MODE_NONE, DEFAULT_ALIGN);
	const int pool_id = _create_vb_pool(
		"onekvm_jpeg", size, 1);
	if (pool_id < 0)
		return pool_id;

	VIDEO_FRAME_INFO_S *frame = allocate_frame(
		pool_id, SIZE_S{static_cast<CVI_U32>(width),
			static_cast<CVI_U32>(height)}, PIXEL_FORMAT_NV21);
	if (frame == nullptr) {
		_destroy_vb_pool(pool_id);
		return CVI_FAILURE;
	}
	g_runtime.jpeg_staging_pool_id = pool_id;
	g_runtime.jpeg_staging_frame = frame;
	return CVI_SUCCESS;
}

void release_invalid_jpeg_stream(int channel)
{
	CVI_VENC_ReleaseStream(channel, &g_runtime.jpeg_stream);
	g_runtime.jpeg_stream_held = false;
	g_runtime.jpeg_stream.u32PackCount = 0;
	g_runtime.jpeg_stream.pstPack = g_runtime.jpeg_packs;
}

} // namespace

int open_jpeg_encoder(int channel, int width, int height, int pixel_format,
	int quality)
{
	if (channel < 0 || channel >= MMF_VENC_MAX_CHN || width <= 0 || height <= 0 ||
		(width & 1) != 0 || (height & 1) != 0 ||
		pixel_format != PIXEL_FORMAT_NV21 || quality < kMinJpegQuality ||
		quality > kMaxJpegQuality || g_runtime.reference_count == 0)
		return -1;

	if (g_runtime.jpeg_initialized) {
		if (g_runtime.jpeg_width != width || g_runtime.jpeg_height != height ||
			g_runtime.jpeg_pixel_format != pixel_format) {
			if (close_jpeg_encoder(channel) != 0)
				return -1;
		} else {
			return quality == g_runtime.jpeg_quality
				? 0 : set_jpeg_quality(channel, quality);
		}
	}

	const int result = create_jpeg_channel(channel, width, height, quality);
	if (result != CVI_SUCCESS)
		return result;

	g_runtime.jpeg_initialized = true;
	g_runtime.jpeg_frame_pending = false;
	g_runtime.jpeg_stream_held = false;
	g_runtime.jpeg_stream = {};
	g_runtime.jpeg_stream.pstPack = g_runtime.jpeg_packs;
	g_runtime.jpeg_width = width;
	g_runtime.jpeg_height = height;
	g_runtime.jpeg_pixel_format = pixel_format;
	g_runtime.jpeg_quality = quality;
	g_runtime.jpeg_staging_frame = nullptr;
	g_runtime.jpeg_staging_pool_id = -1;
	return CVI_SUCCESS;
}

int close_jpeg_encoder(int channel)
{
	if (!g_runtime.jpeg_initialized)
		return 0;
	if (channel < 0 || channel >= MMF_VENC_MAX_CHN)
		return -1;

	if (g_runtime.jpeg_stream_held)
		release_jpeg_packet(channel);

	CVI_S32 first_error = CVI_SUCCESS;
	CVI_S32 result = CVI_VENC_StopRecvFrame(channel);
	if (result != CVI_SUCCESS)
		first_error = result;
	result = CVI_VENC_ResetChn(channel);
	if (result != CVI_SUCCESS && first_error == CVI_SUCCESS)
		first_error = result;
	result = CVI_VENC_DestroyChn(channel);
	if (result != CVI_SUCCESS && first_error == CVI_SUCCESS)
		first_error = result;

	if (g_runtime.jpeg_staging_frame != nullptr)
		free_frame(g_runtime.jpeg_staging_frame);
	if (g_runtime.jpeg_staging_pool_id >= 0)
		_destroy_vb_pool(g_runtime.jpeg_staging_pool_id);

	g_runtime.jpeg_initialized = false;
	g_runtime.jpeg_frame_pending = false;
	g_runtime.jpeg_stream_held = false;
	g_runtime.jpeg_stream = {};
	g_runtime.jpeg_width = 0;
	g_runtime.jpeg_height = 0;
	g_runtime.jpeg_pixel_format = 0;
	g_runtime.jpeg_quality = 0;
	g_runtime.jpeg_staging_frame = nullptr;
	g_runtime.jpeg_staging_pool_id = -1;
	return first_error;
}

int submit_jpeg_frame_timeout(int channel, uint8_t *data, int width, int height,
	int pixel_format, int quality, int timeout_ms)
{
	if (data == nullptr || pixel_format != PIXEL_FORMAT_NV21)
		return -1;
	if (!g_runtime.jpeg_initialized || g_runtime.jpeg_width != width ||
		g_runtime.jpeg_height != height ||
		g_runtime.jpeg_pixel_format != pixel_format) {
		if (g_runtime.jpeg_initialized && close_jpeg_encoder(channel) != 0)
			return -1;
		const int result = open_jpeg_encoder(
			channel, width, height, pixel_format, quality);
		if (result != CVI_SUCCESS)
			return result;
	} else if (quality != g_runtime.jpeg_quality) {
		const int result = set_jpeg_quality(channel, quality);
		if (result != CVI_SUCCESS)
			return result;
	}
	if (g_runtime.jpeg_frame_pending || g_runtime.jpeg_stream_held)
		return -EBUSY;

	VIDEO_FRAME_INFO_S *send_frame = find_capture_frame(
		data, width, height, pixel_format);
	if (send_frame == nullptr) {
		if (ensure_jpeg_staging_frame(width, height) != CVI_SUCCESS)
			return -1;
		send_frame = g_runtime.jpeg_staging_frame;
		const CVI_U32 stride = send_frame->stVFrame.u32Stride[0];
		if (stride != static_cast<CVI_U32>(width)) {
			for (int row = 0; row < height * 3 / 2; ++row) {
				memcpy(send_frame->stVFrame.pu8VirAddr[0] + stride * row,
					data + width * row, width);
			}
		} else {
			memcpy(send_frame->stVFrame.pu8VirAddr[0], data,
				static_cast<size_t>(width) * height * 3 / 2);
		}
		CVI_SYS_IonFlushCache(send_frame->stVFrame.u64PhyAddr[0],
			send_frame->stVFrame.pu8VirAddr[0],
			frame_buffer_size(&send_frame->stVFrame));
	}

	const CVI_S32 result = CVI_VENC_SendFrame(channel, send_frame, timeout_ms);
	if (result == CVI_SUCCESS)
		g_runtime.jpeg_frame_pending = true;
	return result;
}

int submit_jpeg_frame(int channel, uint8_t *data, int width, int height,
	int pixel_format, int quality)
{
	return submit_jpeg_frame_timeout(
		channel, data, width, height, pixel_format, quality, 1000);
}

int read_jpeg_packet_timeout(int channel, uint8_t *destination, int capacity,
	int timeout_ms)
{
	if (destination == nullptr || capacity <= 0 ||
		!g_runtime.jpeg_frame_pending || g_runtime.jpeg_stream_held)
		return -1;

	VENC_CHN_STATUS_S status{};
	int elapsed_ms = 0;
	for (;; ++elapsed_ms) {
		const CVI_S32 result = CVI_VENC_QueryStatus(channel, &status);
		if (result != CVI_SUCCESS ||
			status.u32CurPacks > MMF_VENC_INTERNAL_PACKS)
			return -1;
		if (status.u32CurPacks > 0)
			break;
		if (elapsed_ms >= timeout_ms)
			return -1;
		usleep(1000);
	}

	memset(g_runtime.jpeg_packs, 0, sizeof(g_runtime.jpeg_packs));
	g_runtime.jpeg_stream.pstPack = g_runtime.jpeg_packs;
	const int stream_timeout_ms = timeout_ms - elapsed_ms;
	if (stream_timeout_ms <= 0)
		return -1;
	CVI_S32 result = CVI_VENC_GetStream(
		channel, &g_runtime.jpeg_stream, stream_timeout_ms);
	if (result != CVI_SUCCESS)
		return -1;
	g_runtime.jpeg_frame_pending = false;
	g_runtime.jpeg_stream_held = true;
	if (g_runtime.jpeg_stream.u32PackCount == 0 ||
		g_runtime.jpeg_stream.u32PackCount > MMF_VENC_INTERNAL_PACKS) {
		release_invalid_jpeg_stream(channel);
		return -1;
	}

	CVI_U32 total = 0;
	for (CVI_U32 index = 0;
		 index < g_runtime.jpeg_stream.u32PackCount; ++index) {
		VENC_PACK_S *pack = &g_runtime.jpeg_stream.pstPack[index];
		if (pack->u32Offset > pack->u32Len) {
			release_invalid_jpeg_stream(channel);
			return -1;
		}
		const CVI_U32 size = pack->u32Len - pack->u32Offset;
		if (size > static_cast<CVI_U32>(capacity) - total) {
			release_invalid_jpeg_stream(channel);
			return -2;
		}
		if (size > 0) {
			if (pack->pu8Addr == nullptr) {
				release_invalid_jpeg_stream(channel);
				return -1;
			}
			memcpy(destination + total, pack->pu8Addr + pack->u32Offset, size);
		}
		total += size;
	}
	return static_cast<int>(total);
}

int read_jpeg_packet(int channel, uint8_t *destination, int capacity)
{
	return read_jpeg_packet_timeout(channel, destination, capacity, 1000);
}

int release_jpeg_packet(int channel)
{
	if (!g_runtime.jpeg_stream_held)
		return 0;
	const CVI_S32 result = CVI_VENC_ReleaseStream(
		channel, &g_runtime.jpeg_stream);
	if (result != CVI_SUCCESS)
		return result;
	g_runtime.jpeg_stream_held = false;
	g_runtime.jpeg_stream.u32PackCount = 0;
	g_runtime.jpeg_stream.pstPack = g_runtime.jpeg_packs;
	return CVI_SUCCESS;
}

} // namespace onekvm::mmf
