/* SPDX-License-Identifier: MIT */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

static unsigned int mapped, unmapped;
static int fail_mapping;
static void *devm_map(int fd, uint64_t physical, size_t length)
{
	(void)fd;
	(void)physical;
	if (fail_mapping)
		return NULL;
	mapped++;
	return calloc(1, length);
}
static void devm_unmap(void *address, size_t length)
{
	(void)length;
	unmapped++;
	free(address);
}

#include "../oe-recipes/recipes-onekvm/onekvm-video-soph-mmf/files/mmf-runtime/venc_stream_map.h"

int main(void)
{
	struct venc_stream_maps maps = {0};
	int first, second, other, tokens[VENC_STREAM_MAP_SLOTS];
	unsigned int index;
	void *address = venc_stream_map_acquire(&maps, 0, 0x1003, 8, &first);
	assert(address && first >= 0 && maps.bytes == (size_t)sysconf(_SC_PAGESIZE));
	assert(venc_stream_map_acquire(&maps, 0, 0x1005, 4, &second) == (char *)address + 2);
	assert(first == second && maps.entries[first].pins == 2 && mapped == 1);
	venc_stream_map_release(&maps, second, (char *)address + 2, 4);
	venc_stream_map_release(&maps, first, address, 8);
	assert(unmapped == 0);
	assert(venc_stream_map_acquire(&maps, 0, 0x1003, 8, &first) == address);
	/* A larger overlapping pack cannot invalidate an earlier pinned view. */
	assert(venc_stream_map_acquire(&maps, 0, 0x1003, 8192, &other));
	assert(other != first && maps.entries[first].pins == 1);
	venc_stream_map_release(&maps, other, NULL, 0);
	venc_stream_map_release(&maps, first, address, 8);
	venc_stream_maps_clear(&maps);
	assert(mapped == unmapped && maps.bytes == 0);
	/* Pin every slot, then fall back without evicting live packs. */
	for (index = 0; index < VENC_STREAM_MAP_SLOTS; ++index)
		assert(venc_stream_map_acquire(&maps, 0, 0x10000 + index * 0x10000, 1, &tokens[index]));
	address = venc_stream_map_acquire(&maps, 0, 0x10000000, 8, &other);
	assert(address && other == -1 && unmapped == 2);
	venc_stream_map_release(&maps, other, address, 8);
	for (index = 0; index < VENC_STREAM_MAP_SLOTS; ++index)
		venc_stream_map_release(&maps, tokens[index], NULL, 0);
	assert(venc_stream_map_acquire(&maps, 0, 0x10000000, VENC_STREAM_MAP_BYTES, &first));
	assert(maps.bytes <= VENC_STREAM_MAP_BYTES);
	venc_stream_map_release(&maps, first, NULL, 0);
	venc_stream_maps_clear(&maps);
	assert(mapped == unmapped);
	fail_mapping = 1;
	assert(!venc_stream_map_acquire(&maps, 0, 0x1003, 1, &first));
	assert(first == -1 && maps.bytes == 0);
	assert(!venc_stream_map_acquire(&maps, 0, UINT64_MAX - 1, 8, &first));
	return 0;
}
