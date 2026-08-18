#include "mmf_internal.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace onekvm::mmf {

struct H26xReader {
	std::thread thread;
	std::atomic<bool> stop{false};
	std::atomic<bool> running{false};
	std::atomic<uint64_t> last_packet_ns{0};
	std::mutex mu;
	std::vector<uint8_t> packet;
};

static H26xReader g_readers[MMF_VENC_MAX_CHN];

static uint64_t reader_now_ns()
{
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}
static int _validate_venc_cfg(int ch, const H26xEncoderConfig *cfg)
{
	if (ch < 0 || ch >= MMF_VENC_MAX_CHN || cfg == NULL) {
		printf("Invalid venc channel or config, ch:%d\r\n", ch);
		return -1;
	}
	if (g_runtime.reference_count == 0)
		return -1;
	/* Let the ABI allocator distinguish an occupied hardware channel from an
	 * invalid encoder configuration and continue with the next channel. */
	if (g_runtime.h26x_encoders[ch].initialized)
		return -EBUSY;
	/* SG2002's vendor VENC driver exposes several channel IDs, but running two
	 * H.26x channels concurrently (in particular H.264 plus H.265) deadlocks
	 * inside the kernel driver. JPEG uses its separate path and remains safe to
	 * run alongside one H.26x channel. Fail a second H.26x encoder instead of
	 * allowing a device-wide media lockup. */
	if (g_runtime.h26x_encoder_active)
		return -ENOSPC;
	if (cfg->codec != H26xCodec::H265 && cfg->codec != H26xCodec::H264) {
		printf("Unsupported venc codec type:%d\r\n", static_cast<int>(cfg->codec));
		return -1;
	}
	if (cfg->width <= 0 || cfg->height <= 0 || (cfg->width & 1) || (cfg->height & 1) ||
		cfg->gop <= 0 || cfg->input_fps <= 0 || cfg->output_fps <= 0 ||
		cfg->output_fps > cfg->input_fps || cfg->bitrate_kbps <= 0) {
		printf("Invalid venc config: %dx%d, gop:%d, fps:%d/%d, bitrate:%d\r\n",
			cfg->width, cfg->height, cfg->gop, cfg->input_fps, cfg->output_fps, cfg->bitrate_kbps);
		return -1;
	}
	return 0;
}

static void _set_venc_common_attr(VENC_CHN_ATTR_S *attr, const H26xEncoderConfig *cfg)
{
	memset(attr, 0, sizeof(*attr));
	attr->stVencAttr.enType = cfg->codec == H26xCodec::H265 ? PT_H265 : PT_H264;
	attr->stVencAttr.u32MaxPicWidth = cfg->width;
	attr->stVencAttr.u32MaxPicHeight = cfg->height;
	attr->stVencAttr.u32BufSize = MMF_VENC_STREAM_BUF_SIZE;
	attr->stVencAttr.bByFrame = CVI_TRUE;
	attr->stVencAttr.u32PicWidth = cfg->width;
	attr->stVencAttr.u32PicHeight = cfg->height;
	if (cfg->codec == H26xCodec::H264)
		attr->stVencAttr.u32Profile = H264E_PROFILE_BASELINE;
	attr->stVencAttr.bEsBufQueueEn = CVI_TRUE;
	/* Isolate SendFrame/GetStream only when userspace pairs them. The bound
	   VPSS→VENC path never calls SendFrame; leaving this on encodes frames
	   (EncodedFrame++) but never queues them for GetStream (LeftFrm=0). */
	attr->stVencAttr.bIsoSendFrmEn = CVI_FALSE;
	attr->stGopAttr.enGopMode = VENC_GOPMODE_NORMALP;
	attr->stGopAttr.stNormalP.s32IPQpDelta = 2;
}

static void _set_h264_vbr_attr(VENC_CHN_ATTR_S *attr, const H26xEncoderConfig *cfg)
{
	attr->stRcAttr.enRcMode = VENC_RC_MODE_H264VBR;
	attr->stRcAttr.stH264Vbr.u32Gop = cfg->gop;
	attr->stRcAttr.stH264Vbr.u32StatTime = 2;
	attr->stRcAttr.stH264Vbr.u32SrcFrameRate = cfg->input_fps;
	attr->stRcAttr.stH264Vbr.fr32DstFrameRate = cfg->output_fps;
	attr->stRcAttr.stH264Vbr.u32MaxBitRate = cfg->bitrate_kbps;
	attr->stRcAttr.stH264Vbr.bVariFpsEn = CVI_FALSE;
}

static void _set_h265_vbr_attr(VENC_CHN_ATTR_S *attr, const H26xEncoderConfig *cfg)
{
	attr->stRcAttr.enRcMode = VENC_RC_MODE_H265VBR;
	attr->stRcAttr.stH265Vbr.u32Gop = cfg->gop;
	attr->stRcAttr.stH265Vbr.u32StatTime = 2;
	attr->stRcAttr.stH265Vbr.u32SrcFrameRate = cfg->input_fps;
	attr->stRcAttr.stH265Vbr.fr32DstFrameRate = cfg->output_fps;
	attr->stRcAttr.stH265Vbr.u32MaxBitRate = cfg->bitrate_kbps;
	attr->stRcAttr.stH265Vbr.bVariFpsEn = CVI_FALSE;
}

static CVI_S32 _set_venc_rc_param(int ch, const H26xEncoderConfig *cfg,
	const RateControl *rc)
{
	CVI_S32 ret;
	VENC_RC_PARAM_S param;
	const CVI_U32 min_qp = rc != NULL && rc->min_qp > 0
		? (CVI_U32)rc->min_qp : MMF_VENC_DEFAULT_MIN_QP;
	const CVI_U32 max_qp = rc != NULL && rc->max_qp > 0
		? (CVI_U32)rc->max_qp : MMF_VENC_DEFAULT_MAX_QP;
	const CVI_U32 initial_qp = rc != NULL && rc->initial_qp > 0
		? (CVI_U32)rc->initial_qp : MMF_VENC_DEFAULT_INITIAL_QP;
	if (min_qp > max_qp) {
		printf("Invalid effective VENC QP range: %u-%u\n", min_qp, max_qp);
		return CVI_FAILURE;
	}
	ret = CVI_VENC_GetRcParam(ch, &param);
	if (ret != CVI_SUCCESS) {
		printf("CVI_VENC_GetRcParam failed with %d\n", ret);
		return ret;
	}

	/* Keep NanoKVM's original QP defaults unless the caller explicitly opts in
	 * to advanced QP overrides.  Image-quality presets only change bitrate. */
	param.s32FirstFrameStartQp = initial_qp;
	param.s32InitialDelay = 1000;
	if (cfg->codec == H26xCodec::H265) {
		param.stParamH265Vbr.s32ChangePos = 90;
		param.stParamH265Vbr.u32MinIprop = 1;
		param.stParamH265Vbr.u32MaxIprop = 100;
		param.stParamH265Vbr.s32MaxReEncodeTimes = 0;
		param.stParamH265Vbr.u32MaxQp = max_qp;
		param.stParamH265Vbr.u32MinQp = min_qp;
		param.stParamH265Vbr.u32MaxIQp = max_qp;
		param.stParamH265Vbr.u32MinIQp = min_qp;
	} else {
		param.stParamH264Vbr.s32ChangePos = 90;
		param.stParamH264Vbr.u32MinIprop = 1;
		param.stParamH264Vbr.u32MaxIprop = 100;
		param.stParamH264Vbr.s32MaxReEncodeTimes = 0;
		param.stParamH264Vbr.u32MaxQp = max_qp;
		param.stParamH264Vbr.u32MinQp = min_qp;
		param.stParamH264Vbr.u32MaxIQp = max_qp;
		param.stParamH264Vbr.u32MinIQp = min_qp;
	}

	ret = CVI_VENC_SetRcParam(ch, &param);
	if (ret != CVI_SUCCESS)
		printf("CVI_VENC_SetRcParam failed with %d\n", ret);
	return ret;
}

static CVI_S32 _set_h264_entropy(int ch)
{
	VENC_H264_ENTROPY_S entropy;
	memset(&entropy, 0, sizeof(entropy));
	entropy.u32EntropyEncModeI = H264E_ENTROPY_CAVLC;
	entropy.u32EntropyEncModeP = H264E_ENTROPY_CAVLC;
	entropy.u32EntropyEncModeB = H264E_ENTROPY_CAVLC;
	CVI_S32 ret = CVI_VENC_SetH264Entropy(ch, &entropy);
	if (ret != CVI_SUCCESS)
		printf("CVI_VENC_SetH264Entropy failed with %d\n", ret);
	return ret;
}

static int create_h26x_channel(int ch, const H26xEncoderConfig &config,
	const RateControl &rate_control)
{
	const H26xEncoderConfig *cfg = &config;
	const RateControl *rc = &rate_control;
	const int validation = _validate_venc_cfg(ch, cfg);
	if (validation != 0)
		return validation;
	if (rc->initial_qp < 0 || rc->initial_qp > 51 ||
		rc->min_qp < 0 || rc->min_qp > 51 ||
		rc->max_qp < 0 || rc->max_qp > 51 ||
		(rc->min_qp > 0 && rc->max_qp > 0 && rc->min_qp > rc->max_qp)) {
		printf("Invalid custom VENC RC config\n");
		return -1;
	}

	VENC_CHN_ATTR_S attr;
	H26xEncoderState *info = (H26xEncoderState *)&g_runtime.h26x_encoders[ch];
	_set_venc_common_attr(&attr, cfg);
	if (cfg->codec == H26xCodec::H265)
		_set_h265_vbr_attr(&attr, cfg);
	else
		_set_h264_vbr_attr(&attr, cfg);

	CVI_S32 ret = CVI_VENC_CreateChn(ch, &attr);
	if (ret != CVI_SUCCESS) {
		printf("CVI_VENC_CreateChn [%d] failed with %d\n", ch, ret);
		return ret;
	}
	if ((ret = _set_venc_rc_param(ch, cfg, rc)) != CVI_SUCCESS)
		goto fail_channel;
	if (cfg->codec == H26xCodec::H264 &&
		(ret = _set_h264_entropy(ch)) != CVI_SUCCESS)
		goto fail_channel;

	memset(info, 0, sizeof(*info));
	info->fd = -1;
	info->ch = ch;
	info->codec = cfg->codec;
	info->staging_pool_id = (uint32_t)-1;
	info->staging_frame = NULL;
	info->stream.pstPack = info->packs;
	memcpy(&info->cfg, cfg, sizeof(*cfg));
	info->initialized = 1;
	info->receiver_started = 0;
	g_runtime.h26x_encoder_active = 1;
	return CVI_SUCCESS;

fail_channel:
	CVI_VENC_DestroyChn(ch);
	return ret;
}

int open_h26x_encoder(int ch, const H26xEncoderConfig &config,
	const RateControl &rate_control)
{
	return create_h26x_channel(ch, config, rate_control);
}

int close_h26x_encoder(int ch) {
	if (ch < 0 || ch >= MMF_VENC_MAX_CHN)
		return -1;
	stop_h26x_reader(ch);
	if (!g_runtime.h26x_encoders[ch].initialized) {
		return 0;
	}
	if (g_runtime.h26x_encoders[ch].bound_to_capture)
		unbind_h26x_from_capture(ch);

	if (g_runtime.h26x_encoders[ch].stream_held)
		release_h26x_packet(ch);

	CVI_S32 s32Ret = CVI_SUCCESS;
	if (g_runtime.h26x_encoders[ch].receiver_started) {
		s32Ret = CVI_VENC_StopRecvFrame(ch);
		if (s32Ret != CVI_SUCCESS)
			printf("CVI_VENC_StopRecvPic failed with %d\n", s32Ret);
		else
			g_runtime.h26x_encoders[ch].receiver_started = 0;
	}

	s32Ret = CVI_VENC_ResetChn(ch);
	if (s32Ret != CVI_SUCCESS) {
		printf("CVI_VENC_ResetChn vechn[%d] failed with %#x!\n", ch, s32Ret);
	}

	s32Ret = CVI_VENC_DestroyChn(ch);
	if (s32Ret != CVI_SUCCESS) {
		printf("CVI_VENC_DestroyChn [%d] failed with %d\n", ch, s32Ret);
	}

	if (g_runtime.h26x_encoders[ch].staging_frame) {
		free_frame(g_runtime.h26x_encoders[ch].staging_frame);
		g_runtime.h26x_encoders[ch].staging_frame = NULL;
	}
	if (g_runtime.h26x_encoders[ch].staging_pool_id != (uint32_t)-1) {
		_destroy_vb_pool(g_runtime.h26x_encoders[ch].staging_pool_id);
		g_runtime.h26x_encoders[ch].staging_pool_id = (uint32_t)-1;
	}

	if (g_runtime.h26x_encoders[ch].codec == H26xCodec::H264 ||
		g_runtime.h26x_encoders[ch].codec == H26xCodec::H265) {
		g_runtime.h26x_encoder_active = 0;
	}
	g_runtime.h26x_encoders[ch].initialized = 0;
	g_runtime.h26x_encoders[ch].fd = -1;

	return 0;
}

int close_all_h26x_encoders() {
	for (int i = 0; i < MMF_VENC_MAX_CHN; i ++) {
		close_h26x_encoder(i);
	}
	return 0;
}

VIDEO_FRAME_INFO_S *find_capture_frame(uint8_t *data, int w, int h, int format)
{
	for (int vi_ch = 0; vi_ch < MMF_VI_MAX_CHN; ++vi_ch) {
		VIDEO_FRAME_INFO_S *frame = &g_runtime.vi_frame[vi_ch];
		if (!g_runtime.vi_chn_is_inited[vi_ch] || frame->stVFrame.pu8VirAddr[0] != data)
			continue;
		if ((int)frame->stVFrame.u32Width == w && (int)frame->stVFrame.u32Height == h &&
			(int)frame->stVFrame.enPixelFormat == format)
			return frame;
	}
	return NULL;
}

static int ensure_h26x_staging_frame(H26xEncoderState *info)
{
	if (info->staging_frame != NULL)
		return 0;
	char name[20];
	snprintf(name, sizeof(name), "venc%.1d", info->ch);
	uint32_t size = VENC_GetPicBufferSize(info->cfg.width, info->cfg.height,
		(PIXEL_FORMAT_E)info->cfg.pixel_format, DATA_BITWIDTH_8, COMPRESS_MODE_NONE);
	int pool_id = _create_vb_pool(name, size, 1);
	if (pool_id < 0) {
		printf("[%s][%d]_create_vb_pool failed, id %d\n", __func__, __LINE__, pool_id);
		return CVI_FAILURE;
	}
	VIDEO_FRAME_INFO_S *frame = allocate_frame(
		pool_id, SIZE_S{static_cast<CVI_U32>(info->cfg.width),
			static_cast<CVI_U32>(info->cfg.height)},
		(PIXEL_FORMAT_E)info->cfg.pixel_format);
	if (frame == NULL) {
		printf("Alloc VENC staging frame failed!\r\n");
		_destroy_vb_pool(pool_id);
		return CVI_FAILURE;
	}
	info->staging_pool_id = (uint32_t)pool_id;
	info->staging_frame = frame;
	return 0;
}

int submit_h26x_frame(int ch, uint8_t *data, int w, int h, int format) {
	int res = 0;
	CVI_S32 s32Ret = CVI_SUCCESS;
	if (ch < 0 || ch >= MMF_VENC_MAX_CHN || data == NULL || format != PIXEL_FORMAT_NV21) {
		printf("Invalid param. ch:%d data:%p format:%d\r\n", ch, data, format);
		return -1;
	}

	H26xEncoderState *info = (H26xEncoderState *)&g_runtime.h26x_encoders[ch];
	if (!info->initialized || w != info->cfg.width || h != info->cfg.height || format != info->cfg.pixel_format) {
		printf("Venc frame does not match channel config: %dx%d fmt:%d\r\n", w, h, format);
		return -1;
	}
	// Keep at most one frame in flight. If the previous pop timed out, the
	// next pop should drain that frame instead of building an ever-growing
	// latency queue with stale desktop frames.
	if (info->packet_pending)
		return 0;
	if (ensure_h26x_staging_frame(info) != 0)
		return -1;
	if (!info->receiver_started) {
		VENC_RECV_PIC_PARAM_S recv_param;
		recv_param.s32RecvPicNum = -1;
		s32Ret = CVI_VENC_StartRecvFrame(ch, &recv_param);
		if (s32Ret != CVI_SUCCESS) {
			printf("CVI_VENC_StartRecvPic failed with %#x\n", s32Ret);
			return s32Ret;
		}
		info->receiver_started = 1;
	}
	VIDEO_FRAME_INFO_S *frame_info = (VIDEO_FRAME_INFO_S *)info->staging_frame;
	if (frame_info == NULL) {
		printf("frame info is null!\r\n");
		return -1;
	}

	/* H.26x deliberately uses its existing private staging block. Passing the
	 * borrowed VPSS block directly to asynchronous VENC keeps that block pinned
	 * until hardware completes; a single 1080p frame that exceeds 16.7 ms then
	 * exhausts the VPSS pool and makes fill_buffers spin. Copying here lets Core
	 * release the VPSS frame immediately after SendFrame while VENC owns only
	 * its fixed staging allocation. */
	if (frame_info->stVFrame.u32Stride[0] != (CVI_U32)w) {
		for (int row = 0; row < h * 3 / 2; ++row) {
			memcpy((uint8_t *)frame_info->stVFrame.pu8VirAddr[0]
					+ frame_info->stVFrame.u32Stride[0] * row,
				data + w * row, w);
		}
		CVI_U32 frame_size = frame_buffer_size(&frame_info->stVFrame);
		CVI_SYS_IonFlushCache(frame_info->stVFrame.u64PhyAddr[0],
			frame_info->stVFrame.pu8VirAddr[0], frame_size);
	} else {
		memcpy(frame_info->stVFrame.pu8VirAddr[0], data, w * h * 3 / 2);
		CVI_U32 frame_size = frame_buffer_size(&frame_info->stVFrame);
		CVI_SYS_IonFlushCache(frame_info->stVFrame.u64PhyAddr[0],
			frame_info->stVFrame.pu8VirAddr[0], frame_size);
	}

	s32Ret = CVI_VENC_SendFrame(ch, frame_info, 1000);
	if (s32Ret != CVI_SUCCESS) {
		printf("CVI_VENC_SendFrame failed with %#x\n", s32Ret);
		return s32Ret;
	}

	info->packet_pending = 1;

	return res;
}

static bool venc_stream_ready(int ch)
{
	VENC_CHN_STATUS_S status{};
	if (CVI_VENC_QueryStatus(ch, &status) != CVI_SUCCESS)
		return false;
	return status.u32CurPacks > 0 || status.u32LeftStreamFrames > 0;
}

// Copy one access unit directly from the vendor stream into caller storage.
static int copy_h26x_packet(int ch, uint8_t *dst, int capacity,
	int wait_timeout_us) {
	if (!dst || capacity <= 0 || ch < 0 || ch >= MMF_VENC_MAX_CHN ||
		!g_runtime.h26x_encoders[ch].initialized || wait_timeout_us < 0)
		return -1;

	H26xEncoderState *info = (H26xEncoderState *)&g_runtime.h26x_encoders[ch];
	VENC_STREAM_S *stream = &info->stream;
	if (!info->packet_pending)
		return 0;
	if (info->stream_held)
		return -EBUSY;

	if (info->fd < 0)
		info->fd = CVI_VENC_GetFd(ch);
	if (info->fd < 0)
		return -1;

	struct timeval now;
	gettimeofday(&now, NULL);
	const int64_t deadline_us = wait_timeout_us > 0
		? (int64_t)now.tv_sec * 1000000 + now.tv_usec + wait_timeout_us
		: 0;

	CVI_S32 ret = CVI_FAILURE;
	for (;;) {
		if (wait_timeout_us > 0) {
			gettimeofday(&now, NULL);
			const int64_t remaining =
				deadline_us - ((int64_t)now.tv_sec * 1000000 + now.tv_usec);
			if (remaining <= 0) {
				if (!venc_stream_ready(ch))
					return 0;
			} else {
				int fd = info->fd;
				fd_set read_fds;
				struct timeval timeout = {
					static_cast<time_t>(remaining / 1000000),
					static_cast<suseconds_t>(remaining % 1000000),
				};
				FD_ZERO(&read_fds);
				FD_SET(fd, &read_fds);
				CVI_S32 ready = select(fd + 1, &read_fds, NULL, NULL, &timeout);
				if (ready < 0) {
					if (errno == EINTR)
						return 0;
					printf("VencChn(%d) select failed: %s\n", ch, strerror(errno));
					return -1;
				}
				/* This driver's poll fd often stays silent on the
				   bound path. QueryStatus is the source of truth;
				   a select timeout must not skip a queued AU. */
				if (ready == 0 && !venc_stream_ready(ch))
					return 0;
			}
		} else if (!venc_stream_ready(ch)) {
			return 0;
		}

		stream->pstPack = info->packs;
		/* Never ask GetStream to block. The vendor EnterVcodecLock path
		   ignores a millisecond timeout when the worker already holds
		   the lock. select()/QueryStatus are the only bounded waits. */
		ret = CVI_VENC_GetStream(ch, stream, 0);
		if (ret == CVI_ERR_VENC_BUSY) {
			/* Level-triggered fd stays readable while the pack is
			   queued. Returning 0 here lets Core spin at 100% and
			   probe LT6911. Sleep inside the remaining budget. */
			if (wait_timeout_us <= 0)
				return 0;
			usleep(2000);
			continue;
		}
		if (ret != CVI_SUCCESS)
			return -1;
		break;
	}
	if (!info->bound_to_capture)
		info->packet_pending = 0;
	info->stream_held = 1;
	if (stream->u32PackCount == 0 || stream->u32PackCount > MMF_VENC_INTERNAL_PACKS) {
		CVI_VENC_ReleaseStream(ch, stream);
		info->stream_held = 0;
		return -1;
	}

	CVI_U32 total = 0;
	for (CVI_U32 index = 0; index < stream->u32PackCount; ++index) {
		VENC_PACK_S *pack = &stream->pstPack[index];
		if (pack->u32Offset > pack->u32Len) {
			CVI_VENC_ReleaseStream(ch, stream);
			info->stream_held = 0;
			return -1;
		}
		CVI_U32 size = pack->u32Len - pack->u32Offset;
		if (size > (CVI_U32)capacity - total) {
			CVI_VENC_ReleaseStream(ch, stream);
			info->stream_held = 0;
			return -2;
		}
		if (size > 0) {
			if (!pack->pu8Addr) {
				CVI_VENC_ReleaseStream(ch, stream);
				info->stream_held = 0;
				return -1;
			}
			memcpy(dst + total, pack->pu8Addr + pack->u32Offset, size);
		}
		total += size;
	}
	return (int)total;
}

int read_h26x_packet(int ch, uint8_t *dst, int capacity) {
	return copy_h26x_packet(ch, dst, capacity, 80 * 1000);
}

static int copy_and_release_h26x_packet(int ch, uint8_t *dst, int capacity,
	int first_wait_timeout_us) {
	int result = copy_h26x_packet(
		ch, dst, capacity, first_wait_timeout_us);
	if (result <= 0)
		return result;
	if (release_h26x_packet(ch) != 0)
		return -1;
	/* H.264/H.265 P frames form a reference chain.  Returning a newer access
	 * unit after silently discarding an older P frame produces an undecodable
	 * stream even when it lowers the apparent queue latency.  Consume exactly
	 * one complete access unit per call; explicit drain sites request a fresh
	 * IDR before forwarding video again. */
	return result;
}

int read_latest_h26x_packet(int ch, uint8_t *dst, int capacity) {
	return copy_and_release_h26x_packet(ch, dst, capacity, 80 * 1000);
}

int read_latest_h26x_packet_nowait(int ch, uint8_t *dst, int capacity) {
	return copy_and_release_h26x_packet(ch, dst, capacity, 0);
}

int drain_h26x_packets(int ch, uint8_t *scratch, int capacity) {
	int drained = 0;
	while (drained < MMF_VENC_INTERNAL_PACKS) {
		int result = copy_h26x_packet(ch, scratch, capacity, 0);
		if (result == 0)
			break;
		/* pop_into already releases an access unit that does not fit. Draining
		 * is intentionally destructive, so count it and keep emptying the queue. */
		if (result == -2) {
			++drained;
			continue;
		}
		if (result < 0)
			return result;
		if (release_h26x_packet(ch) != 0)
			return -1;
		++drained;
	}
	return drained;
}

int release_h26x_packet(int ch) {
	CVI_S32 s32Ret = CVI_SUCCESS;
	if (ch < 0 || ch >= MMF_VENC_MAX_CHN || !g_runtime.h26x_encoders[ch].initialized) {
		printf("Invalid venc ch:%d\r\n", ch);
		return -1;
	}

	H26xEncoderState *info = (H26xEncoderState *)&g_runtime.h26x_encoders[ch];
	VENC_STREAM_S *venc_stream = (VENC_STREAM_S *)&g_runtime.h26x_encoders[ch].stream;
	if (!info->stream_held) {
		return s32Ret;
	}

	s32Ret = CVI_VENC_ReleaseStream(ch, venc_stream);
	if (s32Ret != CVI_SUCCESS) {
		printf("CVI_VENC_ReleaseStream failed with %#x\n", s32Ret);
		return s32Ret;
	}

	venc_stream->u32PackCount = 0;
	venc_stream->pstPack = info->packs;
	info->stream_held = 0;
	return s32Ret;
}

int bind_h26x_to_capture(int ch, int vpss_group, int vpss_channel) {
	if (ch < 0 || ch >= MMF_VENC_MAX_CHN || vpss_group < 0 ||
		vpss_channel < 0 || vpss_channel >= MMF_VI_MAX_CHN ||
		!g_runtime.h26x_encoders[ch].initialized || !capture_channel_open(vpss_channel))
		return -1;
	H26xEncoderState *info = &g_runtime.h26x_encoders[ch];
	if (info->bound_to_capture) {
		if (info->capture_group == vpss_group &&
			info->capture_channel == vpss_channel) {
			start_h26x_reader(ch);
			return 0;
		}
		if (unbind_h26x_from_capture(ch) != 0)
			return -1;
	}
	/* The first start must happen after the pipeline is bound.  Once started,
	 * keep the vendor worker alive across client disconnects: this driver
	 * cannot safely stop and restart a VENC worker, but it can wait idle while
	 * its VPSS producer is temporarily unbound. */
	CVI_S32 ret = CVI_SUCCESS;
	const bool start_worker = !info->receiver_started;
	if (info->fd >= 0) {
		CVI_VENC_CloseFd(ch);
		info->fd = -1;
	}
	VENC_RECV_PIC_PARAM_S recv_param;
	recv_param.s32RecvPicNum = -1;
	VPSS_CHN_ATTR_S attr;
	ret = CVI_VPSS_GetChnAttr(vpss_group, vpss_channel, &attr);
	if (ret != CVI_SUCCESS)
		return ret;
	const CVI_U32 old_depth = attr.u32Depth;
	/* A bound output is consumed by VENC and does not need a userspace dequeue
	 * queue. Depth zero removes one full-frame hold and lowers capture latency. */
	attr.u32Depth = 0;
	ret = CVI_VPSS_SetChnAttr(vpss_group, vpss_channel, &attr);
	if (ret != CVI_SUCCESS)
		return ret;
	ret = SAMPLE_COMM_VPSS_Bind_VENC(vpss_group, vpss_channel, ch);
	if (ret != CVI_SUCCESS) {
		attr.u32Depth = old_depth;
		CVI_VPSS_SetChnAttr(vpss_group, vpss_channel, &attr);
		printf("VPSS(%d,%d) bind VENC(%d) failed with %#x\n",
			vpss_group, vpss_channel, ch, ret);
		return ret;
	}
	if (start_worker) {
		ret = CVI_VENC_StartRecvFrame(ch, &recv_param);
		if (ret != CVI_SUCCESS) {
			SAMPLE_COMM_VPSS_UnBind_VENC(vpss_group, vpss_channel, ch);
			attr.u32Depth = old_depth;
			CVI_VPSS_SetChnAttr(vpss_group, vpss_channel, &attr);
			CVI_VENC_ResetChn(ch);
			return ret;
		}
		info->receiver_started = 1;
	}
	info->bound_to_capture = 1;
	info->capture_group = (uint8_t)vpss_group;
	info->capture_channel = (uint8_t)vpss_channel;
	info->packet_pending = 1;
	start_h26x_reader(ch);
	return 0;
}

int unbind_h26x_from_capture(int ch) {
	if (ch < 0 || ch >= MMF_VENC_MAX_CHN || !g_runtime.h26x_encoders[ch].initialized)
		return -1;
	H26xEncoderState *info = &g_runtime.h26x_encoders[ch];
	if (!info->bound_to_capture)
		return 0;
	const int vpss_group = info->capture_group;
	const int vpss_channel = info->capture_channel;
	if (info->stream_held && release_h26x_packet(ch) != 0)
		return -1;
	/* Disconnect only the producer.  CVITEK's VENC worker is not restart-safe:
	 * after StopRecvFrame a later StartRecvFrame can return success while the
	 * worker remains absent, causing VPSS to fill the VENC waitq.  Keep the
	 * worker idle here and stop it exactly once when the channel is destroyed. */
	if (info->fd >= 0) {
		CVI_VENC_CloseFd(ch);
		info->fd = -1;
	}
	CVI_S32 ret = SAMPLE_COMM_VPSS_UnBind_VENC(
		vpss_group, vpss_channel, ch);
	if (ret != CVI_SUCCESS) {
		printf("VPSS(%d,%d) unbind VENC(%d) failed with %#x\n",
			vpss_group, vpss_channel, ch, ret);
		return ret;
	}
	info->bound_to_capture = 0;
	info->packet_pending = 0;
	info->capture_group = 0;
	info->capture_channel = 0;

	/* Restore the userspace dequeue depth while the still-running VENC worker
	 * waits idle without a producer. */
	VPSS_CHN_ATTR_S attr;
	CVI_S32 depth_ret = CVI_VPSS_GetChnAttr(
		vpss_group, vpss_channel, &attr);
	if (depth_ret == CVI_SUCCESS) {
		attr.u32Depth = MMF_VPSS_LOW_LATENCY_DEPTH;
		depth_ret = CVI_VPSS_SetChnAttr(
			vpss_group, vpss_channel, &attr);
	}
	if (ret == CVI_SUCCESS && depth_ret != CVI_SUCCESS)
		ret = depth_ret;
	return ret;
}

int request_h26x_idr(int ch) {
	if (ch < 0 || ch >= MMF_VENC_MAX_CHN || !g_runtime.h26x_encoders[ch].initialized)
		return -1;
	return CVI_VENC_RequestIDR(ch, CVI_TRUE);
}

void start_h26x_reader(int ch)
{
	if (ch < 0 || ch >= MMF_VENC_MAX_CHN)
		return;
	H26xReader &reader = g_readers[ch];
	if (reader.running.load(std::memory_order_acquire))
		return;
	reader.stop.store(false, std::memory_order_relaxed);
	reader.last_packet_ns.store(0, std::memory_order_relaxed);
	{
		std::lock_guard<std::mutex> lock(reader.mu);
		reader.packet.clear();
	}
	reader.running.store(true, std::memory_order_release);
	reader.thread = std::thread([ch]() {
		H26xReader &self = g_readers[ch];
		std::vector<uint8_t> scratch(1024 * 1024);
		if (request_h26x_idr(ch) != 0)
			printf("OneKVM: reader IDR request failed on ch %d\n", ch);
		while (!self.stop.load(std::memory_order_relaxed)) {
			const int got = read_latest_h26x_packet(
				ch, scratch.data(), static_cast<int>(scratch.size()));
			if (got <= 0)
				continue;
			if (self.last_packet_ns.load(std::memory_order_relaxed) == 0)
				printf("OneKVM: VENC reader got first %d-byte AU on ch %d\n",
					got, ch);
			std::lock_guard<std::mutex> lock(self.mu);
			self.packet.assign(scratch.begin(), scratch.begin() + got);
			self.last_packet_ns.store(reader_now_ns(), std::memory_order_relaxed);
		}
		self.running.store(false, std::memory_order_release);
	});
}

void stop_h26x_reader(int ch)
{
	if (ch < 0 || ch >= MMF_VENC_MAX_CHN)
		return;
	H26xReader &reader = g_readers[ch];
	reader.stop.store(true, std::memory_order_relaxed);
	if (reader.thread.joinable())
		reader.thread.detach();
}

int take_ready_h26x_packet(int ch, uint8_t *dst, int capacity)
{
	if (ch < 0 || ch >= MMF_VENC_MAX_CHN || dst == nullptr || capacity <= 0)
		return -1;
	H26xReader &reader = g_readers[ch];
	std::lock_guard<std::mutex> lock(reader.mu);
	if (reader.packet.empty())
		return 0;
	if (static_cast<int>(reader.packet.size()) > capacity)
		return -2;
	std::memcpy(dst, reader.packet.data(), reader.packet.size());
	const int size = static_cast<int>(reader.packet.size());
	reader.packet.clear();
	return size;
}

uint64_t h26x_reader_last_packet_ns(int ch)
{
	if (ch < 0 || ch >= MMF_VENC_MAX_CHN)
		return 0;
	return g_readers[ch].last_packet_ns.load(std::memory_order_relaxed);
}

} // namespace onekvm::mmf
