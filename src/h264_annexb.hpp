#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

constexpr uint8_t kAnnexBParamVPS = 1u << 0;
constexpr uint8_t kAnnexBParamSPS = 1u << 1;
constexpr uint8_t kAnnexBParamPPS = 1u << 2;

struct AnnexBNalUnit {
	std::size_t prefix = 0;
	std::size_t start = 0;
	std::size_t end = 0;
};

inline std::size_t annexb_start_code_size(const uint8_t *data,
	std::size_t size, std::size_t offset)
{
	if (data == nullptr || offset + 3 > size || data[offset] != 0 ||
	    data[offset + 1] != 0)
		return 0;
	if (offset + 4 <= size && data[offset + 2] == 0 && data[offset + 3] == 1)
		return 4;
	return data[offset + 2] == 1 ? 3 : 0;
}

inline bool annexb_next_nal_unit(const uint8_t *data, std::size_t size,
	std::size_t *cursor, AnnexBNalUnit *unit)
{
	if (data == nullptr || cursor == nullptr || unit == nullptr)
		return false;
	std::size_t prefix = *cursor;
	std::size_t prefix_size = 0;
	while (prefix < size &&
	       (prefix_size = annexb_start_code_size(data, size, prefix)) == 0)
		++prefix;
	if (prefix_size == 0 || prefix + prefix_size >= size)
		return false;

	const std::size_t start = prefix + prefix_size;
	std::size_t end = start + 1;
	while (end < size && annexb_start_code_size(data, size, end) == 0)
		++end;
	*unit = AnnexBNalUnit{prefix, start, end};
	*cursor = end;
	return true;
}

inline uint8_t annexb_parameter_set_mask(const uint8_t *data,
	std::size_t size, bool h265)
{
	uint8_t mask = 0;
	std::size_t cursor = 0;
	AnnexBNalUnit unit;
	while (annexb_next_nal_unit(data, size, &cursor, &unit)) {
		const uint8_t type = h265 ? (data[unit.start] >> 1) & 0x3f
			: data[unit.start] & 0x1f;
		if (h265 && type == 32)
			mask |= kAnnexBParamVPS;
		else if ((h265 && type == 33) || (!h265 && type == 7))
			mask |= kAnnexBParamSPS;
		else if ((h265 && type == 34) || (!h265 && type == 8))
			mask |= kAnnexBParamPPS;
	}
	return mask;
}

// CVITEK can return VPS/SPS/PPS as standalone access units immediately before
// an IDR. The reader deliberately drops non-IDR units while resynchronising,
// so retain the latest parameter sets and prepend them to an incomplete key AU.
class AnnexBParameterSets {
public:
	void update(const uint8_t *data, std::size_t size, bool h265)
	{
		std::size_t cursor = 0;
		AnnexBNalUnit unit;
		while (annexb_next_nal_unit(data, size, &cursor, &unit)) {
			const uint8_t type = h265 ? (data[unit.start] >> 1) & 0x3f
				: data[unit.start] & 0x1f;
			std::vector<uint8_t> *target = nullptr;
			if (h265 && type == 32)
				target = &vps_;
			else if ((h265 && type == 33) || (!h265 && type == 7))
				target = &sps_;
			else if ((h265 && type == 34) || (!h265 && type == 8))
				target = &pps_;
			if (target != nullptr)
				target->assign(data + unit.prefix, data + unit.end);
		}
	}

	bool complete(bool h265) const
	{
		return !sps_.empty() && !pps_.empty() && (!h265 || !vps_.empty());
	}

	std::vector<uint8_t> augment_keyframe(const uint8_t *data,
		std::size_t size, bool h265) const
	{
		const uint8_t required = kAnnexBParamSPS | kAnnexBParamPPS |
			(h265 ? kAnnexBParamVPS : 0);
		if (data == nullptr || size == 0 || !complete(h265) ||
		    (annexb_parameter_set_mask(data, size, h265) & required) == required)
			return data != nullptr ? std::vector<uint8_t>(data, data + size)
				: std::vector<uint8_t>();

		std::vector<uint8_t> result;
		result.reserve(vps_.size() + sps_.size() + pps_.size() + size);
		if (h265)
			result.insert(result.end(), vps_.begin(), vps_.end());
		result.insert(result.end(), sps_.begin(), sps_.end());
		result.insert(result.end(), pps_.begin(), pps_.end());
		result.insert(result.end(), data, data + size);
		return result;
	}

private:
	std::vector<uint8_t> vps_;
	std::vector<uint8_t> sps_;
	std::vector<uint8_t> pps_;
};

inline bool annexb_next_nal(const uint8_t *data, std::size_t size, std::size_t *index, std::size_t *start)
{
	if (data == nullptr || index == nullptr || start == nullptr)
		return false;
	while (*index + 3 < size) {
		if (data[*index] == 0 && data[*index + 1] == 0 && data[*index + 2] == 1) {
			*start = *index + 3;
			return *start < size;
		}
		if (*index + 4 <= size && data[*index] == 0 &&
		    data[*index + 1] == 0 && data[*index + 2] == 0 &&
		    data[*index + 3] == 1) {
			*start = *index + 4;
			return *start < size;
		}
		++*index;
	}
	return false;
}

inline bool annexb_has_idr(const uint8_t *data, std::size_t size)
{
	std::size_t index = 0;
	std::size_t start = 0;
	while (annexb_next_nal(data, size, &index, &start)) {
		if ((data[start] & 0x1fu) == 5)
			return true;
		index = start + 1;
	}
	return false;
}

// HEVC IRAP: BLA_W_LP..CRA_NUT (16-21). The H.264 IDR check (nal_unit_type=5)
// never matches these, so the VENC reader would drop every H.265 AU while
// waiting for a keyframe that cannot appear.
inline bool annexb_has_h265_irap(const uint8_t *data, std::size_t size)
{
	std::size_t index = 0;
	std::size_t start = 0;
	while (annexb_next_nal(data, size, &index, &start)) {
		const uint8_t type = (data[start] >> 1) & 0x3f;
		if (type >= 16 && type <= 21)
			return true;
		index = start + 1;
	}
	return false;
}

struct AnnexBBitReader {
	const uint8_t *data = nullptr;
	std::size_t size = 0;
	std::size_t byte = 0;
	int bit = 0;
	bool overflow = false;

	bool get_bit()
	{
		if (overflow || byte >= size)
			return overflow = true, false;
		const bool value = (data[byte] & (0x80u >> bit)) != 0;
		if (++bit == 8) {
			bit = 0;
			++byte;
		}
		return value;
	}

	uint32_t get_bits(int count)
	{
		uint32_t value = 0;
		for (int i = 0; i < count; ++i)
			value = (value << 1) | static_cast<uint32_t>(get_bit());
		return value;
	}

	uint32_t get_ue()
	{
		int leading = 0;
		while (!overflow && !get_bit())
			++leading;
		if (overflow || leading > 31)
			return overflow = true, 0;
		return ((1u << leading) - 1u) + get_bits(leading);
	}

	int32_t get_se()
	{
		const uint32_t code = get_ue();
		if (overflow)
			return 0;
		return (code & 1u) != 0
			? static_cast<int32_t>((code + 1u) / 2u)
			: -static_cast<int32_t>(code / 2u);
	}
};

inline bool annexb_skip_h264_scaling_list(AnnexBBitReader *reader, int size)
{
	if (reader == nullptr)
		return false;
	int last_scale = 8;
	int next_scale = 8;
	for (int i = 0; i < size; ++i) {
		if (next_scale != 0) {
			const int32_t delta = reader->get_se();
			if (reader->overflow)
				return false;
			next_scale = (last_scale + delta + 256) % 256;
		}
		last_scale = next_scale == 0 ? last_scale : next_scale;
	}
	return !reader->overflow;
}

inline bool annexb_unescape_rbsp(const uint8_t *src, std::size_t size,
	uint8_t *dst, std::size_t dst_capacity, std::size_t *out_size)
{
	if (src == nullptr || dst == nullptr || out_size == nullptr)
		return false;
	std::size_t written = 0;
	for (std::size_t i = 0; i < size; ++i) {
		if (written >= dst_capacity)
			return false;
		if (written >= 2 && dst[written - 2] == 0 &&
		    dst[written - 1] == 0 && src[i] == 3)
			continue;
		dst[written++] = src[i];
	}
	*out_size = written;
	return true;
}

inline bool annexb_parse_h264_sps_rbsp(const uint8_t *rbsp, std::size_t size,
	int *width, int *height)
{
	if (rbsp == nullptr || size < 4 || width == nullptr || height == nullptr)
		return false;
	AnnexBBitReader reader{rbsp, size, 0, 0, false};
	const uint32_t profile = reader.get_bits(8);
	(void)reader.get_bits(8);
	(void)reader.get_bits(8);
	(void)reader.get_ue();
	uint32_t chroma_format_idc = 1;
	if (profile == 100 || profile == 110 || profile == 122 || profile == 244 ||
	    profile == 44 || profile == 83 || profile == 86 || profile == 118 ||
	    profile == 128 || profile == 138 || profile == 139 || profile == 134 ||
	    profile == 135) {
		chroma_format_idc = reader.get_ue();
		if (chroma_format_idc == 3)
			(void)reader.get_bit();
		(void)reader.get_ue();
		(void)reader.get_ue();
		(void)reader.get_bit();
		if (reader.get_bit()) {
			const int lists = chroma_format_idc == 3 ? 12 : 8;
			for (int i = 0; i < lists; ++i) {
				if (!reader.get_bit())
					continue;
				if (!annexb_skip_h264_scaling_list(
					    &reader, i < 6 ? 16 : 64))
					return false;
			}
		}
	}
	(void)reader.get_ue();
	const uint32_t poc_type = reader.get_ue();
	if (poc_type == 0) {
		(void)reader.get_ue();
	} else if (poc_type == 1) {
		(void)reader.get_bit();
		(void)reader.get_se();
		(void)reader.get_se();
		const uint32_t cycle = reader.get_ue();
		if (cycle > 255)
			return false;
		for (uint32_t i = 0; i < cycle; ++i)
			(void)reader.get_se();
	} else if (poc_type != 2) {
		return false;
	}
	(void)reader.get_ue();
	(void)reader.get_bit();
	const uint32_t width_mbs_minus1 = reader.get_ue();
	const uint32_t height_map_minus1 = reader.get_ue();
	const bool frame_mbs_only = reader.get_bit();
	if (!frame_mbs_only)
		(void)reader.get_bit();
	(void)reader.get_bit();
	if (reader.overflow || width_mbs_minus1 > 512 || height_map_minus1 > 512)
		return false;
	int pic_width = static_cast<int>((width_mbs_minus1 + 1u) * 16u);
	int pic_height = static_cast<int>((height_map_minus1 + 1u) * 16u *
		(frame_mbs_only ? 1u : 2u));
	if (reader.get_bit()) {
		const uint32_t crop_left = reader.get_ue();
		const uint32_t crop_right = reader.get_ue();
		const uint32_t crop_top = reader.get_ue();
		const uint32_t crop_bottom = reader.get_ue();
		if (reader.overflow)
			return false;
		int crop_unit_x = 1;
		int crop_unit_y = frame_mbs_only ? 1 : 2;
		if (chroma_format_idc == 1) {
			crop_unit_x = 2;
			crop_unit_y = frame_mbs_only ? 2 : 4;
		} else if (chroma_format_idc == 2) {
			crop_unit_x = 2;
			crop_unit_y = frame_mbs_only ? 1 : 2;
		}
		const int cropped_w = pic_width -
			static_cast<int>(crop_left + crop_right) * crop_unit_x;
		const int cropped_h = pic_height -
			static_cast<int>(crop_top + crop_bottom) * crop_unit_y;
		if (cropped_w > 0 && cropped_h > 0) {
			pic_width = cropped_w;
			pic_height = cropped_h;
		}
	}
	if (reader.overflow || pic_width <= 0 || pic_height <= 0)
		return false;
	*width = pic_width;
	*height = pic_height;
	return true;
}

inline bool annexb_h264_sps_size(const uint8_t *data, std::size_t size,
	int *width, int *height)
{
	if (data == nullptr || width == nullptr || height == nullptr)
		return false;
	std::size_t cursor = 0;
	AnnexBNalUnit unit;
	while (annexb_next_nal_unit(data, size, &cursor, &unit)) {
		if ((data[unit.start] & 0x1fu) != 7)
			continue;
		const std::size_t payload = unit.end - unit.start;
		if (payload < 2)
			return false;
		uint8_t rbsp[160];
		std::size_t rbsp_size = 0;
		if (!annexb_unescape_rbsp(data + unit.start + 1, payload - 1,
					 rbsp, sizeof(rbsp), &rbsp_size))
			return false;
		return annexb_parse_h264_sps_rbsp(rbsp, rbsp_size, width, height);
	}
	return false;
}

inline bool annexb_h264_geometry_matches(int got_width, int got_height,
	int want_width, int want_height)
{
	if (got_width <= 0 || got_height <= 0 ||
	    want_width <= 0 || want_height <= 0)
		return false;
	const int dw = got_width - want_width;
	const int dh = got_height - want_height;
	return dw >= -16 && dw <= 16 && dh >= -16 && dh <= 16;
}
