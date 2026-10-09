// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "prop_context.h"
#include "file.h"
#include "text.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char default_context[] = "u:object_r:default_prop:s0";
static const char pre_split_context[] = "u:object_r:properties_device:s0";

static bool read_u32(const struct ksu_prop_context *context, size_t offset, uint32_t *value)
{
	if (offset > context->length || context->length - offset < 4)
		return false;
	memcpy(value, context->data + offset, 4);
	return true;
}

static uint32_t field(const struct ksu_prop_context *context, size_t offset)
{
	uint32_t value;
	return read_u32(context, offset, &value) ? value : UINT32_MAX;
}

static const char *read_string(const struct ksu_prop_context *context, size_t offset)
{
	if (offset > context->length)
		return NULL;
	const unsigned char *text = context->data + offset;
	const unsigned char *end = memchr(text, 0, context->length - offset);
	size_t length = end ? (size_t)(end - text) : context->length - offset;
	return ksu_valid_utf8(text, length) ? (const char *)text : NULL;
}

static const char *binary_label(const struct ksu_prop_context *context, size_t index)
{
	uint32_t table = field(context, 12);
	if (table >= context->length)
		return NULL;
	return read_string(context, field(context, (size_t)table + 4 + index * 4));
}

static void prefix_match(const struct ksu_prop_context *context, const unsigned char *remaining,
			 size_t remaining_length, size_t node, uint32_t *index)
{
	uint32_t count, array;
	if (!read_u32(context, node + 12, &count) || !read_u32(context, node + 16, &array))
		return;
	for (uint32_t i = 0; i < count; ++i) {
		uint32_t entry, name, length, label;
		if (!read_u32(context, (size_t)array + (size_t)i * 4, &entry) ||
		    !read_u32(context, entry, &name) ||
		    !read_u32(context, (size_t)entry + 4, &length) ||
		    !read_u32(context, (size_t)entry + 8, &label))
			return;
		if (length > remaining_length)
			continue;
		if (name > context->length || length > context->length - name)
			return;
		if (!memcmp(remaining, context->data + name, length)) {
			if (label != UINT32_MAX)
				*index = label;
			return;
		}
	}
}

static uint32_t find_child(const struct ksu_prop_context *context, size_t node,
			   const unsigned char *piece, size_t length)
{
	uint32_t count, array;
	if (!read_u32(context, node + 4, &count) || !read_u32(context, node + 8, &array))
		return UINT32_MAX;
	int64_t low = 0, high = (int64_t)(int32_t)count - 1;
	while (low <= high) {
		int64_t middle = (low + high) / 2;
		uint32_t child = field(context, (size_t)array + (size_t)middle * 4);
		const char *name = read_string(context, field(context, field(context, child)));
		if (!name)
			return UINT32_MAX;
		size_t name_length = strlen(name);
		int comparison = 0;
		for (size_t i = 0; i < length; ++i) {
			unsigned char byte = i < name_length ? (unsigned char)name[i] : 0;
			if (byte != piece[i]) {
				comparison = (byte > piece[i]) ? 1 : -1;
				break;
			}
		}
		if (!comparison && name_length > length)
			comparison = 1;
		if (!comparison)
			return child;
		if (comparison < 0)
			low = middle + 1;
		else
			high = middle - 1;
	}
	return UINT32_MAX;
}

static const char *binary_lookup(const struct ksu_prop_context *context, const unsigned char *name,
				 size_t remaining_length)
{
	uint32_t node = field(context, 20), index = UINT32_MAX;
	if (node > context->length || context->length - node < 28)
		return default_context;
	const unsigned char *remaining = name;
	for (;;) {
		uint32_t entry = field(context, node);
		uint32_t label = field(context, (size_t)entry + 8);
		if (label != UINT32_MAX)
			index = label;
		prefix_match(context, remaining, remaining_length, node, &index);
		const unsigned char *dot = memchr(remaining, '.', remaining_length);
		if (!dot)
			break;
		uint32_t child = find_child(context, node, remaining, (size_t)(dot - remaining));
		if (child == UINT32_MAX)
			break;
		node = child;
		remaining_length -= (size_t)(dot - remaining) + 1;
		remaining = dot + 1;
	}
	uint32_t count;
	if (read_u32(context, (size_t)node + 20, &count) && count) {
		uint32_t array;
		if (!read_u32(context, (size_t)node + 24, &array))
			return default_context;
		for (uint32_t i = 0; i < count; ++i) {
			uint32_t entry;
			if (!read_u32(context, (size_t)array + (size_t)i * 4, &entry))
				return default_context;
			const char *exact = read_string(context, field(context, entry));
			if (!exact)
				return default_context;
			if (strlen(exact) == remaining_length &&
			    !memcmp(exact, remaining, remaining_length)) {
				uint32_t label;
				if (!read_u32(context, (size_t)entry + 8, &label))
					return default_context;
				if (label != UINT32_MAX)
					index = label;
				goto found;
			}
		}
	}
	prefix_match(context, remaining, remaining_length, node, &index);
found:
	const char *label = index == UINT32_MAX ? NULL : binary_label(context, index);
	return label ? label : default_context;
}

static const unsigned char *next_token(const unsigned char **cursor, const unsigned char *end,
				       size_t *length)
{
	while (*cursor < end) {
		const unsigned char *next = *cursor;
		if (!ksu_unicode_space(ksu_utf8_next(&next, end)))
			break;
		*cursor = next;
	}
	const unsigned char *start = *cursor;
	while (*cursor < end) {
		const unsigned char *next = *cursor;
		if (ksu_unicode_space(ksu_utf8_next(&next, end)))
			break;
		*cursor = next;
	}
	*length = (size_t)(*cursor - start);
	return start;
}

static int load_prefixes(struct ksu_prop_context *context, const char *path)
{
	size_t length;
	unsigned char *data = ksu_read_file(path, &length);
	if (!data)
		return -1;
	int result = 0;
	size_t capacity = context->count;
	if (!ksu_valid_utf8(data, length)) {
		errno = EILSEQ;
		result = -1;
		goto out;
	}
	const unsigned char *cursor = data, *end = data + length;
	while (cursor < end) {
		const unsigned char *line = cursor;
		while (cursor < end && *cursor != '\n')
			++cursor;
		const unsigned char *line_end = memchr(line, '#', (size_t)(cursor - line));
		if (!line_end)
			line_end = cursor;
		if (cursor < end)
			++cursor;
		size_t prefix_length, label_length;
		const unsigned char *prefix = next_token(&line, line_end, &prefix_length);
		const unsigned char *label = next_token(&line, line_end, &label_length);
		if (!prefix_length || !label_length ||
		    (prefix_length >= 4 && !memcmp(prefix, "ctl.", 4)))
			continue;
		if (context->count == capacity) {
			if (capacity > SIZE_MAX / 2 / sizeof(*context->prefixes)) {
				errno = EOVERFLOW;
				result = -1;
				break;
			}
			capacity = capacity ? capacity * 2 : 16;
			struct ksu_prop_prefix *grown =
			    realloc(context->prefixes, capacity * sizeof(*grown));
			if (!grown) {
				result = -1;
				break;
			}
			context->prefixes = grown;
		}
		size_t prefix_size = strnlen((const char *)prefix, prefix_length);
		size_t label_size = strnlen((const char *)label, label_length);
		struct ksu_prop_prefix entry = {.data = malloc(prefix_size + label_size + 2),
						.length = prefix_size};
		if (!entry.data) {
			result = -1;
			break;
		}
		memcpy(entry.data, prefix, prefix_size);
		entry.data[prefix_size] = 0;
		memcpy(entry.data + prefix_size + 1, label, label_size);
		entry.data[prefix_size + 1 + label_size] = 0;
		size_t position = 0;
		while (position < context->count &&
		       !(context->prefixes[position].length == 1 &&
			 context->prefixes[position].data[0] == '*') &&
		       context->prefixes[position].length >= prefix_length)
			++position;
		memmove(context->prefixes + position + 1, context->prefixes + position,
			(context->count - position) * sizeof(entry));
		context->prefixes[position] = entry;
		++context->count;
	}
out:
	int error = errno;
	free(data);
	errno = error;
	return result;
}

static bool path_exists(const char *root, const char *name)
{
	char *path = ksu_join_path(root, name);
	if (!path)
		return false;
	struct stat st;
	bool exists = !stat(path, &st);
	free(path);
	return exists;
}

static int load_prefix_file(struct ksu_prop_context *context, const char *root, const char *name)
{
	char *path = ksu_join_path(root, name);
	if (!path)
		return -1;
	int result = load_prefixes(context, path), error = errno;
	free(path);
	errno = error;
	return result;
}

void ksu_prop_context_free(struct ksu_prop_context *context)
{
	for (size_t i = 0; i < context->count; ++i)
		free(context->prefixes[i].data);
	free(context->prefixes);
	free(context->directory);
	free(context->data);
	*context = (struct ksu_prop_context){0};
}

int ksu_prop_context_load(struct ksu_prop_context *context, const char *directory,
			  const char *system_root)
{
	*context = (struct ksu_prop_context){0};
	struct stat st;
	if (stat(directory, &st))
		return -1;
	context->directory = strdup(directory);
	if (!context->directory)
		return -1;
	if (!S_ISDIR(st.st_mode)) {
		context->type = KSU_PROP_PRE_SPLIT;
		return 0;
	}
	if (path_exists(directory, "property_info")) {
		char *path = ksu_join_path(directory, "property_info");
		if (!path)
			goto error;
		context->data = ksu_read_file(path, &context->length);
		int saved = errno;
		free(path);
		errno = saved;
		if (!context->data)
			goto error;
		context->data[context->length] = 0;
		if (context->length < 24 || field(context, 4) > 2) {
			errno = EINVAL;
			goto error;
		}
		context->type = KSU_PROP_SERIALIZED;
		return 0;
	}
	context->type = KSU_PROP_SPLIT;
	const char *root = system_root ? system_root : "/";
	if (path_exists(root, "property_contexts")) {
		if (load_prefix_file(context, root, "property_contexts"))
			goto error;
		return 0;
	}
	const char *platform, *vendor, *nonplatform;
	if (path_exists(root, "system/etc/selinux/plat_property_contexts")) {
		platform = "system/etc/selinux/plat_property_contexts";
		vendor = "vendor/etc/selinux/vendor_property_contexts";
		nonplatform = "vendor/etc/selinux/nonplat_property_contexts";
	} else {
		platform = "plat_property_contexts";
		vendor = "vendor_property_contexts";
		nonplatform = "nonplat_property_contexts";
	}
	if (load_prefix_file(context, root, platform))
		goto error;
	(void)load_prefix_file(context, root, path_exists(root, vendor) ? vendor : nonplatform);
	return 0;
error:
	int error = errno;
	ksu_prop_context_free(context);
	errno = error;
	return -1;
}

const char *ksu_prop_context_get_n(const struct ksu_prop_context *context,
				   const unsigned char *name, size_t length)
{
	if (context->type == KSU_PROP_PRE_SPLIT)
		return pre_split_context;
	if (context->type == KSU_PROP_SERIALIZED)
		return binary_lookup(context, name, length);
	for (size_t i = 0; i < context->count; ++i) {
		const struct ksu_prop_prefix *prefix = context->prefixes + i;
		if ((prefix->length == 1 && prefix->data[0] == '*') ||
		    (prefix->length <= length && !memcmp(name, prefix->data, prefix->length)))
			return prefix->data + prefix->length + 1;
	}
	return default_context;
}

const char **ksu_prop_context_labels(const struct ksu_prop_context *context, size_t *count)
{
	*count = 0;
	size_t capacity = context->type == KSU_PROP_PRE_SPLIT ? 1 : context->count;
	if (context->type == KSU_PROP_SERIALIZED) {
		uint32_t table = field(context, 12), declared;
		capacity = read_u32(context, table, &declared) ? declared : 0;
		if (table < context->length && capacity > (context->length - table) / 4)
			capacity = (context->length - table) / 4;
	}
	const char **labels = calloc(capacity ? capacity : 1, sizeof(*labels));
	if (!labels)
		return NULL;
	for (size_t i = 0; i < capacity; ++i) {
		const char *label =
		    context->type == KSU_PROP_PRE_SPLIT ? pre_split_context
		    : context->type == KSU_PROP_SERIALIZED
			? binary_label(context, i)
			: context->prefixes[i].data + context->prefixes[i].length + 1;
		if (!label)
			continue;
		if (context->type == KSU_PROP_SPLIT) {
			size_t j = 0;
			while (j < *count && strcmp(labels[j], label))
				++j;
			if (j != *count)
				continue;
		}
		labels[(*count)++] = label;
	}
	return labels;
}

char *ksu_prop_context_path(const struct ksu_prop_context *context, const char *name)
{
	return context->type == KSU_PROP_PRE_SPLIT ? strdup(context->directory)
						   : ksu_join_path(context->directory, name);
}
