// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "prop.h"
#include "file.h"
#include "prop_area.h"
#include "prop_context.h"
#include "prop_persist.h"
#include "text.h"
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>

#ifndef KSU_PROPS_DIR
#define KSU_PROPS_DIR "/dev/__properties__"
#endif
#ifndef KSU_PERSIST_DIR
#define KSU_PERSIST_DIR "/data/property"
#endif
#ifndef KSU_SYSTEM_ROOT
#define KSU_SYSTEM_ROOT "/"
#endif

typedef void (*prop_callback)(void *, const char *, const char *, uint32_t);

static struct {
	const void *(*find)(const char *);
	void (*read)(const void *, prop_callback, void *);
	void (*each)(void (*)(const void *, void *), void *);
	int (*set)(const char *, const char *);
	uint32_t (*serial)(const void *), (*area_serial)(void);
	bool (*wait)(const void *, uint32_t, uint32_t *, const struct timespec *);
} api;

struct mapped_area {
	struct ksu_prop_area area;
	char *label;
	bool appcompat;
};

static pthread_mutex_t property_lock = PTHREAD_MUTEX_INITIALIZER;
static struct ksu_prop_context contexts[2];
static struct ksu_prop_area serial_areas[2];
static struct mapped_area *areas;
static size_t area_count;
static bool initialized, has_appcompat;

int ksu_prop_init(void)
{
	pthread_mutex_lock(&property_lock);
	int result = -1;
	if (initialized) {
		result = 0;
		goto done;
	}
	int (*initialize)(void) = dlsym(RTLD_DEFAULT, "__system_properties_init");
	api.find = dlsym(RTLD_DEFAULT, "__system_property_find");
	api.read = dlsym(RTLD_DEFAULT, "__system_property_read_callback");
	api.each = dlsym(RTLD_DEFAULT, "__system_property_foreach");
	api.set = dlsym(RTLD_DEFAULT, "__system_property_set");
	api.serial = dlsym(RTLD_DEFAULT, "__system_property_serial");
	api.area_serial = dlsym(RTLD_DEFAULT, "__system_property_area_serial");
	api.wait = dlsym(RTLD_DEFAULT, "__system_property_wait");
	if (!initialize || !api.find || !api.read || !api.each || !api.set || !api.serial) {
		errno = ENOSYS;
		goto done;
	}
	if (initialize()) {
		errno = EIO;
		goto done;
	}
	if (ksu_prop_context_load(&contexts[0], KSU_PROPS_DIR, KSU_SYSTEM_ROOT))
		goto done;
	struct stat st;
	const char *appcompat = KSU_PROPS_DIR "/appcompat_override";
	has_appcompat = !stat(appcompat, &st) && S_ISDIR(st.st_mode) &&
			!ksu_prop_context_load(&contexts[1], appcompat, KSU_SYSTEM_ROOT);
	initialized = true;
	result = 0;
done:
	int saved = errno;
	pthread_mutex_unlock(&property_lock);
	errno = saved;
	return result;
}

static int map_file(const char *path, struct ksu_prop_area *area)
{
	int fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -1;
	struct stat st;
	int result = fstat(fd, &st);
	void *memory = MAP_FAILED;
	if (!result && st.st_size > 0)
		memory = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	else if (!result)
		errno = EINVAL;
	int saved = errno;
	close(fd);
	errno = saved;
	if (memory == MAP_FAILED)
		return -1;
	if (ksu_prop_area_init(area, memory, (size_t)st.st_size)) {
		saved = errno;
		munmap(memory, (size_t)st.st_size);
		errno = saved;
		return -1;
	}
	return 0;
}

/* Called with property_lock held; mappings survive reallocating their index. */
static struct ksu_prop_area *map_area(const char *label, bool appcompat)
{
	for (size_t i = 0; i < area_count; ++i)
		if (areas[i].appcompat == appcompat && !strcmp(areas[i].label, label))
			return &areas[i].area;
	char *path = ksu_prop_context_path(&contexts[appcompat], label);
	if (!path)
		return NULL;
	struct ksu_prop_area area;
	int result = map_file(path, &area);
	free(path);
	if (result)
		return NULL;
	char *key = strdup(label);
	struct mapped_area *expanded =
	    key ? realloc(areas, (area_count + 1) * sizeof(*areas)) : NULL;
	if (!expanded) {
		int saved = errno;
		free(key);
		munmap(area.base, area.length);
		errno = saved;
		return NULL;
	}
	areas = expanded;
	areas[area_count] = (struct mapped_area){area, key, appcompat};
	return &areas[area_count++].area;
}

static struct ksu_prop_area *map_serial(bool appcompat)
{
	struct ksu_prop_area *area = &serial_areas[appcompat];
	if (!area->base) {
		struct ksu_prop_context *context = &contexts[appcompat];
		char *path = context->type == KSU_PROP_PRE_SPLIT
				 ? strdup(context->directory)
				 : ksu_join_path(context->directory, "properties_serial");
		if (!path)
			return NULL;
		int result = map_file(path, area);
		free(path);
		if (result)
			return NULL;
	}
	return area;
}

static bool prefix(const unsigned char *name, size_t length, const char *text)
{
	size_t size = strlen(text);
	return size <= length && !memcmp(name, text, size);
}

static const unsigned char *override_name(const unsigned char *name, size_t *length)
{
	const char *start = "ro.appcompat_override.";
	if (prefix(name, *length, start)) {
		*length -= strlen(start);
		name += strlen(start);
	}
	return name;
}

static char *c_string(const unsigned char *data, size_t length)
{
	if (memchr(data, 0, length)) {
		errno = EINVAL;
		return NULL;
	}
	char *copy = malloc(length + 1);
	if (copy) {
		memcpy(copy, data, length);
		copy[length] = 0;
	}
	return copy;
}

int ksu_prop_set(const unsigned char *name, size_t name_length, const unsigned char *value,
		 size_t value_length, bool skip_service, bool *need_rebuild)
{
	*need_rebuild = false;
	bool readonly = prefix(name, name_length, "ro.");
	if (!readonly && value_length >= KSU_PROP_VALUE_MAX) {
		errno = E2BIG;
		return -1;
	}
	if (!skip_service && !readonly) {
		char *key = c_string(name, name_length), *text = c_string(value, value_length);
		int result = key && text ? api.set(key, text) : -1;
		if (key && text && result)
			errno = EIO;
		int saved = errno;
		free(key);
		free(text);
		errno = saved;
		return result ? -1 : 0;
	}
	pthread_mutex_lock(&property_lock);
	const char *label = ksu_prop_context_get_n(&contexts[0], name, name_length);
	struct ksu_prop_area *area = map_area(label, false),
			     *global = area ? map_serial(false) : NULL;
	int result = global ? ksu_prop_area_set(area, global, name, name_length, value,
						value_length, need_rebuild)
			    : -1;
	if (!result && has_appcompat) {
		size_t length = name_length;
		const unsigned char *key = override_name(name, &length);
		label = ksu_prop_context_get_n(&contexts[1], key, length);
		area = map_area(label, true);
		global = area ? map_serial(true) : NULL;
		bool need = false;
		if (global)
			ksu_prop_area_set(area, global, key, length, value, value_length, &need);
		*need_rebuild |= need;
	}
	int saved = errno;
	pthread_mutex_unlock(&property_lock);
	errno = saved;
	return result;
}

static int delete_property(const unsigned char *name, size_t length)
{
	pthread_mutex_lock(&property_lock);
	const char *label = ksu_prop_context_get_n(&contexts[0], name, length);
	struct ksu_prop_area *area = map_area(label, false);
	int deleted = area ? ksu_prop_area_remove(area, name, length) : -1;
	if (deleted > 0) {
		struct ksu_prop_area *global = map_serial(false);
		if (global)
			ksu_prop_area_bump(global);
	}
	if (deleted >= 0 && has_appcompat) {
		const unsigned char *key = override_name(name, &length);
		label = ksu_prop_context_get_n(&contexts[1], key, length);
		area = map_area(label, true);
		if (area)
			ksu_prop_area_remove(area, key, length);
		if (deleted > 0) {
			struct ksu_prop_area *global = map_serial(true);
			if (global)
				ksu_prop_area_bump(global);
		}
	}
	int saved = errno;
	pthread_mutex_unlock(&property_lock);
	errno = saved;
	return deleted;
}

struct prop_read {
	const unsigned char *expected;
	size_t length;
	uint32_t serial;
	bool seen, different;
};

static void compare_value(void *cookie, const char *name, const char *value, uint32_t serial)
{
	(void)name;
	struct prop_read *read = cookie;
	read->serial = serial;
	read->seen = true;
	const unsigned char *cursor = (const unsigned char *)value, *end = cursor + strlen(value);
	if (ksu_valid_utf8(cursor, (size_t)(end - cursor))) {
		read->different = (size_t)(end - cursor) != read->length ||
				  memcmp(cursor, read->expected, read->length);
		return;
	}
	size_t offset = 0;
	while (cursor < end) {
		unsigned char utf8[4];
		size_t size = ksu_utf8_put(utf8, ksu_utf8_next(&cursor, end));
		if (offset > read->length || size > read->length - offset ||
		    memcmp(utf8, read->expected + offset, size)) {
			read->different = true;
			return;
		}
		offset += size;
	}
	read->different = offset != read->length;
}

bool ksu_property_equals(const char *name, const char *value)
{
	const prop_info *info = __system_property_find(name);
	if (!info)
		return false;
	struct prop_read read = {.expected = (const unsigned char *)value, .length = strlen(value)};
	__system_property_read_callback(info, compare_value, &read);
	return read.seen && !read.different;
}

static int remaining(const struct timespec *deadline, struct timespec *timeout)
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now))
		return -1;
	timeout->tv_sec = deadline->tv_sec - now.tv_sec;
	timeout->tv_nsec = deadline->tv_nsec - now.tv_nsec;
	if (timeout->tv_nsec < 0) {
		--timeout->tv_sec;
		timeout->tv_nsec += 1000000000;
	}
	if (timeout->tv_sec < 0 || (!timeout->tv_sec && !timeout->tv_nsec)) {
		*timeout = (struct timespec){0};
		return 0;
	}
	return 1;
}

int ksu_prop_wait(const unsigned char *name, size_t name_length, const unsigned char *old_value,
		  size_t old_length, const struct timespec *timeout)
{
	if (!api.wait) {
		errno = ENOSYS;
		return -1;
	}
	struct timespec deadline, interval;
	if (timeout) {
		if (clock_gettime(CLOCK_MONOTONIC, &deadline))
			return -1;
		if (__builtin_add_overflow(deadline.tv_sec, timeout->tv_sec, &deadline.tv_sec)) {
			errno = EOVERFLOW;
			return -1;
		}
		deadline.tv_nsec += timeout->tv_nsec;
		if (deadline.tv_nsec >= 1000000000) {
			++deadline.tv_sec;
			deadline.tv_nsec -= 1000000000;
		}
	}
	char *key = c_string(name, name_length);
	if (!key && errno != EINVAL)
		return -1;
	const void *info = NULL;
	int result = 0;
	for (;;) {
		if (timeout && (result = remaining(&deadline, &interval)) <= 0)
			break;
		info = key ? api.find(key) : NULL;
		if (info)
			break;
		uint32_t previous = api.area_serial ? api.area_serial() : 0, next;
		if (timeout && remaining(&deadline, &interval) < 0) {
			result = -1;
			break;
		}
		if (!api.wait(NULL, previous, &next, timeout ? &interval : NULL) && timeout) {
			result = 0;
			break;
		}
	}
	if (!info)
		goto done;
	result = 1;
	if (!old_value)
		goto done;
	for (;;) {
		if (timeout && (result = remaining(&deadline, &interval)) <= 0)
			break;
		struct prop_read read = {.expected = old_value, .length = old_length};
		api.read(info, compare_value, &read);
		if (read.seen && read.different) {
			result = 1;
			break;
		}
		uint32_t previous = api.area_serial ? api.area_serial() : read.serial, next;
		if (timeout && remaining(&deadline, &interval) < 0) {
			result = -1;
			break;
		}
		if (!api.wait(api.area_serial ? NULL : info, previous, &next,
			      timeout ? &interval : NULL) &&
		    timeout) {
			result = 0;
			break;
		}
	}
done:
	int saved = errno;
	free(key);
	errno = saved;
	return result;
}

static int set_property(const unsigned char *name, size_t name_length, const unsigned char *value,
			size_t length, unsigned flags, bool *need_rebuild)
{
	if (ksu_prop_set(name, name_length, value, length, flags & KSU_PROP_SKIP_SVC, need_rebuild))
		return -1;
	if ((flags & KSU_PROP_PERSISTENT) &&
	    ((flags & KSU_PROP_SKIP_SVC) || prefix(name, name_length, "ro.")) &&
	    prefix(name, name_length, "persist.") &&
	    ksu_prop_persist_set(KSU_PERSIST_DIR, name, name_length, value, length))
		return -1;
	if (flags & KSU_PROP_VERBOSE)
		fprintf(stderr, "resetprop: set %.*s=%.*s\n", (int)name_length, name, (int)length,
			value);
	return 0;
}

int ksu_prop_load_file(const char *path, unsigned flags, bool *need_rebuild)
{
	*need_rebuild = false;
	FILE *input = fopen(path, "re");
	if (!input)
		return -1;
	char *buffer = NULL;
	size_t capacity = 0;
	ssize_t bytes;
	int result = -1;
	while ((bytes = getline(&buffer, &capacity, input)) >= 0) {
		const unsigned char *line = (const unsigned char *)buffer;
		size_t length = (size_t)bytes;
		if (!ksu_valid_utf8(line, length)) {
			errno = EILSEQ;
			goto done;
		}
		ksu_trim_space(&line, &length);
		if (!length || *line == '#')
			continue;
		const unsigned char *equal = memchr(line, '=', length);
		if (!equal)
			continue;
		const unsigned char *value = equal + 1;
		size_t value_length = length - (size_t)(value - line);
		length = (size_t)(equal - line);
		ksu_trim_space(&line, &length);
		ksu_trim_space(&value, &value_length);
		bool need = false;
		if (length && set_property(line, length, value, value_length, flags, &need))
			goto done;
		*need_rebuild |= need;
	}
	if (!ferror(input))
		result = 0;
done:
	int saved = errno;
	free(buffer);
	fclose(input);
	errno = saved;
	return result;
}

struct prop_row {
	struct ksu_string_pair pair;
	unsigned char *data;
	size_t order;
};

struct prop_list {
	struct prop_row *rows;
	size_t count, capacity;
	int error;
};

static int append_row(struct prop_list *list, struct ksu_string_pair pair, unsigned char *data)
{
	if (list->count == list->capacity) {
		size_t capacity = list->capacity ? list->capacity * 2 : 16;
		struct prop_row *rows = realloc(list->rows, capacity * sizeof(*rows));
		if (!rows)
			return -1;
		list->rows = rows;
		list->capacity = capacity;
	}
	list->rows[list->count] = (struct prop_row){pair, data, list->count};
	++list->count;
	return 0;
}

static void read_row(void *cookie, const char *name, const char *value, uint32_t serial)
{
	(void)serial;
	struct prop_list *list = cookie;
	if (list->error)
		return;
	size_t name_length = strlen(name), value_length = strlen(value);
	bool name_valid = ksu_valid_utf8((const unsigned char *)name, name_length);
	bool value_valid = ksu_valid_utf8((const unsigned char *)value, value_length);
	unsigned char *data =
	    malloc(name_length * (name_valid ? 1 : 3) + value_length * (value_valid ? 1 : 3) + 2);
	if (!data) {
		list->error = errno;
		return;
	}
	if (name_valid)
		memcpy(data, name, name_length);
	else
		name_length = ksu_utf8_copy(data, (const unsigned char *)name, name_length);
	data[name_length] = 0;
	if (value_valid)
		memcpy(data + name_length + 1, value, value_length);
	else
		value_length = ksu_utf8_copy(data + name_length + 1, (const unsigned char *)value,
					     value_length);
	struct ksu_string_pair pair = {data, name_length, data + name_length + 1, value_length};
	if (append_row(list, pair, data)) {
		list->error = errno;
		free(data);
	}
}

static void each_row(const void *info, void *cookie)
{
	api.read(info, read_row, cookie);
}

static int compare_pair(const struct ksu_string_pair *a, const struct ksu_string_pair *b)
{
	size_t size = a->key_length < b->key_length ? a->key_length : b->key_length;
	int order = memcmp(a->key, b->key, size);
	return order ? order : (a->key_length > b->key_length) - (a->key_length < b->key_length);
}

static int compare_row(const void *left, const void *right)
{
	const struct prop_row *a = left, *b = right;
	int order = compare_pair(&a->pair, &b->pair);
	return order ? order : (a->order > b->order) - (a->order < b->order);
}

static int list_properties(unsigned flags)
{
	struct prop_list list = {0};
	struct ksu_string_map persistent = {0};
	int result = -1;
	if (!(flags & KSU_PROP_PERSIST_ONLY))
		api.each(each_row, &list);
	if (list.error) {
		errno = list.error;
		goto done;
	}
	if (list.count > 1)
		qsort(list.rows, list.count, sizeof(*list.rows), compare_row);
	if ((flags & (KSU_PROP_PERSISTENT | KSU_PROP_PERSIST_ONLY)) &&
	    ksu_prop_persist_load(KSU_PERSIST_DIR, &persistent))
		goto done;
	size_t i = 0, j = 0;
	while (i < list.count || j < persistent.count) {
		struct ksu_string_pair pair;
		if (j == persistent.count ||
		    (i < list.count &&
		     compare_pair(&list.rows[i].pair, &persistent.entries[j]) <= 0)) {
			pair = list.rows[i++].pair;
			while (j < persistent.count && !compare_pair(&pair, &persistent.entries[j]))
				++j;
		} else
			pair = persistent.entries[j++];
		if (flags & KSU_PROP_CONTEXT) {
			pair.value = (const unsigned char *)ksu_prop_context_get_n(
			    &contexts[0], pair.key, pair.key_length);
			pair.value_length = strlen((const char *)pair.value);
		}
		fputc('[', stdout);
		fwrite(pair.key, 1, pair.key_length, stdout);
		fputs("]: [", stdout);
		fwrite(pair.value, 1, pair.value_length, stdout);
		fputs("]\n", stdout);
	}
	result = 0;
done:
	int saved = errno;
	for (size_t i = 0; i < list.count; ++i)
		free(list.rows[i].data);
	free(list.rows);
	ksu_strings_free(&persistent);
	errno = saved;
	return result;
}

static void print_value(void *cookie, const char *name, const char *value, uint32_t serial)
{
	(void)name;
	(void)serial;
	bool *seen = cookie;
	*seen = true;
	const unsigned char *cursor = (const unsigned char *)value, *end = cursor + strlen(value);
	if (ksu_valid_utf8(cursor, (size_t)(end - cursor))) {
		fwrite(cursor, 1, (size_t)(end - cursor), stdout);
		fputc('\n', stdout);
		return;
	}
	while (cursor < end) {
		unsigned char utf8[4];
		size_t size = ksu_utf8_put(utf8, ksu_utf8_next(&cursor, end));
		fwrite(utf8, 1, size, stdout);
	}
	fputc('\n', stdout);
}

static int get_property(const struct ksu_resetprop *options)
{
	if (options->flags & KSU_PROP_CONTEXT) {
		puts(ksu_prop_context_get_n(&contexts[0], options->name, options->name_length));
		return 1;
	}
	bool seen = false;
	if (!(options->flags & KSU_PROP_PERSIST_ONLY)) {
		char *key = c_string(options->name, options->name_length);
		const void *info = key ? api.find(key) : NULL;
		free(key);
		if (info)
			api.read(info, print_value, &seen);
	}
	if (!seen && (options->flags & (KSU_PROP_PERSISTENT | KSU_PROP_PERSIST_ONLY)) &&
	    prefix(options->name, options->name_length, "persist.")) {
		unsigned char *value;
		size_t length;
		if (!ksu_prop_persist_get(KSU_PERSIST_DIR, options->name, options->name_length,
					  &value, &length) &&
		    value) {
			fwrite(value, 1, length, stdout);
			fputc('\n', stdout);
			free(value);
			seen = true;
		}
	}
	return seen;
}

static int rebuild_area(const char *label, bool check, bool appcompat)
{
	struct ksu_prop_area *area = map_area(label, appcompat);
	return area ? ksu_prop_area_rebuild(area, check) : -1;
}

static int rebuild_properties(const struct ksu_resetprop *options)
{
	pthread_mutex_lock(&property_lock);
	int result = 0;
	if (options->name) {
		const char *label;
		char *owned = NULL;
		if (options->flags & (KSU_PROP_CONTEXT | KSU_PROP_DELETE))
			label = ksu_prop_context_get_n(&contexts[0], options->name,
						       options->name_length);
		else
			label = owned = c_string(options->name, options->name_length);
		result = label ? rebuild_area(label, false, false) : -1;
		if (!result && has_appcompat)
			result = rebuild_area(label, false, true);
		free(owned);
	} else {
		for (unsigned index = 0; index <= (unsigned)has_appcompat; ++index) {
			size_t count;
			const char **labels = ksu_prop_context_labels(&contexts[index], &count);
			if (!labels) {
				result = -1;
				break;
			}
			for (size_t i = 0; i < count; ++i)
				if (rebuild_area(labels[i], !(options->flags & KSU_PROP_FORCE),
						 index != 0)) {
					__android_log_print(ANDROID_LOG_ERROR, "KernelSU",
							    "failed to rebuild %sarea %s: %s",
							    index ? "appcompat " : "", labels[i],
							    strerror(errno));
					result = -1;
				}
			free(labels);
		}
	}
	int saved = errno;
	pthread_mutex_unlock(&property_lock);
	errno = saved;
	return result;
}

static int command_error(const char *message)
{
	fprintf(stderr, "resetprop: %s\n", message);
	return 1;
}

int ksu_resetprop_run(const struct ksu_resetprop *options)
{
	unsigned flags = options->flags;
	const char *error = "Failed to initialize system property API";
	if (ksu_prop_init())
		goto failed;
	unsigned modes = !!(flags & KSU_PROP_WAIT) + !!(flags & KSU_PROP_DELETE) + !!options->file;
	if (modes > 1)
		return command_error("multiple operation modes detected");
	if ((flags & KSU_PROP_REBUILD) && modes && !(flags & KSU_PROP_DELETE))
		return command_error("Only -d can be used with -c");
	if (flags & KSU_PROP_WAIT) {
		if (!options->name)
			return command_error("--wait requires a property name");
		error = "wait failed";
		int result = ksu_prop_wait(options->name, options->name_length, options->value,
					   options->value_length, options->timeout);
		if (result < 0)
			goto failed;
		if (!result) {
			fprintf(stderr, "resetprop: timeout waiting for %.*s\n",
				(int)options->name_length, options->name);
			return 2;
		}
		return 0;
	}
	bool need_rebuild = false;
	if (options->file) {
		error = "Failed to load properties from file";
		if (ksu_prop_load_file(options->file, flags, &need_rebuild))
			goto failed;
		goto warning;
	}
	if (flags & KSU_PROP_DELETE) {
		if (!options->name)
			return command_error("--delete requires a property name");
		error = "delete failed";
		int deleted = delete_property(options->name, options->name_length);
		if (deleted < 0)
			goto failed;
		if (deleted && (flags & KSU_PROP_PERSISTENT) &&
		    prefix(options->name, options->name_length, "persist.") &&
		    ksu_prop_persist_delete(KSU_PERSIST_DIR, options->name, options->name_length) <
			0)
			goto failed;
		if (flags & KSU_PROP_VERBOSE)
			fprintf(stderr, "resetprop: %s%.*s%s\n", deleted ? "deleted " : "",
				(int)options->name_length, options->name,
				deleted ? "" : " not found");
		if (!deleted)
			goto not_found;
		if (!(flags & KSU_PROP_REBUILD))
			return 0;
	}
	if (flags & KSU_PROP_REBUILD) {
		error = "Failed to rebuild property area";
		if (rebuild_properties(options)) {
			if (options->name)
				goto failed;
			fputs("Something wrong happened, see log for detail.\n", stderr);
			return 1;
		}
		return 0;
	}
	if (!options->name) {
		error = "Failed to list properties";
		if (list_properties(flags))
			goto failed;
		return 0;
	}
	if (!options->value) {
		if (!get_property(options))
			goto not_found;
		return 0;
	}
	error = "Failed to set property";
	if (set_property(options->name, options->name_length, options->value, options->value_length,
			 flags, &need_rebuild))
		goto failed;
warning:
	if (need_rebuild)
		fputs("resetprop: warning: rebuild is needed!\n", stderr);
	return 0;
not_found:
	fprintf(stderr, "resetprop: %.*s not found\n", (int)options->name_length, options->name);
	return 1;
failed:
	fprintf(stderr, "resetprop: %s: %s\n", error, strerror(errno));
	return 1;
}
