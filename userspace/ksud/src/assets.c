// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "assets.h"
#include "file.h"
#include <errno.h>
#include <fcntl.h>
#include <lzma.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const struct ksu_asset *find_asset(const char *name)
{
	for (size_t i = 0; i < ksu_asset_count; ++i)
		if (!strcmp(name, ksu_assets[i].name))
			return &ksu_assets[i];
	errno = ENOENT;
	return NULL;
}

static int write_asset(const struct ksu_asset *asset, int fd)
{
	lzma_stream stream = LZMA_STREAM_INIT;
	lzma_ret code = lzma_stream_decoder(&stream, UINT64_MAX, 0);
	if (code != LZMA_OK) {
		errno = code == LZMA_MEM_ERROR ? ENOMEM : EINVAL;
		return -1;
	}
	stream.next_in = asset->data;
	stream.avail_in = asset->packed_size;
	unsigned char buffer[16384];
	int result = -1;
	do {
		stream.next_out = buffer;
		stream.avail_out = sizeof(buffer);
		code = lzma_code(&stream, LZMA_FINISH);
		if (code != LZMA_OK && code != LZMA_STREAM_END) {
			errno = code == LZMA_MEM_ERROR ? ENOMEM : EINVAL;
			goto done;
		}
		size_t count = sizeof(buffer) - stream.avail_out;
		for (size_t offset = 0; offset < count;) {
			ssize_t written = write(fd, buffer + offset, count - offset);
			if (written < 0 && errno == EINTR)
				continue;
			if (written <= 0) {
				if (!written)
					errno = EIO;
				goto done;
			}
			offset += (size_t)written;
		}
	} while (code != LZMA_STREAM_END);
	if (stream.total_out == asset->size && !stream.avail_in)
		result = 0;
	else
		errno = EINVAL;
done:
	int saved = errno;
	lzma_end(&stream);
	errno = saved;
	return result;
}

int ksu_asset_write(const char *name, int fd)
{
	const struct ksu_asset *asset = find_asset(name);
	return asset ? write_asset(asset, fd) : -1;
}

int ksu_asset_extract(const char *name, const char *path, bool ignore_if_exists)
{
	const struct ksu_asset *asset = find_asset(name);
	if (!asset)
		return -1;
	struct stat st;
	if (ignore_if_exists && !stat(path, &st))
		return 0;
	const char *slash = strrchr(path, '/');
	char *parent =
	    slash ? strndup(path, slash == path ? 1 : (size_t)(slash - path)) : strdup(".");
	if (!parent)
		return -1;
	int result = ksu_mkdirs(parent);
	free(parent);
	if (result || (unlink(path) && errno != ENOENT))
		return -1;
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
	if (fd < 0)
		return -1;
	result = write_asset(asset, fd);
	if (!result)
		result = fchmod(fd, 0755);
	int saved = errno;
	close(fd);
	errno = saved;
	return result;
}

int ksu_assets_ensure(const char *directory, const char *daemon, bool ignore_if_exists)
{
	for (size_t i = 0; i < ksu_asset_count; ++i) {
		if (!strcmp(ksu_assets[i].name, "waitsys"))
			continue;
		char *path = ksu_join_path(directory, ksu_assets[i].name);
		if (!path)
			return -1;
		int result = ksu_asset_extract(ksu_assets[i].name, path, ignore_if_exists);
		free(path);
		if (result)
			return -1;
	}
	char *path = ksu_join_path(directory, "resetprop");
	if (!path)
		return -1;
	unlink(path);
	int result = symlink(daemon, path);
	int saved = errno;
	free(path);
	errno = saved;
	return result;
}
