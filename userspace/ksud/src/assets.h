// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef KSUD_ASSETS_H
#define KSUD_ASSETS_H

#include <stdbool.h>
#include <stddef.h>

struct ksu_asset {
	const char *name;
	const unsigned char *data;
	size_t packed_size, size;
};

extern const struct ksu_asset ksu_assets[];
extern const size_t ksu_asset_count;
extern const char ksu_installer[], ksu_banner[];
int ksu_asset_write(const char *name, int fd);
int ksu_asset_extract(const char *name, const char *path, bool ignore_if_exists);
int ksu_assets_ensure(const char *directory, const char *daemon, bool ignore_if_exists);

#endif
