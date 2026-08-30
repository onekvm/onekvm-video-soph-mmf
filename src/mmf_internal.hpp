#pragma once

#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/param.h>
#include "math.h"
#include <inttypes.h>

#include <fcntl.h>		/* low-level i/o */
#include "cvi_buffer.h"
#include "cvi_ae_comm.h"
#include "cvi_awb_comm.h"
#include "cvi_comm_isp.h"
#include "cvi_comm_sns.h"
#include "cvi_ae.h"
#include "cvi_awb.h"
#include "cvi_isp.h"
#include "cvi_sns_ctrl.h"
#include "cvi_sys.h"
#include "sample_comm.h"
#include "mmf.hpp"

namespace onekvm::mmf {

#define MMF_VI_MAX_CHN 			2		// manually limit the max channel number of vi
#define MMF_VENC_MAX_CHN		4
#define MMF_VENC_INTERNAL_PACKS	64
#define MMF_VENC_STREAM_BUF_SIZE	(1024 * 1024)
#define MMF_VI_MAP_CACHE_SIZE	4

/* VPSS u32Depth only counts completed frames queued inside the channel.  It
 * does not account for a frame borrowed by userspace/VENC or the block that
 * the VPSS worker is filling.  The low-latency path has at most one borrowed
 * frame because source_read rejects a second read until source_release. */
#define MMF_VPSS_LOW_LATENCY_DEPTH	1
#define MMF_VPSS_BORROWED_BLOCKS	1
#define MMF_VPSS_PRODUCER_BLOCKS	1

/* NanoKVM's original H.264 rate-control defaults.  Automatic image quality
 * changes the bitrate budget only; it must not silently change QP. */
#define MMF_VENC_DEFAULT_INITIAL_QP	35
#define MMF_VENC_DEFAULT_MIN_QP		20
#define MMF_VENC_DEFAULT_MAX_QP		51

#define MMF_VB_VI_ID			0

#if VPSS_MAX_PHY_CHN_NUM < MMF_VI_MAX_CHN
#error "VPSS_MAX_PHY_CHN_NUM < MMF_VI_MAX_CHN"
#endif

typedef struct {
	CVI_U64 phy_addr;
	CVI_VOID *vir_addr;
	CVI_U32 size;
} CaptureMapping;

struct H26xEncoderState {
	uint8_t ch;
	H26xCodec codec;
	bool initialized;
	bool packet_pending;
	bool receiver_started;
	bool stream_held;
	bool bound_to_capture;
	uint8_t capture_group;
	uint8_t capture_channel;
	int fd;
	VIDEO_FRAME_INFO_S *staging_frame;
	VENC_STREAM_S stream;
	VENC_PACK_S packs[MMF_VENC_INTERNAL_PACKS];
	H26xEncoderConfig cfg;
	uint32_t staging_pool_id;
	uint64_t last_submit_ns;
	uint64_t last_encode_ns;
	uint64_t last_capture_ns;
	uint64_t last_hw_sample_ns;
};

struct BufferPool {
	char name[15];
	bool is_used;
	uint32_t pool_id;
	uint32_t size;
	uint32_t max_num;
};

struct RuntimeState {
	int reference_count;
	bool vi_is_inited;
	bool vi_chn_is_inited[MMF_VI_MAX_CHN];
	bool vi_chn_running[MMF_VI_MAX_CHN];
	bool vi_bound_to_vpss;
	int vi_chn_pool_id[MMF_VI_MAX_CHN];
	SIZE_S vi_size;
	VIDEO_FRAME_INFO_S *vpss_user_frame[2];
	int vpss_user_pool_id;
	int vpss_user_index;
	VIDEO_FRAME_INFO_S vi_frame[MMF_VI_MAX_CHN];
	CaptureMapping vi_mappings[MMF_VI_MAX_CHN][MMF_VI_MAP_CACHE_SIZE];
	uint8_t vi_map_next[MMF_VI_MAX_CHN];
	VB_CONFIG_S vb_conf;

	bool jpeg_initialized;
	bool jpeg_frame_pending;
	bool jpeg_stream_held;
	VENC_STREAM_S jpeg_stream;
	VENC_PACK_S jpeg_packs[MMF_VENC_INTERNAL_PACKS];
	int jpeg_width;
	int jpeg_height;
	int jpeg_pixel_format;
	int jpeg_quality;
	VIDEO_FRAME_INFO_S *jpeg_staging_frame;
	int jpeg_staging_pool_id;

	H26xEncoderState h26x_encoders[MMF_VENC_MAX_CHN];
	bool h26x_encoder_active;

	BufferPool vb_pool[VB_MAX_COMM_POOLS];
};

struct CaptureOptions {
	bool mirror[MMF_VI_MAX_CHN];
	bool flip[MMF_VI_MAX_CHN];
};

#define MMF_INTERNAL __attribute__((visibility("hidden")))

extern MMF_INTERNAL RuntimeState g_runtime;
extern MMF_INTERNAL CaptureOptions g_capture_options;

MMF_INTERNAL int _create_vb_pool(
    const char *name, uint32_t size, uint32_t max_num);
MMF_INTERNAL int _destroy_vb_pool(uint32_t pool_id);
MMF_INTERNAL CVI_U32 frame_buffer_size(const VIDEO_FRAME_S *frame);
MMF_INTERNAL VIDEO_FRAME_INFO_S *allocate_frame(
    int id, SIZE_S size, PIXEL_FORMAT_E format);
MMF_INTERNAL CVI_S32 free_frame(VIDEO_FRAME_INFO_S *frame);
MMF_INTERNAL CVI_S32 _mmf_vpss_deinit(VPSS_GRP group, VPSS_CHN channel);
MMF_INTERNAL CVI_S32 destroy_vpss_group(VPSS_GRP group);
MMF_INTERNAL CVI_S32 _mmf_vpss_init(VPSS_GRP group, VPSS_CHN channel,
    SIZE_S input_size, SIZE_S output_size, PIXEL_FORMAT_E input_format,
    PIXEL_FORMAT_E output_format, int fps, int depth, bool mirror, bool flip,
    int fit);
MMF_INTERNAL VIDEO_FRAME_INFO_S *find_capture_frame(
    uint8_t *data, int width, int height, int format);

} // namespace onekvm::mmf
