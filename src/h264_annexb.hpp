#pragma once

#include <cstddef>
#include <cstdint>

inline bool annexb_has_idr(const uint8_t *data, std::size_t size)
{
	if (data == nullptr || size < 4)
		return false;
	std::size_t index = 0;
	while (index + 3 < size) {
		std::size_t start = 0;
		if (data[index] == 0 && data[index + 1] == 0 && data[index + 2] == 1) {
			start = index + 3;
		} else if (index + 4 <= size && data[index] == 0 &&
			   data[index + 1] == 0 && data[index + 2] == 0 &&
			   data[index + 3] == 1) {
			start = index + 4;
		} else {
			++index;
			continue;
		}
		if (start >= size)
			break;
		if ((data[start] & 0x1fu) == 5)
			return true;
		index = start + 1;
	}
	return false;
}
