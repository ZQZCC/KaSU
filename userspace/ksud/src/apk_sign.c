// SPDX-License-Identifier: GPL-3.0-or-later
#include "cli.h"
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

void ksu_sha256(const unsigned char *data, size_t size, char hash[65]);

struct span {
	const unsigned char *data;
	size_t size;
};

static uint32_t le32(const unsigned char *data)
{
	return (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 |
	       (uint32_t)data[3] << 24;
}

static uint64_t le64(const unsigned char *data)
{
	return le32(data) | (uint64_t)le32(data + 4) << 32;
}

static int invalid(const char *message)
{
	fprintf(stderr, "%s\n", message);
	errno = 0;
	return -1;
}

static bool blob(struct span *input, struct span *output)
{
	if (input->size < 4)
		return false;
	uint32_t size = le32(input->data);
	if (size > input->size - 4)
		return false;
	*output = (struct span){input->data + 4, size};
	input->data += 4 + size;
	input->size -= 4 + size;
	return true;
}

static int signature(struct span file, uint32_t *size, char hash[65])
{
	if (file.size < 22)
		return invalid("not a zip file");
	size_t comment = 0;
	const unsigned char *eocd = NULL;
	for (; comment <= UINT16_MAX && comment <= file.size - 22; ++comment) {
		const unsigned char *end = file.data + file.size - 22 - comment;
		if (le32(end) == 0x06054b50 &&
		    ((unsigned)end[20] | (unsigned)end[21] << 8) == comment) {
			eocd = end;
			break;
		}
	}
	if (!eocd)
		return invalid("not a zip file");
	if (comment)
		printf("warning: comment length is %zu\n", comment);
	size_t directory = le32(eocd + 16);
	if (directory < 24 || directory > (size_t)(eocd - file.data))
		return invalid("Can not found sig block");
	const unsigned char *footer = file.data + directory - 24;
	if (memcmp(footer + 8, "APK Sig Block 42", 16))
		return invalid("Can not found sig block");
	uint64_t block_size = le64(footer);
	if (block_size < 24 || block_size > directory - 8)
		return invalid("not a signed apk");
	const unsigned char *start = file.data + directory - (size_t)block_size - 8;
	if (le64(start) != block_size)
		return invalid("not a signed apk");
	struct span pairs = {start + 8, (size_t)(footer - start - 8)};
	bool found = false, v3 = false;
	while (pairs.size) {
		if (pairs.size < 8)
			return invalid("not a signed apk");
		uint64_t length = le64(pairs.data);
		if (length < 4 || length > pairs.size - 8)
			return invalid("not a signed apk");
		uint32_t id = le32(pairs.data + 8);
		if (id == 0xf05368c0 || id == 0x1b93ad61)
			v3 = true;
		if (id == 0x7109871a) {
			struct span value = {pairs.data + 12, (size_t)length - 4};
			struct span signers, signer, signed_data, digests, certificates,
			    certificate;
			if (!blob(&value, &signers) || !blob(&signers, &signer) ||
			    !blob(&signer, &signed_data) || !blob(&signed_data, &digests) ||
			    !blob(&signed_data, &certificates) ||
			    !blob(&certificates, &certificate))
				return invalid("not a signed apk");
			*size = (uint32_t)certificate.size;
			ksu_sha256(certificate.data, certificate.size, hash);
			found = true;
		}
		pairs.data += 8 + (size_t)length;
		pairs.size -= 8 + (size_t)length;
	}
	if (v3)
		return invalid("Unexpected v3 signature found!");
	return found ? 0 : invalid("No signature found!");
}

int ksu_apk_signature(const char *path, uint32_t *size, char hash[65])
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	struct stat st;
	if (fstat(fd, &st)) {
		int error = errno;
		close(fd);
		errno = error;
		return -1;
	}
	if (!st.st_size) {
		close(fd);
		return invalid("not a zip file");
	}
	void *data = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	int error = errno;
	close(fd);
	if (data == MAP_FAILED) {
		errno = error;
		return -1;
	}
	int result = signature((struct span){data, (size_t)st.st_size}, size, hash);
	error = errno;
	munmap(data, (size_t)st.st_size);
	errno = error;
	return result;
}
