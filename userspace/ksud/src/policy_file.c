// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "file.h"
#include "policy.h"
#include "uapi/app_profile.h"

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define POLICY_MAGIC 0x7f4b5355u
#define POLICY_MAX_FILE (8u + KSU_POLICY_MAX_PROFILES * KSU_POLICY_PROFILE_SIZE)

_Static_assert(KSU_APP_PROFILE_VER == 4, "stored profile version");
_Static_assert(sizeof(struct app_profile) == KSU_POLICY_PROFILE_SIZE, "profile size");
_Static_assert(offsetof(struct app_profile, key) == 4, "profile key");
_Static_assert(offsetof(struct app_profile, curr_uid) == 260, "profile uid");
_Static_assert(offsetof(struct app_profile, allow_su) == 264, "profile allow flag");
_Static_assert(offsetof(struct app_profile, rp_config.profile.groups_count) == 544,
	       "profile groups");
_Static_assert(offsetof(struct app_profile, rp_config.profile.selinux_domain) == 704,
	       "profile domain");

static uint32_t read_u32(const unsigned char *data)
{
	uint32_t value;
	memcpy(&value, data, sizeof(value));
	return le32toh(value);
}

static bool valid_record(const unsigned char *record)
{
	if (read_u32(record) != KSU_APP_PROFILE_VER || !memchr(record + 4, 0, 256) ||
	    record[264] > 1 || record[272] > 1)
		return false;
	if (record[264]) {
		if (read_u32(record + 544) > 32 || !record[704] || !memchr(record + 704, 0, 64))
			return false;
	} else if (record[273] > 1)
		return false;
	return read_u32(record + 260) != 9999 || (record[4] == '$' && !record[5]);
}

void ksu_policy_free(struct ksu_saved_policy *policy)
{
	free(policy->profiles);
	*policy = (struct ksu_saved_policy){0};
}

int ksu_policy_decode(const unsigned char *data, size_t length, struct ksu_saved_policy *policy)
{
	*policy = (struct ksu_saved_policy){0};
	if (length < 8 || length > POLICY_MAX_FILE || read_u32(data) != POLICY_MAGIC)
		goto invalid;
	uint32_t version = read_u32(data + 4);
	if (version < 2 || version > KSU_APP_PROFILE_VER)
		goto invalid;
	size_t stride = version == KSU_APP_PROFILE_VER ? KSU_POLICY_PROFILE_SIZE : 776;
	size_t count = (length - 8) / stride;
	if ((length - 8) % stride || count > KSU_POLICY_MAX_PROFILES)
		goto invalid;
	policy->profiles = malloc(count ? count * KSU_POLICY_PROFILE_SIZE : 1);
	if (!policy->profiles)
		return -1;
	policy->needs_rewrite = version != KSU_APP_PROFILE_VER;
	size_t capacity = 1;
	while (capacity < count * 2)
		capacity *= 2;
	uint32_t *positions = calloc(capacity, sizeof(*positions));
	if (!positions)
		goto error;
	uint32_t current_version = htole32(KSU_APP_PROFILE_VER);
	for (size_t i = 0; i < count; i++) {
		unsigned char *record = policy->profiles + policy->length;
		memcpy(record, data + 8 + i * stride, stride);
		memset(record + stride, 0, KSU_POLICY_PROFILE_SIZE - stride);
		policy->needs_rewrite |= read_u32(record) != KSU_APP_PROFILE_VER;
		memcpy(record, &current_version, sizeof(current_version));
		if (version < KSU_APP_PROFILE_VER && record[264] == 1) {
			record[776] = 1;
			if (version == 2 && !memcmp(record + 704, "u:r:su:s0", 10)) {
				memset(record + 704, 0, 64);
				memcpy(record + 704, "u:r:ksu:s0", 11);
			}
		}
		if (!valid_record(record)) {
			free(positions);
			goto invalid;
		}
		uint32_t uid = read_u32(record + 260);
		size_t slot = (uint32_t)(uid * 2654435761u) & (capacity - 1);
		while (positions[slot] &&
		       read_u32(policy->profiles + (positions[slot] - 1) * KSU_POLICY_PROFILE_SIZE +
				260) != uid)
			slot = (slot + 1) & (capacity - 1);
		if (positions[slot]) {
			policy->needs_rewrite = true;
			memcpy(policy->profiles + (positions[slot] - 1) * KSU_POLICY_PROFILE_SIZE,
			       record, KSU_POLICY_PROFILE_SIZE);
		} else {
			positions[slot] = policy->length / KSU_POLICY_PROFILE_SIZE + 1;
			policy->length += KSU_POLICY_PROFILE_SIZE;
		}
	}
	free(positions);
	return 0;
invalid:
	errno = EINVAL;
error:
	int error = errno;
	ksu_policy_free(policy);
	errno = error;
	return -1;
}

int ksu_policy_load(const char *path, struct ksu_saved_policy *policy)
{
	*policy = (struct ksu_saved_policy){0};
	FILE *file = fopen(path, "re");
	if (!file) {
		if (errno != ENOENT)
			return -1;
		policy->needs_rewrite = true;
		return 0;
	}
	int result = -1;
	unsigned char *data = NULL;
	struct stat st;
	if (fstat(fileno(file), &st))
		goto done;
	if (!S_ISREG(st.st_mode)) {
		errno = EINVAL;
		goto done;
	}
	size_t length;
	data = ksu_read_stream(file, POLICY_MAX_FILE + 1u, &length);
	if (!data)
		goto done;
	result = ksu_policy_decode(data, length, policy);
done:
	int error = errno;
	free(data);
	fclose(file);
	errno = error;
	return result;
}

int ksu_policy_save(const char *path, const unsigned char *profiles, size_t length)
{
	if (length % KSU_POLICY_PROFILE_SIZE ||
	    length / KSU_POLICY_PROFILE_SIZE > KSU_POLICY_MAX_PROFILES)
		goto invalid;
	for (size_t offset = 0; offset < length; offset += KSU_POLICY_PROFILE_SIZE)
		if (!valid_record(profiles + offset))
			goto invalid;
	char *parent = strdup(path);
	if (!parent)
		return -1;
	char *slash = strrchr(parent, '/');
	if (slash)
		slash[slash == parent] = 0;
	else
		*parent = 0;
	char *temporary = ksu_join_path(parent, ".allowlist.XXXXXX");
	FILE *file = NULL;
	int result = -1;
	bool created = false;
	if (!temporary || ksu_mkdirs(parent))
		goto done;
	int fd = mkostemp(temporary, O_CLOEXEC);
	if (fd < 0)
		goto done;
	created = true;
	if (fchmod(fd, 0644) || !(file = fdopen(fd, "we"))) {
		int error = errno;
		close(fd);
		errno = error;
		goto done;
	}
	uint32_t header[] = {htole32(POLICY_MAGIC), htole32(KSU_APP_PROFILE_VER)};
	if (fwrite(header, 1, sizeof(header), file) != sizeof(header) ||
	    (length && fwrite(profiles, 1, length, file) != length) || fflush(file) || fsync(fd))
		goto done;
	int closed = fclose(file);
	file = NULL;
	if (closed || rename(temporary, path))
		goto done;
	fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		goto done;
	result = fsync(fd);
	int error = errno;
	close(fd);
	errno = error;
done: {
	int error = errno;
	if (file)
		fclose(file);
	if (created)
		unlink(temporary);
	free(temporary);
	free(parent);
	errno = error;
}
	return result;
invalid:
	errno = EINVAL;
	return -1;
}
