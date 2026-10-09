// SPDX-License-Identifier: GPL-3.0-or-later
#include "file.h"
#include "native.h"
#include "text.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int ksu_profile_get(const char *path)
{
	size_t length;
	unsigned char *data = ksu_read_file(path, &length);
	if (!data)
		return -1;
	int result = 0;
	if (!ksu_valid_utf8(data, length)) {
		errno = EILSEQ;
		result = -1;
	} else if (fwrite(data, 1, length, stdout) != length || putchar('\n') == EOF) {
		result = -1;
	}
	int error = errno;
	free(data);
	errno = error;
	return result;
}

int ksu_profile_set(const char *directory, const char *path, const void *data, size_t length)
{
	return ksu_mkdirs(directory) ? -1 : ksu_write_file(path, data, length);
}

int ksu_profile_delete(const char *path)
{
	return unlink(path);
}

int ksu_profile_list(const char *directory)
{
	DIR *dir = opendir(directory);
	if (!dir)
		return 0;
	int result = 0;
	struct dirent *entry;
	errno = 0;
	while ((entry = readdir(dir))) {
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		if (ksu_valid_utf8(entry->d_name, strlen(entry->d_name)) &&
		    puts(entry->d_name) == EOF) {
			result = -1;
			break;
		}
		errno = 0;
	}
	if (errno)
		result = -1;
	int error = errno;
	closedir(dir);
	errno = error;
	return result;
}
