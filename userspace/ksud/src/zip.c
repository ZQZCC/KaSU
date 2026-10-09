// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "file.h"
#include "native.h"
#include "text.h"

#include "zlib.h"

#include "infback9.h"
#include "mz.h"
#include "mz_strm.h"
#include "mz_strm_os.h"
#include "mz_zip.h"

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct zip_entry {
	char *name;
	size_t length, order;
	int64_t position;
	bool utf8;
};

struct ksu_zip {
	void *zip, *stream;
	struct zip_entry *entries;
	size_t count;
};

struct zip_output {
	FILE *file;
	unsigned char *data;
	size_t length, capacity;
	uint32_t crc;
};

static int zip_error(int code)
{
	if (code == MZ_OK)
		return 0;
	if (code == MZ_MEM_ERROR)
		errno = ENOMEM;
	else if (code == MZ_END_OF_LIST || code == MZ_EXIST_ERROR)
		errno = ENOENT;
	else if (code == MZ_SUPPORT_ERROR)
		errno = ENOTSUP;
	else if (code == MZ_FORMAT_ERROR || code == MZ_CRC_ERROR || code == MZ_DATA_ERROR)
		errno = EINVAL;
	else if (!errno)
		errno = EIO;
	return -1;
}

void ksu_zip_close(struct ksu_zip *archive)
{
	if (!archive)
		return;
	int error = errno;
	for (size_t i = 0; i < archive->count; i++)
		free(archive->entries[i].name);
	free(archive->entries);
	if (archive->zip) {
		mz_zip_close(archive->zip);
		mz_zip_delete(&archive->zip);
	}
	if (archive->stream) {
		mz_stream_os_close(archive->stream);
		mz_stream_os_delete(&archive->stream);
	}
	free(archive);
	errno = error;
}

static int compare_name(const void *left, const void *right)
{
	const struct zip_entry *a = left, *b = right;
	size_t length = a->length < b->length ? a->length : b->length;
	int order = memcmp(a->name, b->name, length);
	if (order)
		return order;
	if (a->length != b->length)
		return a->length < b->length ? -1 : 1;
	return (a->order > b->order) - (a->order < b->order);
}

static int compare_order(const void *left, const void *right)
{
	const struct zip_entry *a = left, *b = right;
	return (a->order > b->order) - (a->order < b->order);
}

static char *raw_name(mz_zip_file *file, size_t *length, bool *utf8)
{
	const unsigned char *name = (const unsigned char *)file->filename;
	*length = file->filename_size;
	*utf8 = (file->flag & MZ_ZIP_FLAG_UTF8) != 0;
	for (size_t offset = 0; offset + 4 <= file->extrafield_size;) {
		uint16_t type, size;
		memcpy(&type, file->extrafield + offset, sizeof(type));
		memcpy(&size, file->extrafield + offset + 2, sizeof(size));
		size = le16toh(size);
		offset += 4;
		if (size > file->extrafield_size - offset)
			goto invalid;
		if (le16toh(type) == 0x7075) {
			uint32_t crc;
			if (size < 5)
				goto invalid;
			memcpy(&crc, file->extrafield + offset + 1, sizeof(crc));
			if (le32toh(crc) != (uint32_t)crc32(0, name, (unsigned)*length))
				goto invalid;
			name = file->extrafield + offset + 5;
			*length = size - 5;
			*utf8 = true;
			if (!ksu_valid_utf8(name, *length)) {
				errno = EILSEQ;
				return NULL;
			}
		}
		offset += size;
	}
	char *copy = malloc(*length + 1);
	if (copy) {
		memcpy(copy, name, *length);
		copy[*length] = 0;
	}
	return copy;
invalid:
	errno = EINVAL;
	return NULL;
}

struct ksu_zip *ksu_zip_open(const char *path)
{
	struct ksu_zip *archive = calloc(1, sizeof(*archive));
	if (!archive)
		return NULL;
	archive->stream = mz_stream_os_create();
	archive->zip = mz_zip_create();
	if (!archive->stream || !archive->zip) {
		errno = ENOMEM;
		goto fail;
	}
	if (zip_error(mz_stream_os_open(archive->stream, path, MZ_OPEN_MODE_READ)) < 0 ||
	    zip_error(mz_zip_open(archive->zip, archive->stream, MZ_OPEN_MODE_READ)) < 0)
		goto fail;
	size_t capacity = 0;
	int code = mz_zip_goto_first_entry(archive->zip);
	while (code == MZ_OK) {
		mz_zip_file *file;
		if (zip_error(mz_zip_entry_get_info(archive->zip, &file)) < 0)
			goto fail;
		if (archive->count == capacity) {
			if (capacity > SIZE_MAX / 2 / sizeof(*archive->entries)) {
				errno = EOVERFLOW;
				goto fail;
			}
			capacity = capacity ? capacity * 2 : 16;
			struct zip_entry *entries =
			    realloc(archive->entries, capacity * sizeof(*entries));
			if (!entries)
				goto fail;
			archive->entries = entries;
		}
		size_t length;
		bool utf8;
		char *name = raw_name(file, &length, &utf8);
		if (!name)
			goto fail;
		archive->entries[archive->count] = (struct zip_entry){
		    .name = name,
		    .length = length,
		    .order = archive->count,
		    .position = mz_zip_get_entry(archive->zip),
		    .utf8 = utf8,
		};
		archive->count++;
		code = mz_zip_goto_next_entry(archive->zip);
	}
	if (code != MZ_END_OF_LIST) {
		zip_error(code);
		goto fail;
	}
	/* Rust ZipArchive keeps the last duplicate in its first insertion slot. */
	if (archive->count > 1)
		qsort(archive->entries, archive->count, sizeof(*archive->entries), compare_name);
	size_t kept = 0;
	for (size_t i = 0; i < archive->count;) {
		size_t first = i++;
		while (i < archive->count &&
		       archive->entries[first].length == archive->entries[i].length &&
		       !memcmp(archive->entries[first].name, archive->entries[i].name,
			       archive->entries[first].length))
			i++;
		struct zip_entry entry = archive->entries[i - 1];
		entry.order = archive->entries[first].order;
		for (size_t j = first; j < i - 1; j++)
			free(archive->entries[j].name);
		archive->entries[kept++] = entry;
	}
	archive->count = kept;
	if (kept > 1)
		qsort(archive->entries, kept, sizeof(*archive->entries), compare_order);
	return archive;
fail:
	ksu_zip_close(archive);
	return NULL;
}

static int select_entry(struct ksu_zip *archive, size_t index, mz_zip_file **file)
{
	if (zip_error(mz_zip_goto_entry(archive->zip, archive->entries[index].position)) < 0 ||
	    zip_error(mz_zip_entry_get_info(archive->zip, file)) < 0)
		return -1;
	if ((*file)->flag & MZ_ZIP_FLAG_ENCRYPTED) {
		errno = EACCES;
		return -1;
	}
	switch ((*file)->compression_method) {
	case 0:
	case 8:
	case 9:
	case 14:
	case 95:
		return 0;
	default:
		errno = ENOTSUP;
		return -1;
	}
}

void *ksu_zip_calloc(void *opaque, unsigned items, unsigned size)
{
	(void)opaque;
	return calloc(items, size);
}

void ksu_zip_free(void *opaque, void *pointer)
{
	(void)opaque;
	free(pointer);
}

static int write_output(struct zip_output *output, const unsigned char *data, size_t length)
{
	if (!output->file) {
		if (length > output->capacity - output->length) {
			errno = EINVAL;
			return -1;
		}
		memcpy(output->data + output->length, data, length);
	} else if (fwrite(data, 1, length, output->file) != length)
		return -1;
	output->length += length;
	output->crc = (uint32_t)crc32(output->crc, data, (unsigned)length);
	return 0;
}

struct deflate64_input {
	void *zip;
	unsigned char buffer[32768];
	int code;
};

static unsigned read_deflate64(void *opaque, unsigned char **buffer)
{
	struct deflate64_input *input = opaque;
	input->code = mz_zip_entry_read(input->zip, input->buffer, sizeof(input->buffer));
	*buffer = input->buffer;
	return input->code < 0 ? 0 : (unsigned)input->code;
}

static int write_deflate64(void *opaque, unsigned char *buffer, unsigned length)
{
	return write_output(opaque, buffer, length) < 0;
}

static int read_entry(struct ksu_zip *archive, mz_zip_file *file, struct zip_output *output)
{
	uint16_t method = file->compression_method;
	/* Raw dispatch bypasses minizip's unsupported method-9 decompressor. */
	if (method == 9)
		file->compression_method = MZ_COMPRESS_METHOD_STORE;
	int code = mz_zip_entry_read_open(archive->zip, method == 9, NULL);
	file->compression_method = method;
	if (zip_error(code) < 0)
		return -1;
	int result = 0;
	if (method == 9) {
		unsigned char *window = malloc(65536);
		z_stream stream = {0};
		struct deflate64_input input = {.zip = archive->zip};
		if (!window)
			result = -1;
		else if (inflateBack9Init(&stream, window) != Z_OK) {
			errno = ENOMEM;
			result = -1;
		} else {
			code =
			    inflateBack9(&stream, read_deflate64, &input, write_deflate64, output);
			if (code != Z_STREAM_END) {
				if (input.code < 0)
					zip_error(input.code);
				else if (code != Z_BUF_ERROR || !errno)
					errno = EINVAL;
				result = -1;
			}
			inflateBack9End(&stream);
		}
		free(window);
	} else {
		unsigned char buffer[32768];
		while ((code = mz_zip_entry_read(archive->zip, buffer, sizeof(buffer))) > 0)
			if (write_output(output, buffer, (size_t)code) < 0) {
				result = -1;
				break;
			}
		if (code < 0)
			result = zip_error(code);
	}
	int error = errno;
	code = mz_zip_entry_read_close(archive->zip, NULL, NULL, NULL);
	if (result < 0)
		errno = error;
	else if (zip_error(code) < 0)
		result = -1;
	else if (output->crc != file->crc) {
		errno = EINVAL;
		result = -1;
	}
	return result;
}

static unsigned char *read_memory(struct ksu_zip *archive, mz_zip_file *file, size_t *length)
{
	if (file->uncompressed_size < 0 || (uint64_t)file->uncompressed_size >= SIZE_MAX) {
		errno = EOVERFLOW;
		return NULL;
	}
	struct zip_output output = {.capacity = (size_t)file->uncompressed_size};
	output.data = malloc(output.capacity + 1);
	if (!output.data)
		return NULL;
	if (read_entry(archive, file, &output) < 0) {
		free(output.data);
		return NULL;
	}
	output.data[output.length] = 0;
	*length = output.length;
	return output.data;
}

int ksu_zip_properties(struct ksu_zip *archive, struct ksu_string_map *properties)
{
	int result = -1;
	errno = ENOENT;
	for (size_t i = 0; i < archive->count; i++) {
		if (archive->entries[i].length != 11 ||
		    memcmp(archive->entries[i].name, "module.prop", 11))
			continue;
		mz_zip_file *file;
		if (select_entry(archive, i, &file) < 0)
			break;
		size_t length;
		unsigned char *data = read_memory(archive, file, &length);
		if (!data)
			break;
		result = ksu_properties_parse(data, length, properties);
		break;
	}
	return result;
}

int ksu_zip_size(struct ksu_zip *archive, uint64_t *size)
{
	int result = 0;
	*size = 0;
	for (size_t i = 0; i < archive->count; i++) {
		mz_zip_file *file;
		if (select_entry(archive, i, &file) < 0) {
			result = -1;
			break;
		}
		*size += (uint64_t)file->uncompressed_size;
	}
	return result;
}

static const uint16_t cp437_high[128] = {
    0x00c7, 0x00fc, 0x00e9, 0x00e2, 0x00e4, 0x00e0, 0x00e5, 0x00e7, 0x00ea, 0x00eb, 0x00e8, 0x00ef,
    0x00ee, 0x00ec, 0x00c4, 0x00c5, 0x00c9, 0x00e6, 0x00c6, 0x00f4, 0x00f6, 0x00f2, 0x00fb, 0x00f9,
    0x00ff, 0x00d6, 0x00dc, 0x00a2, 0x00a3, 0x00a5, 0x20a7, 0x0192, 0x00e1, 0x00ed, 0x00f3, 0x00fa,
    0x00f1, 0x00d1, 0x00aa, 0x00ba, 0x00bf, 0x2310, 0x00ac, 0x00bd, 0x00bc, 0x00a1, 0x00ab, 0x00bb,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557,
    0x255d, 0x255c, 0x255b, 0x2510, 0x2514, 0x2534, 0x252c, 0x251c, 0x2500, 0x253c, 0x255e, 0x255f,
    0x255a, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256c, 0x2567, 0x2568, 0x2564, 0x2565, 0x2559,
    0x2558, 0x2552, 0x2553, 0x256b, 0x256a, 0x2518, 0x250c, 0x2588, 0x2584, 0x258c, 0x2590, 0x2580,
    0x03b1, 0x00df, 0x0393, 0x03c0, 0x03a3, 0x03c3, 0x00b5, 0x03c4, 0x03a6, 0x0398, 0x03a9, 0x03b4,
    0x221e, 0x03c6, 0x03b5, 0x2229, 0x2261, 0x00b1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00f7, 0x2248,
    0x00b0, 0x2219, 0x00b7, 0x221a, 0x207f, 0x00b2, 0x25a0, 0x00a0,
};

static char *entry_name(const struct zip_entry *entry)
{
	if (memchr(entry->name, 0, entry->length)) {
		errno = EINVAL;
		return NULL;
	}
	char *name = malloc(entry->length * 3 + 1);
	if (!name)
		return NULL;
	const unsigned char *p = (const unsigned char *)entry->name;
	const unsigned char *end = p + entry->length;
	size_t length = 0;
	while (p < end) {
		uint32_t value;
		if (entry->utf8)
			value = ksu_utf8_next(&p, end);
		else {
			unsigned char byte = *p++;
			value = byte < 0x80 ? byte : cp437_high[byte - 0x80];
		}
		length += ksu_utf8_put((unsigned char *)name + length, value);
	}
	name[length] = 0;
	return name;
}

static int path_separator(char c)
{
	return c == '/' || c == '\\';
}

static char *skip_unc(char *p)
{
	for (unsigned component = 0; component < 2; component++) {
		while (*p && path_separator(*p))
			p++;
		while (*p && !path_separator(*p))
			p++;
	}
	return p;
}

static int normalize_path(char *path)
{
	char *p = path, *out = path;
	if (path_separator(p[0]) && p[1] == p[0]) {
		p += 2;
		if ((p[0] == '?' || p[0] == '.') && path_separator(p[1])) {
			p += 2;
			if (!strncmp(p, "UNC", 3) && path_separator(p[3]))
				p = skip_unc(p + 4);
			else if (p[0] && p[1] == ':')
				p += 2;
			else
				while (*p && !path_separator(*p))
					p++;
		} else
			p = skip_unc(p);
	} else if (((p[0] >= 'a' && p[0] <= 'z') || (p[0] >= 'A' && p[0] <= 'Z')) && p[1] == ':')
		p += 2;
	while (*p) {
		while (path_separator(*p))
			p++;
		char *part = p;
		while (*p && !path_separator(*p))
			p++;
		size_t length = (size_t)(p - part);
		if (!length || (length == 1 && *part == '.'))
			continue;
		if (length == 2 && !memcmp(part, "..", 2)) {
			if (out == path) {
				errno = EINVAL;
				return -1;
			}
			while (out > path && out[-1] != '/')
				out--;
			if (out > path)
				out--;
			continue;
		}
		if (out > path)
			*out++ = '/';
		memmove(out, part, length);
		out += length;
	}
	*out = 0;
	return 0;
}

static int writable_directory(const char *path)
{
	struct stat st;
	if (ksu_mkdirs(path) < 0 || stat(path, &st) < 0)
		return -1;
	return chmod(path, (st.st_mode & 07777) | 0700);
}

static char *prepare_path(const char *root, char *name, char **base)
{
	if (normalize_path(name) < 0)
		return NULL;
	char *path = strdup(root);
	if (!path)
		return NULL;
	char *part = name;
	while (*part) {
		char *end = strchr(part, '/');
		if (end)
			*end = 0;
		char *next = ksu_join_path(path, part);
		free(path);
		path = next;
		if (!path)
			return NULL;
		for (unsigned limit = 5;;) {
			struct stat st;
			if (lstat(path, &st) < 0) {
				if (errno != ENOENT || (end && writable_directory(path) < 0))
					goto fail;
				break;
			}
			if (!S_ISLNK(st.st_mode))
				break;
			if (!--limit) {
				errno = ELOOP;
				goto fail;
			}
			char *target = malloc((size_t)st.st_size + 1);
			if (!target)
				goto fail;
			ssize_t length = readlink(path, target, (size_t)st.st_size + 1);
			if (length < 0 || length > st.st_size) {
				free(target);
				goto fail;
			}
			target[length] = 0;
			char *normalized = strdup(target);
			if (!*base) {
				*base = strdup(root);
				if (*base && normalize_path(*base) < 0) {
					free(target);
					free(normalized);
					goto fail;
				}
			}
			if (!normalized || !*base) {
				free(target);
				free(normalized);
				goto fail;
			}
			int valid = normalize_path(normalized) == 0;
			size_t base_length = strlen(*base);
			valid = valid && !strncmp(normalized, *base, base_length) &&
				(!normalized[base_length] || normalized[base_length] == '/');
			free(normalized);
			if (!valid) {
				free(target);
				errno = EINVAL;
				goto fail;
			}
			next = ksu_join_path(path, target);
			free(target);
			free(path);
			path = next;
			if (!path)
				return NULL;
		}
		part = end ? end + 1 : part + strlen(part);
	}
	return path;
fail:
	free(path);
	return NULL;
}

static int unix_mode(mz_zip_file *file, mode_t *mode)
{
	uint32_t attributes = file->external_fa;
	if (!attributes)
		return 0;
	*mode = attributes >> 16;
	if (*mode || (file->version_madeby >> 8) == 3)
		return 1;
	if ((file->version_madeby >> 8) != 0)
		return 0;
	*mode = (attributes & 0x10) ? (S_IFDIR | 0775) : (S_IFREG | 0664);
	if (attributes & 1)
		*mode &= ~0222;
	return 1;
}

struct zip_permission {
	char *path;
	mode_t mode;
	size_t order;
};

static int child_first(const void *left, const void *right)
{
	const struct zip_permission *a = left, *b = right;
	int path = -strcmp(a->path, b->path);
	return path ? path : (a->order < b->order) - (a->order > b->order);
}

int ksu_zip_extract(struct ksu_zip *archive, const char *directory)
{
	char *root = NULL, *base = NULL;
	struct zip_permission *permissions = NULL;
	size_t count = 0;
	int result = -1;
	if (ksu_mkdirs(directory) < 0 || !(root = realpath(directory, NULL)))
		goto done;
	permissions = calloc(archive->count ? archive->count : 1, sizeof(*permissions));
	if (!permissions)
		goto done;
	for (size_t i = 0; i < archive->count; i++) {
		mz_zip_file *file;
		if (select_entry(archive, i, &file) < 0)
			goto done;
		char *name = entry_name(&archive->entries[i]);
		if (!name)
			goto done;
		size_t name_length = strlen(name);
		bool is_directory = name_length && path_separator(name[name_length - 1]);
		char *target = prepare_path(root, name, &base);
		free(name);
		if (!target)
			goto done;
		mode_t mode = 0;
		bool has_mode = unix_mode(file, &mode);
		int extracted;
		if (has_mode && S_ISLNK(mode)) {
			size_t length;
			unsigned char *link = read_memory(archive, file, &length);
			if (!link)
				extracted = -1;
			else {
				if (!ksu_valid_utf8(link, length) || memchr(link, 0, length)) {
					errno = EINVAL;
					extracted = -1;
				} else
					extracted = symlink((char *)link, target);
				free(link);
			}
		} else if (is_directory)
			extracted = writable_directory(target);
		else {
			FILE *out = fopen(target, "wb");
			extracted = -1;
			if (out) {
				struct zip_output output = {.file = out};
				extracted = read_entry(archive, file, &output);
				int error = errno;
				int closed = fclose(out);
				if (extracted < 0)
					errno = error;
				else if (closed < 0)
					extracted = -1;
			}
			if (!extracted && has_mode) {
				permissions[count++] = (struct zip_permission){target, mode, i};
				target = NULL;
			}
		}
		free(target);
		if (extracted < 0)
			goto done;
	}
	qsort(permissions, count, sizeof(*permissions), child_first);
	for (size_t i = 0; i < count; i++)
		if ((!i || strcmp(permissions[i - 1].path, permissions[i].path)) &&
		    chmod(permissions[i].path, permissions[i].mode) < 0)
			goto done;
	result = 0;
done: {
	int error = errno;
	for (size_t i = 0; i < count; i++)
		free(permissions[i].path);
	free(permissions);
	free(base);
	free(root);
	errno = error;
}
	return result;
}
