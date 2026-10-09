// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef KSUD_FILE_H
#define KSUD_FILE_H

#include <stddef.h>
#include <stdio.h>

int ksu_mkdirs(const char *path);
int ksu_mkdirs_mode(const char *path, unsigned int mode);
char *ksu_join_path(const char *directory, const char *name);
unsigned char *ksu_read_file(const char *path, size_t *length);
unsigned char *ksu_read_stream(FILE *file, size_t limit, size_t *length);
int ksu_write_file(const char *path, const void *data, size_t length);
int ksu_copy_file(const char *source, const char *destination);

#endif
