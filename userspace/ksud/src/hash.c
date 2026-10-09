// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.h"

/* Use the SHA-256 already built for XZ; no separate crypto engine. */
void ksu_sha256(const unsigned char *data, size_t size, char hash[65])
{
	lzma_check_state check;
	lzma_sha256_init(&check);
	lzma_sha256_update(data, size, &check);
	lzma_sha256_finish(&check);
	static const char digits[] = "0123456789abcdef";
	for (size_t i = 0; i < 32; ++i) {
		hash[i * 2] = digits[check.buffer.u8[i] >> 4];
		hash[i * 2 + 1] = digits[check.buffer.u8[i] & 15];
	}
	hash[64] = 0;
}
