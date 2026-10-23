/* SPDX-License-Identifier: MIT
 * Bounded, per-channel cache of the exact pages occupied by VENC packs.
 * GetStream/ReleaseStream pin views; a later pack must not evict an earlier
 * pack from the same access unit. The kernel still invalidates DMA output
 * before publishing each stream. This cache does not replace that operation.
 */
#ifndef ONEKVM_VENC_STREAM_MAP_H
#define ONEKVM_VENC_STREAM_MAP_H

#include <stdint.h>
#include <stddef.h>
#include <unistd.h>

#define VENC_STREAM_MAP_SLOTS 256
#define VENC_STREAM_MAP_BYTES (4u * 1024u * 1024u)

struct venc_stream_mapping {
	uint64_t physical;
	size_t length;
	void *address;
	uint64_t used;
	unsigned int pins;
};

struct venc_stream_maps {
	struct venc_stream_mapping entries[VENC_STREAM_MAP_SLOTS];
	size_t bytes;
	uint64_t clock;
};

static void venc_stream_map_evict(struct venc_stream_maps *maps, int slot)
{
	struct venc_stream_mapping *entry = &maps->entries[slot];
	if (entry->address) {
		devm_unmap(entry->address, entry->length);
		maps->bytes -= entry->length;
	}
	*entry = (struct venc_stream_mapping){0};
}

static void venc_stream_maps_clear(struct venc_stream_maps *maps)
{
	int slot;
	for (slot = 0; slot < VENC_STREAM_MAP_SLOTS; ++slot)
		venc_stream_map_evict(maps, slot);
	maps->clock = 0;
}

static int venc_stream_map_oldest(struct venc_stream_maps *maps)
{
	int oldest = -1, slot;
	for (slot = 0; slot < VENC_STREAM_MAP_SLOTS; ++slot) {
		struct venc_stream_mapping *entry = &maps->entries[slot];
		if (entry->address && !entry->pins &&
		    (oldest < 0 || entry->used < maps->entries[oldest].used))
			oldest = slot;
	}
	return oldest;
}

static void *venc_stream_map_acquire(struct venc_stream_maps *maps, int fd,
	uint64_t physical, size_t length, int *token)
{
	long page_size = sysconf(_SC_PAGESIZE);
	uint64_t base;
	size_t padding, mapped;
	int slot, free_slot = -1;
	void *address;

	*token = -1;
	if (!length || physical > UINT64_MAX - length)
		return NULL;
	for (slot = 0; slot < VENC_STREAM_MAP_SLOTS; ++slot) {
		struct venc_stream_mapping *entry = &maps->entries[slot];
		if (!entry->address) {
			free_slot = slot;
			continue;
		}
		if (physical >= entry->physical &&
		    physical - entry->physical <= entry->length &&
		    length <= entry->length - (physical - entry->physical)) {
			entry->pins++;
			entry->used = ++maps->clock;
			*token = slot;
			return (unsigned char *)entry->address + (physical - entry->physical);
		}
	}
	if (page_size <= 0 || (unsigned long)page_size > SIZE_MAX / 2)
		return devm_map(fd, physical, length);
	base = physical - physical % (unsigned long)page_size;
	padding = physical - base;
	if (length > SIZE_MAX - padding - (page_size - 1))
		return NULL;
	mapped = ((length + padding + page_size - 1) / page_size) * page_size;
	if (mapped > VENC_STREAM_MAP_BYTES)
		return devm_map(fd, physical, length);
	while (maps->bytes > VENC_STREAM_MAP_BYTES - mapped || free_slot < 0) {
		slot = venc_stream_map_oldest(maps);
		if (slot < 0)
			return devm_map(fd, physical, length);
		venc_stream_map_evict(maps, slot);
		free_slot = slot;
	}
	address = devm_map(fd, base, mapped);
	if (!address)
		return NULL;
	maps->entries[free_slot] = (struct venc_stream_mapping){
		.physical = base, .length = mapped, .address = address,
		.used = ++maps->clock, .pins = 1,
	};
	maps->bytes += mapped;
	*token = free_slot;
	return (unsigned char *)address + padding;
}

static void venc_stream_map_release(struct venc_stream_maps *maps,
	int token, void *address, size_t length)
{
	if (token < 0)
		devm_unmap(address, length);
	else if (maps->entries[token].pins)
		maps->entries[token].pins--;
}

#endif
