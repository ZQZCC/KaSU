// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "file.h"
#include "native.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int ksu_mkdirs_mode(const char *path, unsigned int mode)
{
	char *copy = strdup(path);
	if (!copy)
		return -1;
	int result = 0;
	for (char *end = copy + (*copy == '/');; ++end) {
		if (*end && *end != '/')
			continue;
		char delimiter = *end;
		*end = '\0';
		if (*copy && mkdir(copy, mode)) {
			struct stat st;
			if (errno != EEXIST || stat(copy, &st) || !S_ISDIR(st.st_mode)) {
				result = -1;
				if (errno == EEXIST)
					errno = ENOTDIR;
				break;
			}
		}
		*end = delimiter;
		if (!delimiter)
			break;
	}
	int error = errno;
	free(copy);
	errno = error;
	return result;
}

int ksu_mkdirs(const char *path)
{
	return ksu_mkdirs_mode(path, 0777);
}

char *ksu_join_path(const char *directory, const char *name)
{
	if (*name == '/')
		return strdup(name);
	char *path;
	size_t length = strlen(directory);
	const char *separator = length && directory[length - 1] != '/' ? "/" : "";
	if (asprintf(&path, "%s%s%s", directory, separator, name) < 0)
		return NULL;
	return path;
}

int ksu_ensure_file(const char *path)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
	if (fd >= 0) {
		close(fd);
		return 0;
	}
	struct stat st;
	return errno == EEXIST && !stat(path, &st) && S_ISREG(st.st_mode) ? 0 : -1;
}

static int remove_entry(int parent, const char *name)
{
	int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return errno == ENOTDIR || errno == ELOOP ? unlinkat(parent, name, 0) : -1;
	DIR *directory = fdopendir(fd);
	if (!directory) {
		int error = errno;
		close(fd);
		errno = error;
		return -1;
	}
	int result;
	for (;;) {
		errno = 0;
		struct dirent *entry = readdir(directory);
		if (!entry) {
			result = errno ? -1 : 0;
			break;
		}
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		if (remove_entry(fd, entry->d_name) && errno != ENOENT) {
			result = -1;
			break;
		}
	}
	int error = errno;
	closedir(directory);
	if (result) {
		errno = error;
		return -1;
	}
	return unlinkat(parent, name, AT_REMOVEDIR);
}

int ksu_remove_tree(const char *path)
{
	struct stat st;
	if (lstat(path, &st))
		return -1;
	if (S_ISLNK(st.st_mode))
		return unlink(path);
	if (!S_ISDIR(st.st_mode)) {
		errno = ENOTDIR;
		return -1;
	}
	return remove_entry(AT_FDCWD, path);
}

unsigned char *ksu_read_file(const char *path, size_t *length)
{
	FILE *file = fopen(path, "re");
	if (!file)
		return NULL;
	unsigned char *data = ksu_read_stream(file, SIZE_MAX, length);
	int error = errno;
	fclose(file);
	errno = error;
	return data;
}

unsigned char *ksu_read_stream(FILE *file, size_t limit, size_t *length)
{
	size_t used = 0, capacity = 4096;
	struct stat st;
	if (!fstat(fileno(file), &st) && st.st_size > 0)
		capacity = (size_t)st.st_size + 1;
	if (capacity > limit)
		capacity = limit;
	unsigned char *data = malloc(capacity);
	if (!data)
		goto error;
	for (;;) {
		used += fread(data + used, 1, capacity - used, file);
		if (ferror(file))
			goto error;
		if (feof(file) || used == limit)
			break;
		if (capacity > SIZE_MAX / 2) {
			errno = EOVERFLOW;
			goto error;
		}
		capacity *= 2;
		if (capacity > limit)
			capacity = limit;
		unsigned char *grown = realloc(data, capacity);
		if (!grown)
			goto error;
		data = grown;
	}
	*length = used;
	return data;
error:
	int error = errno;
	free(data);
	errno = error;
	return NULL;
}

int ksu_write_file(const char *path, const void *data, size_t length)
{
	FILE *file = fopen(path, "we");
	if (!file)
		return -1;
	bool written = !length || fwrite(data, 1, length, file) == length;
	int error = errno;
	int result = fclose(file);
	if (!written) {
		errno = error;
		return -1;
	}
	return result;
}

int ksu_copy_file(const char *source, const char *destination)
{
	FILE *input = fopen(source, "re");
	if (!input)
		return -1;
	FILE *output = NULL;
	struct stat st;
	int result = -1;
	if (fstat(fileno(input), &st))
		goto done;
	if (!S_ISREG(st.st_mode)) {
		errno = EINVAL;
		goto done;
	}
	output = fopen(destination, "we");
	if (!output)
		goto done;
	unsigned char buffer[32768];
	size_t length;
	while ((length = fread(buffer, 1, sizeof(buffer), input)))
		if (fwrite(buffer, 1, length, output) != length)
			goto done;
	if (ferror(input) || fflush(output) || fchmod(fileno(output), st.st_mode & 07777))
		goto done;
	result = 0;
done:
	int error = errno;
	if (output && fclose(output) && !result) {
		result = -1;
		error = errno;
	}
	fclose(input);
	errno = error;
	return result;
}
