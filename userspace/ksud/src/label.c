// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "native.h"
#include "text.h"

#include <errno.h>
#include <fts.h>
#include <string.h>
#include <sys/xattr.h>

int ksu_setfilecon(const char *path, const char *context)
{
	return lsetxattr(path, "security.selinux", context, strlen(context), 0);
}

int ksu_setsyscon(const char *path)
{
	return ksu_setfilecon(path, "u:object_r:system_file:s0");
}

static int restore_label(const char *path, int only_unlabeled)
{
	if (only_unlabeled) {
		static const char unlabeled[] = "u:object_r:unlabeled:s0";
		char value[sizeof(unlabeled) - 1];
		ssize_t size = lgetxattr(path, "security.selinux", value, sizeof(value));
		if (size != 0 &&
		    ((size_t)size != sizeof(value) || memcmp(value, unlabeled, sizeof(value))))
			return 0;
	}
	return ksu_setsyscon(path);
}

int ksu_restore_syscon(const char *root, int only_unlabeled)
{
	if (!*root)
		return 0;
	char *paths[] = {(char *)root, NULL};
	FTS *walk = fts_open(paths, FTS_PHYSICAL | FTS_COMFOLLOW | FTS_NOCHDIR, NULL);
	if (!walk)
		return -1;
	FTSENT *entry;
	while ((entry = fts_read(walk))) {
		if (entry->fts_info == FTS_DP || entry->fts_info == FTS_DNR ||
		    entry->fts_info == FTS_ERR || entry->fts_info == FTS_NS ||
		    (!entry->fts_level && entry->fts_info == FTS_SLNONE))
			continue;
		if (entry->fts_level && *entry->fts_name == '.' &&
		    ksu_valid_utf8(entry->fts_name, entry->fts_namelen)) {
			fts_set(walk, entry, FTS_SKIP);
			continue;
		}
		if (restore_label(entry->fts_path, only_unlabeled))
			break;
	}
	int error = errno;
	fts_close(walk);
	errno = error;
	return error ? -1 : 0;
}
