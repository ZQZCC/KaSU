// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "prop_area.h"
#include "file.h"
#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

struct prop_trie {
	uint32_t name_length;
	_Atomic uint32_t prop, left, right, children;
	unsigned char name[];
};

struct prop_info {
	_Atomic uint32_t serial;
	unsigned char value[KSU_PROP_VALUE_MAX];
	unsigned char name[];
};

struct long_property {
	unsigned char error[56];
	uint32_t offset;
};

_Static_assert(sizeof(struct prop_trie) == 20, "property trie layout");
_Static_assert(sizeof(struct prop_info) == 96, "property info layout");
_Static_assert(sizeof(struct long_property) == 60, "long property layout");

static const unsigned char long_error[] = "Must use __system_property_read_callback() to read";

static void *invalid(void)
{
	errno = EINVAL;
	return NULL;
}

static void *data_at(const struct ksu_prop_area *area, uint32_t offset, size_t size)
{
	if (offset > area->capacity || size > area->capacity - offset)
		return invalid();
	return area->base + KSU_PROP_AREA_HEADER + offset;
}

static struct prop_trie *trie_at(const struct ksu_prop_area *area, uint32_t offset)
{
	if (offset & 3)
		return invalid();
	return data_at(area, offset, sizeof(struct prop_trie));
}

static struct prop_info *info_at(const struct ksu_prop_area *area, uint32_t offset)
{
	if (offset & 3)
		return invalid();
	return data_at(area, offset, sizeof(struct prop_info));
}

static _Atomic uint32_t *used(const struct ksu_prop_area *area)
{
	return (_Atomic uint32_t *)area->base;
}

static _Atomic uint32_t *serial(const struct ksu_prop_area *area)
{
	return (_Atomic uint32_t *)(area->base + 4);
}

int ksu_prop_area_init(struct ksu_prop_area *area, void *memory, size_t length)
{
	if (length < KSU_PROP_AREA_HEADER || ((uintptr_t)memory & 3) ||
	    length - KSU_PROP_AREA_HEADER > UINT32_MAX ||
	    atomic_load_explicit((_Atomic uint32_t *)memory + 2, memory_order_relaxed) !=
		0x504f5250 ||
	    atomic_load_explicit((_Atomic uint32_t *)memory + 3, memory_order_relaxed) !=
		0xfc6ed0ab) {
		invalid();
		return -1;
	}
	*area = (struct ksu_prop_area){memory, length, (uint32_t)(length - KSU_PROP_AREA_HEADER)};
	return 0;
}

int ksu_prop_area_empty(struct ksu_prop_area *area, void *memory, size_t length, bool dirty_backup)
{
	size_t initial = sizeof(struct prop_trie) + (dirty_backup ? KSU_PROP_VALUE_MAX : 0);
	if (length < KSU_PROP_AREA_HEADER + initial || ((uintptr_t)memory & 3)) {
		invalid();
		return -1;
	}
	memset(memory, 0, length);
	uint32_t *header = memory;
	header[0] = (uint32_t)initial;
	header[2] = 0x504f5250;
	header[3] = 0xfc6ed0ab;
	return ksu_prop_area_init(area, memory, length);
}

static uint32_t allocate(struct ksu_prop_area *area, size_t length)
{
	uint32_t offset = atomic_load_explicit(used(area), memory_order_relaxed);
	if (length > UINT32_MAX - 3 || offset > area->capacity ||
	    ((length + 3) & ~(size_t)3) > area->capacity - offset) {
		errno = ENOSPC;
		return UINT32_MAX;
	}
	atomic_store_explicit(used(area), offset + (uint32_t)((length + 3) & ~(size_t)3),
			      memory_order_relaxed);
	return offset;
}

static uint32_t new_trie(struct ksu_prop_area *area, const unsigned char *name, size_t length)
{
	uint32_t offset = allocate(area, sizeof(struct prop_trie) + length + 1);
	if (offset == UINT32_MAX)
		return offset;
	struct prop_trie *node = trie_at(area, offset);
	node->name_length = (uint32_t)length;
	atomic_store_explicit(&node->prop, 0, memory_order_relaxed);
	atomic_store_explicit(&node->left, 0, memory_order_relaxed);
	atomic_store_explicit(&node->right, 0, memory_order_relaxed);
	atomic_store_explicit(&node->children, 0, memory_order_relaxed);
	memcpy(node->name, name, length);
	node->name[length] = 0;
	return offset;
}

static int traverse(struct ksu_prop_area *area, const unsigned char *name, size_t length,
		    bool create, uint32_t *result)
{
	uint32_t current = 0;
	*result = UINT32_MAX;
	for (;;) {
		const unsigned char *dot = memchr(name, '.', length);
		size_t piece = dot ? (size_t)(dot - name) : length;
		if (!piece) {
			invalid();
			return -1;
		}
		struct prop_trie *parent = trie_at(area, current);
		if (!parent)
			return -1;
		_Atomic uint32_t *link = &parent->children;
		for (;;) {
			uint32_t next = atomic_load_explicit(link, memory_order_acquire);
			if (!next) {
				if (!create)
					return 0;
				next = new_trie(area, name, piece);
				if (next == UINT32_MAX)
					return -1;
				atomic_store_explicit(link, next, memory_order_release);
			}
			struct prop_trie *node = trie_at(area, next);
			if (!node || !data_at(area, next + sizeof(*node), node->name_length))
				return -1;
			int order = (piece > node->name_length) - (piece < node->name_length);
			if (!order)
				order = memcmp(name, node->name, piece);
			if (!order) {
				current = next;
				break;
			}
			link = order < 0 ? &node->left : &node->right;
		}
		if (!dot) {
			*result = current;
			return 0;
		}
		name += piece + 1;
		length -= piece + 1;
	}
}

int ksu_prop_area_find(const struct ksu_prop_area *area, const unsigned char *name, size_t length,
		       uint32_t *offset)
{
	struct ksu_prop_area view = *area;
	uint32_t node;
	*offset = 0;
	if (traverse(&view, name, length, false, &node))
		return -1;
	if (node != UINT32_MAX)
		*offset = atomic_load_explicit(&trie_at(area, node)->prop, memory_order_acquire);
	return 0;
}

static void write_long(struct ksu_prop_area *area, struct prop_info *info, uint32_t offset,
		       uint32_t storage, const unsigned char *value, size_t length)
{
	unsigned char *data = data_at(area, storage, length + 1);
	memcpy(data, value, length);
	data[length] = 0;
	struct long_property *property = (struct long_property *)info->value;
	memcpy(property->error, long_error, sizeof(long_error));
	property->offset = storage - offset;
}

static uint32_t new_info(struct ksu_prop_area *area, const unsigned char *name, size_t name_length,
			 const unsigned char *value, size_t value_length, uint32_t counter)
{
	uint32_t offset = allocate(area, sizeof(struct prop_info) + name_length + 1);
	if (offset == UINT32_MAX)
		return offset;
	uint32_t storage = 0;
	bool is_long = value_length >= KSU_PROP_VALUE_MAX;
	if (is_long) {
		storage = allocate(area, value_length + 1);
		if (storage == UINT32_MAX)
			return UINT32_MAX;
	}
	struct prop_info *info = info_at(area, offset);
	if (is_long)
		write_long(area, info, offset, storage, value, value_length);
	else {
		memcpy(info->value, value, value_length);
		info->value[value_length] = 0;
	}
	memcpy(info->name, name, name_length);
	info->name[name_length] = 0;
	uint32_t value_serial = is_long ? ((sizeof(long_error) - 1) << 24) | KSU_PROP_LONG_FLAG
					: (uint32_t)value_length << 24;
	atomic_store_explicit(&info->serial, value_serial | (counter << 1), memory_order_relaxed);
	return offset;
}

int ksu_prop_area_emplace(struct ksu_prop_area *area, const unsigned char *name, size_t name_length,
			  const unsigned char *value, size_t value_length, uint32_t counter)
{
	uint32_t node;
	if (traverse(area, name, name_length, true, &node))
		return -1;
	uint32_t offset = new_info(area, name, name_length, value, value_length, counter);
	if (offset == UINT32_MAX)
		return -1;
	atomic_store_explicit(&trie_at(area, node)->prop, offset, memory_order_release);
	return 0;
}

static void wake(_Atomic uint32_t *address)
{
	syscall(SYS_futex, address, FUTEX_WAKE, INT_MAX, NULL);
}

void ksu_prop_area_bump(struct ksu_prop_area *area)
{
	uint32_t value = atomic_load_explicit(serial(area), memory_order_relaxed);
	atomic_store_explicit(serial(area), value + 1, memory_order_release);
	wake(serial(area));
}

int ksu_prop_area_set(struct ksu_prop_area *area, struct ksu_prop_area *serial_area,
		      const unsigned char *name, size_t name_length, const unsigned char *value,
		      size_t value_length, bool *need_rebuild)
{
	uint32_t offset;
	if (ksu_prop_area_find(area, name, name_length, &offset))
		return -1;
	if (!offset) {
		if (ksu_prop_area_emplace(area, name, name_length, value, value_length, 0))
			return -1;
		ksu_prop_area_bump(serial_area);
		return 0;
	}
	struct prop_info *info = info_at(area, offset);
	if (!info)
		return -1;
	uint32_t old = atomic_load_explicit(&info->serial, memory_order_relaxed);
	bool is_long = value_length >= KSU_PROP_VALUE_MAX;
	*need_rebuild = is_long || (old & KSU_PROP_LONG_FLAG);
	uint32_t storage = 0;
	if (is_long) {
		storage = allocate(area, value_length + 1);
		if (storage == UINT32_MAX)
			return -1;
	}
	uint32_t dirty = old | 1;
	atomic_store_explicit(&info->serial, dirty, memory_order_relaxed);
	if (is_long)
		write_long(area, info, offset, storage, value, value_length);
	else {
		memcpy(info->value, value, value_length);
		memset(info->value + value_length, 0, KSU_PROP_VALUE_MAX - value_length);
	}
	uint32_t flags = is_long ? ((sizeof(long_error) - 1) << 24) | KSU_PROP_LONG_FLAG
				 : (uint32_t)value_length << 24;
	uint32_t counter_mask = 0x00ffffff & ~KSU_PROP_LONG_FLAG;
	atomic_thread_fence(memory_order_release);
	atomic_store_explicit(&info->serial, flags | ((dirty + 1) & counter_mask),
			      memory_order_relaxed);
	wake(&info->serial);
	ksu_prop_area_bump(serial_area);
	atomic_thread_fence(memory_order_release);
	atomic_store_explicit(&info->serial, flags | ((dirty & ~1U) & counter_mask),
			      memory_order_relaxed);
	return 0;
}

static const unsigned char *string_at(const struct ksu_prop_area *area, uint32_t offset,
				      size_t *length)
{
	const unsigned char *data = data_at(area, offset, 1);
	if (!data)
		return NULL;
	const unsigned char *end = memchr(data, 0, area->capacity - offset);
	if (!end)
		return invalid();
	*length = (size_t)(end - data);
	return data;
}

int ksu_prop_area_remove(struct ksu_prop_area *area, const unsigned char *name, size_t length)
{
	uint32_t node;
	if (traverse(area, name, length, false, &node))
		return -1;
	if (node == UINT32_MAX)
		return 0;
	struct prop_trie *trie = trie_at(area, node);
	uint32_t offset = atomic_load_explicit(&trie->prop, memory_order_acquire);
	if (!offset)
		return 0;
	struct prop_info *info = info_at(area, offset);
	if (!info)
		return -1;
	uint32_t value = atomic_load_explicit(&info->serial, memory_order_relaxed);
	atomic_store_explicit(&trie->prop, 0, memory_order_release);
	if (value & KSU_PROP_LONG_FLAG) {
		struct long_property *property = (struct long_property *)info->value;
		size_t size;
		const unsigned char *long_value = string_at(area, offset + property->offset, &size);
		if (!long_value)
			return -1;
		memset((void *)long_value, 0, size);
	}
	size_t size;
	const unsigned char *prop_name = string_at(area, offset + sizeof(*info), &size);
	if (!prop_name)
		return -1;
	memset((void *)prop_name, 0, size);
	memset(info, 0, sizeof(*info));
	return 1;
}

int ksu_prop_area_read(const struct ksu_prop_area *area, uint32_t offset,
		       struct ksu_prop_record *record)
{
	*record = (struct ksu_prop_record){0};
	struct prop_info *info = info_at(area, offset);
	if (!info)
		return -1;
	size_t name_length, length;
	const unsigned char *name = string_at(area, offset + sizeof(*info), &name_length);
	if (!name || !ksu_valid_utf8(name, name_length)) {
		invalid();
		return -1;
	}
	bool immutable = name_length >= 3 && !memcmp(name, "ro.", 3);
	const unsigned char *value;
	unsigned char copy[256];
	uint32_t current;
	if (immutable) {
		current = atomic_load_explicit(&info->serial, memory_order_relaxed);
		if (current & KSU_PROP_LONG_FLAG) {
			struct long_property *property = (struct long_property *)info->value;
			value = string_at(area, offset + property->offset, &length);
		} else {
			length = current >> 24;
			value = data_at(area, offset + sizeof(current), length);
		}
		if (!value)
			return -1;
	} else {
		uint32_t next = atomic_load_explicit(&info->serial, memory_order_acquire);
		for (;;) {
			current = next;
			if (current & 1) {
				syscall(SYS_futex, &info->serial, FUTEX_WAIT, current, NULL);
				next = atomic_load_explicit(&info->serial, memory_order_relaxed);
				continue;
			}
			length = current >> 24;
			value = data_at(area, offset + sizeof(current), length);
			if (!value)
				return -1;
			memcpy(copy, value, length);
			atomic_thread_fence(memory_order_acquire);
			next = atomic_load_explicit(&info->serial, memory_order_relaxed);
			if (next == current)
				break;
			atomic_thread_fence(memory_order_acquire);
		}
		value = copy;
	}
	record->data = malloc(name_length + length + 1);
	if (!record->data)
		return -1;
	memcpy(record->data, name, name_length + 1);
	memcpy(record->data + name_length + 1, value, length);
	record->pair = (struct ksu_string_pair){record->data, name_length,
						record->data + name_length + 1, length};
	record->counter =
	    (current & 0x00ffffff & (immutable ? ~KSU_PROP_LONG_FLAG : UINT32_MAX)) >> 1;
	return 0;
}

enum { SEEN_NODE = 1, SEEN_PROP = 2, COVERED = 4 };

struct area_scan {
	const struct ksu_prop_area *area;
	unsigned char *slots;
	uint32_t used;
	bool check, abnormal;
};

static int cover(struct area_scan *scan, uint32_t offset, size_t length)
{
	if ((offset & 3) || length > UINT32_MAX - 3 || offset > scan->used ||
	    ((length + 3) & ~(size_t)3) > scan->used - offset) {
		invalid();
		return -1;
	}
	for (size_t i = offset / 4; i < (offset + length + 3) / 4; ++i)
		scan->slots[i] |= COVERED;
	return 0;
}

static int scan_property(struct area_scan *scan, uint32_t offset)
{
	const struct ksu_prop_area *area = scan->area;
	struct prop_info *info = info_at(area, offset);
	if (!info)
		return -1;
	if (scan->slots[offset / 4] & SEEN_PROP)
		return 0;
	scan->slots[offset / 4] |= SEEN_PROP;
	if (!scan->check)
		return 0;
	size_t name_length, value_length;
	const unsigned char *name = string_at(area, offset + sizeof(*info), &name_length);
	if (!name || !ksu_valid_utf8(name, name_length) ||
	    cover(scan, offset, sizeof(*info) + name_length + 1))
		return -1;
	uint32_t value_serial = atomic_load_explicit(&info->serial, memory_order_relaxed);
	bool is_long = value_serial & KSU_PROP_LONG_FLAG;
	uint32_t value_offset = offset + sizeof(value_serial);
	const unsigned char *value;
	if (is_long) {
		struct long_property *property = (struct long_property *)info->value;
		if (property->offset < ((sizeof(*info) + name_length + 4) & ~(size_t)3) ||
		    property->offset > UINT32_MAX - offset) {
			invalid();
			return -1;
		}
		value_offset = offset + property->offset;
		value = string_at(area, value_offset, &value_length);
		if (!value || cover(scan, value_offset, value_length + 1))
			return -1;
	} else {
		const unsigned char *end = memchr(info->value, 0, KSU_PROP_VALUE_MAX);
		if (!end) {
			invalid();
			return -1;
		}
		value = info->value;
		value_length = (size_t)(end - value);
	}
	if (!ksu_valid_utf8(value, value_length)) {
		invalid();
		return -1;
	}
	bool immutable = name_length >= 3 && !memcmp(name, "ro.", 3);
	size_t value_size = value_length + 1;
	size_t value_max = is_long ? (value_size + 3) & ~(size_t)3 : KSU_PROP_VALUE_MAX;
	if (immutable && value_size < value_max) {
		const unsigned char *padding =
		    data_at(area, value_offset + (uint32_t)value_size, value_max - value_size);
		if (!padding)
			return -1;
		for (size_t i = 0; i < value_max - value_size; ++i)
			scan->abnormal |= padding[i] != 0;
	}
	scan->abnormal |=
	    is_long && value_offset != ((offset + sizeof(*info) + name_length + 4) & ~(size_t)3);
	scan->abnormal |= immutable && (value_serial & 0x00ffffff & ~KSU_PROP_LONG_FLAG) >> 1;
	return 0;
}

static int scan_node(struct area_scan *scan, uint32_t offset)
{
	struct prop_trie *node = trie_at(scan->area, offset);
	if (!node)
		return -1;
	if (scan->slots[offset / 4] & SEEN_NODE)
		return 0;
	scan->slots[offset / 4] |= SEEN_NODE;
	uint32_t prop = atomic_load_explicit(&node->prop, memory_order_acquire);
	uint32_t children = atomic_load_explicit(&node->children, memory_order_acquire);
	uint32_t left = atomic_load_explicit(&node->left, memory_order_acquire);
	uint32_t right = atomic_load_explicit(&node->right, memory_order_acquire);
	if (scan->check) {
		size_t length = sizeof(*node);
		if (node->name_length) {
			const unsigned char *name = data_at(scan->area, offset + sizeof(*node),
							    (size_t)node->name_length + 1);
			if (!name)
				return -1;
			const unsigned char *end = memchr(name, 0, (size_t)node->name_length + 1);
			if (!end || !ksu_valid_utf8(name, (size_t)(end - name))) {
				invalid();
				return -1;
			}
			length += (size_t)node->name_length + 1;
		}
		if (cover(scan, offset, length))
			return -1;
		scan->abnormal |= !prop && !children && offset;
		scan->abnormal |= (left && left <= offset) || (right && right <= offset) ||
				  (children && children <= offset) || (prop && prop <= offset);
		scan->abnormal |=
		    !children && prop &&
		    prop != offset + sizeof(*node) + (((size_t)node->name_length + 4) & ~(size_t)3);
	}
	if ((prop && scan_property(scan, prop)) || (children && scan_node(scan, children)) ||
	    (left && scan_node(scan, left)) || (right && scan_node(scan, right)))
		return -1;
	return 0;
}

int ksu_prop_area_rebuild(struct ksu_prop_area *area, bool check)
{
	struct prop_trie *root = trie_at(area, 0);
	uint32_t old_used = atomic_load_explicit(used(area), memory_order_relaxed);
	if (!root || old_used > area->capacity) {
		invalid();
		return -1;
	}
	uint32_t children = atomic_load_explicit(&root->children, memory_order_acquire);
	bool dirty_backup =
	    children ? children != sizeof(*root) : old_used == sizeof(*root) + KSU_PROP_VALUE_MAX;
	struct area_scan scan = {.area = area, .used = old_used, .check = check};
	scan.slots = calloc(((size_t)area->capacity + 3) / 4, 1);
	if (!scan.slots)
		return -1;
	unsigned char *memory = NULL;
	int result = -1;
	if ((check && dirty_backup && cover(&scan, sizeof(*root), KSU_PROP_VALUE_MAX)) ||
	    scan_node(&scan, 0))
		goto done;
	if (check) {
		for (size_t i = 0; i < ((size_t)old_used + 3) / 4; ++i)
			scan.abnormal |= !(scan.slots[i] & COVERED);
		if (!scan.abnormal) {
			result = 0;
			goto done;
		}
	}
	memory = malloc(area->length);
	struct ksu_prop_area rebuilt;
	if (!memory || ksu_prop_area_empty(&rebuilt, memory, area->length, dirty_backup))
		goto done;
	/* Allocation order is the compatibility key; no owned property map is needed. */
	for (size_t i = 0; i < ((size_t)area->capacity + 3) / 4; ++i) {
		if (!(scan.slots[i] & SEEN_PROP))
			continue;
		struct ksu_prop_record record;
		if (ksu_prop_area_read(area, (uint32_t)(i * 4), &record))
			goto done;
		int status = ksu_prop_area_emplace(&rebuilt, record.pair.key,
						   record.pair.key_length, record.pair.value,
						   record.pair.value_length, record.counter);
		free(record.data);
		if (status)
			goto done;
	}
	size_t size =
	    KSU_PROP_AREA_HEADER + atomic_load_explicit(used(&rebuilt), memory_order_relaxed);
	memcpy(area->base, memory, size);
	if (old_used + (size_t)KSU_PROP_AREA_HEADER > size)
		memset(area->base + size, 0, old_used + KSU_PROP_AREA_HEADER - size);
	result = 0;
done:
	int saved = errno;
	free(memory);
	free(scan.slots);
	errno = saved;
	return result;
}
