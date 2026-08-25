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
