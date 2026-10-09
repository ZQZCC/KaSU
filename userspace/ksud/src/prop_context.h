// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef KSUD_PROP_CONTEXT_H
#define KSUD_PROP_CONTEXT_H

#include <stddef.h>

enum ksu_prop_context_type { KSU_PROP_SERIALIZED, KSU_PROP_SPLIT, KSU_PROP_PRE_SPLIT };

struct ksu_prop_prefix {
	char *data; /* prefix\0context\0 */
	size_t length;
};

struct ksu_prop_context {
	enum ksu_prop_context_type type;
	char *directory;
	unsigned char *data;
	size_t length;
	struct ksu_prop_prefix *prefixes;
	size_t count;
};

int ksu_prop_context_load(struct ksu_prop_context *context, const char *directory,
			  const char *system_root);
void ksu_prop_context_free(struct ksu_prop_context *context);
const char *ksu_prop_context_get_n(const struct ksu_prop_context *context,
				   const unsigned char *name, size_t length);
/* Only the pointer array is owned by the caller; strings belong to context. */
const char **ksu_prop_context_labels(const struct ksu_prop_context *context, size_t *count);
char *ksu_prop_context_path(const struct ksu_prop_context *context, const char *name);

#endif
