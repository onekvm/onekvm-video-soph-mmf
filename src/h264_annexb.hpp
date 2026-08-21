#pragma once

#include <cstddef>
#include <cstdint>

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
