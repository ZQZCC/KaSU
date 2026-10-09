// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static inline void ksu_freep(void *address)
{
	void *pointer;
	memcpy(&pointer, address, sizeof(pointer));
	int error = errno;
	free(pointer);
	errno = error;
}

#define KSU_AUTO_FREE __attribute__((cleanup(ksu_freep)))
