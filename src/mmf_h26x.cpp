#include "mmf_internal.hpp"
#include "h264_annexb.hpp"
#include "hw_latency_parser.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <poll.h>
#include <thread>
#include <vector>

namespace onekvm::mmf {

/* Keep enough complete access units to absorb the scheduler stalls caused by
 * synchronous CryptoDMA batches. The Core sender normally drains faster than
 * VENC produces, so this capacity does not add steady-state prefetch latency;
 * it only preserves the P-frame chain across short 50-200 ms stalls. */
constexpr std::size_t kReaderQueueMax = 16;
constexpr int64_t kVencPollWatchdogUs = 40 * 1000;

struct H26xQueuedPacket {
	std::vector<uint8_t> data;
	bool key_frame = false;
};

struct H26xReader {
	std::thread thread;
	std::atomic<bool> stop{false};
	std::atomic<bool> running{false};
	std::atomic<bool> want_idr{false};
	std::atomic<int> pending_output_fps{0};
	std::atomic<int> pending_gop{0};
	std::atomic<int> pending_bitrate_kbps{0};
	std::atomic<int> pending_initial_qp{0};
	std::atomic<int> pending_min_qp{0};
	std::atomic<int> pending_max_qp{0};
	std::atomic<uint32_t> pending_rc_mask{0};
	std::atomic<uint64_t> last_packet_ns{0};
	std::mutex mu;
	std::mutex join_mu;
	std::condition_variable cv;
	std::deque<H26xQueuedPacket> queue;
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

	info->last_submit_ns = reader_now_ns();
	info->packet_pending = 1;

	return res;
}

static bool venc_stream_ready(int ch)
{
	VENC_CHN_STATUS_S status{};
	if (CVI_VENC_QueryStatus(ch, &status) != CVI_SUCCESS)
		return false;
	/* Vendor JPEG / sample paths only trust u32CurPacks. LeftStreamFrames
	   is documented as TODO and can stick nonzero. */
	return status.u32CurPacks > 0;
}

/* Bound path: pack.u64PTS is encode-complete (~1 ms from QueryStatus), not
   VI dqbuf. Sample VPSS CostTime and VENC HwEncTime at most twice a second.
   Do not read /proc/cvitek/vi or vi_dbg here. */
static void refresh_bound_hw_latency(H26xEncoderState *info)
{
	if (info == nullptr || !info->bound_to_capture)
		return;
	const uint64_t now = reader_now_ns();
	const uint64_t last = __atomic_load_n(&info->last_hw_sample_ns, __ATOMIC_RELAXED);
	if (last != 0 && now - last < 500000000ull)
		return;
	__atomic_store_n(&info->last_hw_sample_ns, now, __ATOMIC_RELAXED);

	uint32_t vpss_us = 0;
	if (FILE *vpss = fopen("/proc/cvitek/vpss", "r")) {
		char line[512];
		while (fgets(line, sizeof(line), vpss) != nullptr) {
			if (parse_vpss_grp_cost_us(line, info->capture_group, &vpss_us))
				break;
		}
		fclose(vpss);
	}

	uint32_t hwenc_us = 0;
	if (FILE *venc = fopen("/proc/cvitek/venc", "r")) {
		char line[512];
		while (fgets(line, sizeof(line), venc) != nullptr) {
			if (parse_venc_hwenc_us(line, info->ch, &hwenc_us))
				break;
		}
		fclose(venc);
	}

	const uint32_t capture_us = bound_capture_us(vpss_us, info->cfg.input_fps);
	if (capture_us > 0 && capture_us < 1000000u)
		__atomic_store_n(&info->last_capture_ns,
			static_cast<uint64_t>(capture_us) * 1000ull, __ATOMIC_RELAXED);
	if (hwenc_us > 0 && hwenc_us < 1000000u)
		__atomic_store_n(&info->last_encode_ns,
			static_cast<uint64_t>(hwenc_us) * 1000ull, __ATOMIC_RELAXED);
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
	uint64_t pack_ready_ns = 0;
	for (;;) {
		gettimeofday(&now, NULL);
		const int64_t remaining = wait_timeout_us > 0
			? deadline_us - ((int64_t)now.tv_sec * 1000000 + now.tv_usec)
			: 0;
		const bool past_deadline = wait_timeout_us > 0 && remaining <= 0;
		if (past_deadline)
			return 0;

		/* The driver reports already queued streams from its poll callback and
		 * wakes this fd after the bound worker publishes a new stream.  A longer
		 * watchdog retains a bounded recovery path without issuing QueryStatus on
		 * every millisecond of a normal 60 fps frame interval. */
		struct pollfd pfd{};
		pfd.fd = info->fd;
		pfd.events = POLLIN;
		const int64_t poll_us = wait_timeout_us > 0
			? (remaining < kVencPollWatchdogUs ? remaining : kVencPollWatchdogUs)
			: 0;
		const int timeout_ms = static_cast<int>((poll_us + 999) / 1000);
		const int poll_ret = poll(&pfd, 1, timeout_ms);
		if (poll_ret < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (poll_ret == 0) {
			if (wait_timeout_us <= 0)
				return 0;
			/* A timeout is exceptional with the fixed bound-mode driver. Check
			 * status once as a watchdog for older or missed-wakeup drivers. */
			if (!venc_stream_ready(ch)) {
				refresh_bound_hw_latency(info);
				continue;
			}
		} else {
			if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
				return -1;
			if (!(pfd.revents & (POLLIN | POLLRDNORM)))
				continue;
		}
		if (pack_ready_ns == 0)
			pack_ready_ns = reader_now_ns();

		stream->pstPack = info->packs;
		/* Never ask GetStream to block. The vendor EnterVcodecLock path
		   ignores a millisecond timeout when the worker already holds
		   the lock. The fd poll above is the bounded wait. */
		ret = CVI_VENC_GetStream(ch, stream, 0);
		if (ret == CVI_ERR_VENC_BUSY) {
			if (wait_timeout_us <= 0 || past_deadline)
				return 0;
			usleep(200);
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
	const uint64_t done_ns = reader_now_ns();
	/* Unbound: SendFrame → GetStream. Bound encode comes from VENC
	   HwEncTime; pack.u64PTS is encode-complete and is not capture. */
	if (!info->bound_to_capture) {
		uint64_t encode_start_ns = info->last_submit_ns;
		if (encode_start_ns == 0)
			encode_start_ns = pack_ready_ns;
		if (encode_start_ns > 0 && done_ns > encode_start_ns) {
			const uint64_t encode_ns = done_ns - encode_start_ns;
			if (encode_ns < 1000000000ull)
				__atomic_store_n(&info->last_encode_ns, encode_ns,
					__ATOMIC_RELAXED);
		}
	}
	info->last_submit_ns = 0;
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
	refresh_h26x_hw_latency(ch);
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
			if (resume_vpss_channel(vpss_channel) != 0)
				return -1;
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
	if (resume_vpss_channel(vpss_channel) != 0)
		return -1;
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
	if (ret != CVI_SUCCESS) {
		(void)pause_vpss_channel(vpss_channel);
		return ret;
	}
	const CVI_U32 old_depth = attr.u32Depth;
	/* A bound output is consumed by VENC and does not need a userspace dequeue
	 * queue. Depth zero removes one full-frame hold and lowers capture latency. */
	attr.u32Depth = 0;
	ret = CVI_VPSS_SetChnAttr(vpss_group, vpss_channel, &attr);
	if (ret != CVI_SUCCESS) {
		(void)pause_vpss_channel(vpss_channel);
		return ret;
	}
	ret = SAMPLE_COMM_VPSS_Bind_VENC(vpss_group, vpss_channel, ch);
	if (ret != CVI_SUCCESS) {
		attr.u32Depth = old_depth;
		CVI_VPSS_SetChnAttr(vpss_group, vpss_channel, &attr);
		printf("VPSS(%d,%d) bind VENC(%d) failed with %#x\n",
			vpss_group, vpss_channel, ch, ret);
		(void)pause_vpss_channel(vpss_channel);
		return ret;
	}
	if (start_worker) {
		ret = CVI_VENC_StartRecvFrame(ch, &recv_param);
		if (ret != CVI_SUCCESS) {
			SAMPLE_COMM_VPSS_UnBind_VENC(vpss_group, vpss_channel, ch);
			attr.u32Depth = old_depth;
			CVI_VPSS_SetChnAttr(vpss_group, vpss_channel, &attr);
			CVI_VENC_ResetChn(ch);
			(void)pause_vpss_channel(vpss_channel);
			return ret;
		}
		info->receiver_started = 1;
	}
	info->bound_to_capture = 1;
	info->capture_group = (uint8_t)vpss_group;
	info->capture_channel = (uint8_t)vpss_channel;
	info->packet_pending = 1;
	/* A newly started worker emits a complete parameter-set + IDR access unit
	 * for its first frame.  Requesting another IDR here races that pending AU:
	 * the vendor recovery path discards it and the replacement carries SPS but
	 * no PPS.  Existing workers still need an explicit IDR when they are rebound
	 * after an idle or placeholder interval. */
	start_h26x_reader(ch, !start_worker);
	return 0;
}

int unbind_h26x_from_capture(int ch) {
	if (ch < 0 || ch >= MMF_VENC_MAX_CHN || !g_runtime.h26x_encoders[ch].initialized)
		return -1;
	H26xEncoderState *info = &g_runtime.h26x_encoders[ch];
	if (!info->bound_to_capture) {
		park_unbound_vpss_channels();
		return 0;
	}
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
	park_unbound_vpss_channels();
	return ret;
}

void h26x_reader_want_idr(int ch)
{
	if (ch < 0 || ch >= MMF_VENC_MAX_CHN)
		return;
	H26xReader &reader = g_readers[ch];
	reader.want_idr.store(true, std::memory_order_relaxed);
	std::lock_guard<std::mutex> lock(reader.mu);
	auto it = reader.queue.begin();
	while (it != reader.queue.end()) {
		if (it->key_frame)
			++it;
		else
			it = reader.queue.erase(it);
	}
	reader.cv.notify_all();
}

int request_h26x_idr(int ch) {
	if (ch < 0 || ch >= MMF_VENC_MAX_CHN || !g_runtime.h26x_encoders[ch].initialized)
		return -1;
	h26x_reader_want_idr(ch);
	H26xReader &reader = g_readers[ch];
	/* StartRecvFrame naturally makes the first encoded frame an IDR with the
	 * complete SPS/PPS set.  WebRTC and WebSocket subscribers both request a
	 * keyframe as they attach; if that request reaches the vendor before the
	 * first AU is dequeued, its GOP-reset path replaces the complete AU with an
	 * SPS-only IDR.  The reader is already waiting for the natural keyframe, so
	 * coalesce only this startup request. */
	if (reader.running.load(std::memory_order_acquire) &&
	    reader.last_packet_ns.load(std::memory_order_relaxed) == 0)
		return 0;
	return CVI_VENC_RequestIDR(ch, CVI_TRUE);
}

constexpr uint32_t kH26xRcFpsGop = 1u << 0;
constexpr uint32_t kH26xRcBitrate = 1u << 1;
constexpr uint32_t kH26xRcQp = 1u << 2;

static int apply_h26x_rate_control(int ch, int output_fps, int gop,
	int bitrate_kbps, int initial_qp, int min_qp, int max_qp, uint32_t mask)
{
	if (ch < 0 || ch >= MMF_VENC_MAX_CHN || !g_runtime.h26x_encoders[ch].initialized)
		return -1;
	if (mask == 0)
		return 0;
	H26xEncoderConfig &cfg = g_runtime.h26x_encoders[ch].cfg;
	if ((mask & kH26xRcFpsGop) != 0) {
		output_fps = clamp_output_fps(output_fps, cfg.width, cfg.height);
		if (output_fps <= 0)
			return -1;
		if (gop <= 0)
			gop = output_fps;
	} else {
		output_fps = cfg.output_fps;
		gop = cfg.gop;
	}
	if ((mask & kH26xRcBitrate) != 0 && bitrate_kbps <= 0)
		return -1;
	if ((mask & kH26xRcBitrate) == 0)
		bitrate_kbps = cfg.bitrate_kbps;

	if ((mask & (kH26xRcFpsGop | kH26xRcBitrate)) != 0) {
		VENC_CHN_ATTR_S attr{};
		CVI_S32 ret = CVI_VENC_GetChnAttr(ch, &attr);
		if (ret != CVI_SUCCESS) {
			printf("CVI_VENC_GetChnAttr [%d] failed with %d\n", ch, ret);
			return ret;
		}
		const CVI_U32 src_fps = static_cast<CVI_U32>(
			venc_src_fps(output_fps, cfg.width, cfg.height));
		if (attr.stVencAttr.enType == PT_H264 &&
		    attr.stRcAttr.enRcMode == VENC_RC_MODE_H264VBR) {
			attr.stRcAttr.stH264Vbr.u32SrcFrameRate = src_fps;
			attr.stRcAttr.stH264Vbr.fr32DstFrameRate = output_fps;
			attr.stRcAttr.stH264Vbr.u32Gop = static_cast<CVI_U32>(gop);
			attr.stRcAttr.stH264Vbr.u32MaxBitRate = bitrate_kbps;
		} else if (attr.stVencAttr.enType == PT_H265 &&
			   attr.stRcAttr.enRcMode == VENC_RC_MODE_H265VBR) {
			attr.stRcAttr.stH265Vbr.u32SrcFrameRate = src_fps;
			attr.stRcAttr.stH265Vbr.fr32DstFrameRate = output_fps;
			attr.stRcAttr.stH265Vbr.u32Gop = static_cast<CVI_U32>(gop);
			attr.stRcAttr.stH265Vbr.u32MaxBitRate = bitrate_kbps;
		} else {
			return -1;
		}
		ret = CVI_VENC_SetChnAttr(ch, &attr);
		if (ret != CVI_SUCCESS) {
			printf("CVI_VENC_SetChnAttr [%d] rc failed with %d\n", ch, ret);
			return ret;
		}
		cfg.output_fps = output_fps;
		cfg.gop = gop;
		cfg.bitrate_kbps = bitrate_kbps;
		printf("OneKVM: VENC ch %d fps %d gop %d bitrate %d kbps\n",
			ch, output_fps, gop, bitrate_kbps);
	}

	if ((mask & kH26xRcQp) == 0)
		return 0;
	RateControl rc{};
	rc.initial_qp = initial_qp;
	rc.min_qp = min_qp;
	rc.max_qp = max_qp;
	return _set_venc_rc_param(ch, &cfg, &rc);
}

int set_h26x_output_fps(int ch, int output_fps, int gop)
{
	if (ch < 0 || ch >= MMF_VENC_MAX_CHN)
		return -1;
	if (output_fps <= 0)
		return -1;
	if (gop <= 0)
		gop = output_fps;
	H26xReader &reader = g_readers[ch];
	reader.pending_gop.store(gop, std::memory_order_relaxed);
	reader.pending_output_fps.store(output_fps, std::memory_order_relaxed);
	reader.pending_rc_mask.fetch_or(kH26xRcFpsGop, std::memory_order_release);
	reader.cv.notify_all();
	return 0;
}

int set_h26x_rate_control(int ch, int output_fps, int gop, int bitrate_kbps,
	int initial_qp, int min_qp, int max_qp)
{
	if (ch < 0 || ch >= MMF_VENC_MAX_CHN)
		return -1;
	if (output_fps <= 0)
		return -1;
	if (gop <= 0)
		gop = output_fps;
	H26xReader &reader = g_readers[ch];
	reader.pending_gop.store(gop, std::memory_order_relaxed);
	reader.pending_output_fps.store(output_fps, std::memory_order_relaxed);
	if (bitrate_kbps > 0)
		reader.pending_bitrate_kbps.store(bitrate_kbps, std::memory_order_relaxed);
	reader.pending_initial_qp.store(initial_qp, std::memory_order_relaxed);
	reader.pending_min_qp.store(min_qp, std::memory_order_relaxed);
	reader.pending_max_qp.store(max_qp, std::memory_order_relaxed);
	uint32_t mask = kH26xRcFpsGop | kH26xRcQp;
	if (bitrate_kbps > 0)
		mask |= kH26xRcBitrate;
	reader.pending_rc_mask.fetch_or(mask, std::memory_order_release);
	reader.cv.notify_all();
	return 0;
}

void start_h26x_reader(int ch, bool request_idr)
{
	if (ch < 0 || ch >= MMF_VENC_MAX_CHN)
		return;
	H26xReader &reader = g_readers[ch];
	std::lock_guard<std::mutex> join_lock(reader.join_mu);
	if (reader.running.load(std::memory_order_acquire))
		return;
	if (reader.thread.joinable())
		reader.thread.join();
	reader.stop.store(false, std::memory_order_relaxed);
	reader.last_packet_ns.store(0, std::memory_order_relaxed);
	reader.want_idr.store(true, std::memory_order_relaxed);
	{
		std::lock_guard<std::mutex> lock(reader.mu);
		reader.queue.clear();
	}
	reader.running.store(true, std::memory_order_release);
	reader.thread = std::thread([ch, request_idr]() {
		H26xReader &self = g_readers[ch];
		std::vector<uint8_t> scratch(1024 * 1024);
		AnnexBParameterSets parameter_sets;
		bool idr_ioctl_sent = !request_idr;
		if (request_idr) {
			/* This is the reader's own between-GetStream request.  Calling
			 * request_h26x_idr() here would mistake it for a subscriber's
			 * concurrent startup request and coalesce it while last_packet_ns is
			 * still zero, so an existing worker rebound after idle would wait for
			 * the natural GOP instead of receiving the requested recovery IDR. */
			if (CVI_VENC_RequestIDR(ch, CVI_TRUE) != CVI_SUCCESS)
				printf("OneKVM: reader IDR request failed on ch %d\n", ch);
			else
				idr_ioctl_sent = true;
		}
		while (!self.stop.load(std::memory_order_relaxed)) {
			bool queue_overrun = false;
			{
				std::lock_guard<std::mutex> lock(self.mu);
				if (self.stop.load(std::memory_order_relaxed))
					break;
				/* Never propagate a slow consumer back into the vendor VENC.
				 * Once its stream buffers fill, VPSS fills the VENC input waitq
				 * and the worker cannot recover.  In particular, waiting here for
				 * the consumer used to stop GetStream for 50 ms and directly cause
				 * "VENC waitq is full".  Drop the stale userspace chain immediately,
				 * drain through a fresh IDR, and resume with decodable output. */
				if (self.queue.size() >= kReaderQueueMax) {
					self.queue.clear();
					self.want_idr.store(true, std::memory_order_relaxed);
					queue_overrun = true;
				}
			}
			if (queue_overrun) {
				/* The ioctl runs only here, between GetStream calls.  Issue it once
				 * for this overrun instead of freezing until the next regular GOP;
				 * idr_ioctl_sent prevents repeats while that IDR is pending. */
				idr_ioctl_sent = false;
				refresh_bound_hw_latency(&g_runtime.h26x_encoders[ch]);
			}
			const uint32_t rc_mask = self.pending_rc_mask.exchange(
				0, std::memory_order_acq_rel);
			if (rc_mask != 0) {
				if (apply_h26x_rate_control(
					    ch,
					    self.pending_output_fps.load(std::memory_order_relaxed),
					    self.pending_gop.load(std::memory_order_relaxed),
					    self.pending_bitrate_kbps.load(std::memory_order_relaxed),
					    self.pending_initial_qp.load(std::memory_order_relaxed),
					    self.pending_min_qp.load(std::memory_order_relaxed),
					    self.pending_max_qp.load(std::memory_order_relaxed),
					    rc_mask) == 0)
					self.want_idr.store(true, std::memory_order_relaxed);
			}
			if (!self.want_idr.load(std::memory_order_relaxed))
				idr_ioctl_sent = false;
			else if (!idr_ioctl_sent) {
				if (CVI_VENC_RequestIDR(ch, CVI_TRUE) != CVI_SUCCESS)
					printf("OneKVM: reader IDR request failed on ch %d\n", ch);
				else
					idr_ioctl_sent = true;
			}
			const int got = read_latest_h26x_packet(
				ch, scratch.data(), static_cast<int>(scratch.size()));
			if (got <= 0) {
				std::unique_lock<std::mutex> lock(self.mu);
				self.cv.wait_for(lock, std::chrono::milliseconds(5), [&] {
					return self.stop.load(std::memory_order_relaxed);
				});
				continue;
			}
			const bool h265 =
				g_runtime.h26x_encoders[ch].codec == H26xCodec::H265;
			parameter_sets.update(
				scratch.data(), static_cast<std::size_t>(got), h265);
			const bool key = h265
				? annexb_has_h265_irap(
					scratch.data(), static_cast<std::size_t>(got))
				: annexb_has_idr(
					scratch.data(), static_cast<std::size_t>(got));
			if (self.want_idr.load(std::memory_order_relaxed) && !key)
				continue;
			if (self.last_packet_ns.load(std::memory_order_relaxed) == 0)
				printf("OneKVM: VENC reader got first %d-byte AU on ch %d\n",
					got, ch);
			bool overflow = false;
			{
				std::lock_guard<std::mutex> lock(self.mu);
				if (self.queue.size() >= kReaderQueueMax) {
					overflow = true;
				} else {
					H26xQueuedPacket packet;
					if (key) {
						packet.data = parameter_sets.augment_keyframe(
							scratch.data(), static_cast<std::size_t>(got), h265);
					} else {
						packet.data.assign(scratch.begin(), scratch.begin() + got);
					}
					packet.key_frame = key;
					self.queue.push_back(std::move(packet));
					if (key)
						self.want_idr.store(false, std::memory_order_relaxed);
					self.last_packet_ns.store(reader_now_ns(),
						std::memory_order_relaxed);
					self.cv.notify_all();
				}
			}
			if (overflow) {
				self.want_idr.store(true, std::memory_order_relaxed);
				/* The consumer may drain one AU before the next loop, so the
				 * queue-overrun check at the top is not guaranteed to run. Keep
				 * the request armed here; otherwise we can wait for the natural
				 * GOP after already discarding the dependent P-frame chain. */
				idr_ioctl_sent = false;
			}
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
	reader.cv.notify_all();
	std::lock_guard<std::mutex> join_lock(reader.join_mu);
	if (!reader.thread.joinable())
		return;
	if (reader.thread.get_id() == std::this_thread::get_id()) {
		reader.thread.detach();
		return;
	}
	reader.thread.join();
}

int take_ready_h26x_packet(int ch, uint8_t *dst, int capacity, bool *key_frame)
{
	if (ch < 0 || ch >= MMF_VENC_MAX_CHN || dst == nullptr || capacity <= 0)
		return -1;
	H26xReader &reader = g_readers[ch];
	std::lock_guard<std::mutex> lock(reader.mu);
	if (reader.queue.empty())
		return 0;
	H26xQueuedPacket &front = reader.queue.front();
	if (static_cast<int>(front.data.size()) > capacity)
		return -2;
	std::memcpy(dst, front.data.data(), front.data.size());
	const int size = static_cast<int>(front.data.size());
	if (key_frame != nullptr)
		*key_frame = front.key_frame;
	reader.queue.pop_front();
	reader.cv.notify_all();
	return size;
}

bool wait_ready_h26x_packet(int ch, int timeout_ms)
{
	if (ch < 0 || ch >= MMF_VENC_MAX_CHN || timeout_ms < 0)
		return false;
	H26xReader &reader = g_readers[ch];
	std::unique_lock<std::mutex> lock(reader.mu);
	if (!reader.queue.empty())
		return true;
	reader.cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
		return !reader.queue.empty() ||
			reader.stop.load(std::memory_order_relaxed);
	});
	return !reader.queue.empty();
}

uint64_t h26x_last_encode_ns(int ch)
{
	if (ch < 0 || ch >= MMF_VENC_MAX_CHN)
		return 0;
	return __atomic_load_n(&g_runtime.h26x_encoders[ch].last_encode_ns, __ATOMIC_RELAXED);
}

void refresh_h26x_hw_latency(int ch)
{
	if (ch < 0 || ch >= MMF_VENC_MAX_CHN)
		return;
	refresh_bound_hw_latency(&g_runtime.h26x_encoders[ch]);
}

uint64_t h26x_last_capture_ns(int ch)
{
	if (ch < 0 || ch >= MMF_VENC_MAX_CHN)
		return 0;
	return __atomic_load_n(&g_runtime.h26x_encoders[ch].last_capture_ns, __ATOMIC_RELAXED);
}

uint64_t h26x_reader_last_packet_ns(int ch)
{
	if (ch < 0 || ch >= MMF_VENC_MAX_CHN)
		return 0;
	return g_readers[ch].last_packet_ns.load(std::memory_order_relaxed);
}

} // namespace onekvm::mmf
