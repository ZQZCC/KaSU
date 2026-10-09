// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#define __packed __attribute__((packed))
#include "uapi/sulog.h"
#undef __packed
#include "config.h"
#include "driver.h"
#include "file.h"
#include "process.h"
#include "text.h"
#include "uapi/supercall.h"
#include <android/log.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct event_header {
	uint16_t type, flags;
	uint32_t length;
	uint64_t sequence, timestamp;
} __attribute__((packed));

struct dropped_info {
	uint64_t dropped, first, last;
} __attribute__((packed));

_Static_assert(sizeof(struct event_header) == 24, "event queue header changed");
_Static_assert(sizeof(struct ksu_sulog_event) == 52, "sulog event changed");

struct log_writer {
	const char *directory, *config_directory;
	char day[32];
	uint32_t index;
	uint64_t size, maximum;
	FILE *file;
};

static void skip_space(const unsigned char **cursor, const unsigned char *end)
{
	while (*cursor < end) {
		const unsigned char *next = *cursor;
		if (!ksu_unicode_space(ksu_utf8_next(&next, end)))
			break;
		*cursor = next;
	}
}

static bool decimal(const unsigned char **cursor, const unsigned char *end, size_t width,
		    uint64_t maximum, uint64_t *value)
{
	const unsigned char *start = *cursor;
	*value = 0;
	while (*cursor < end && (size_t)(*cursor - start) < width && **cursor >= '0' &&
	       **cursor <= '9') {
		unsigned int digit = *(*cursor)++ - '0';
		if (*value > maximum / 10 || (*value == maximum / 10 && digit > maximum % 10))
			return false;
		*value = *value * 10 + digit;
	}
	return *cursor != start;
}

static bool parse_date(const unsigned char *data, size_t length, int64_t *day)
{
	const unsigned char *cursor = data, *end = data + length;
	uint64_t year, month, date;
	skip_space(&cursor, end);
	bool negative = cursor < end && *cursor == '-';
	bool sign = cursor < end && (*cursor == '-' || *cursor == '+');
	if (sign)
		++cursor;
	if (!decimal(&cursor, end, sign ? SIZE_MAX : 4, INT64_MAX, &year))
		return false;
	if (year > (negative ? 262143u : 262142u) || cursor == end || *cursor++ != '-')
		return false;
	skip_space(&cursor, end);
	if (!decimal(&cursor, end, 2, 12, &month) || !month || cursor == end || *cursor++ != '-')
		return false;
	skip_space(&cursor, end);
	if (!decimal(&cursor, end, 2, 31, &date) || !date || cursor != end)
		return false;
	int signed_year = negative ? -(int)year : (int)year;
	const int month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	bool leap = signed_year % 4 == 0 && (signed_year % 100 != 0 || signed_year % 400 == 0);
	if (date > (uint64_t)(month_days[month - 1] + (month == 2 && leap)))
		return false;
	struct tm tm = {
	    .tm_year = signed_year - 1900, .tm_mon = (int)month - 1, .tm_mday = (int)date};
	*day = timegm(&tm) / 86400;
	return true;
}

static bool parse_log_name(const char *name, int64_t *day, uint32_t *index)
{
	size_t length = strlen(name);
	if (length < 10 || !ksu_valid_utf8(name, length) || strncmp(name, "sulog-", 6) ||
	    strcmp(name + length - 4, ".log"))
		return false;
	const unsigned char *start = (const unsigned char *)name + 6, *end = start + length - 10;
	if (end - start == 10) {
		*index = 0;
		return parse_date(start, 10, day);
	}
	const unsigned char *separator = end;
	while (separator > start && separator[-1] != '-')
		--separator;
	bool canonical = separator > start && separator - start == 11;
	for (const unsigned char *p = separator; canonical && p < end; ++p)
		canonical = *p >= '0' && *p <= '9';
	if (!canonical) {
		separator = end;
		while (separator > start && separator[-1] != '.')
			--separator;
		if (separator == start)
			return false;
	}
	const unsigned char *number = separator;
	if (!canonical && number < end && *number == '+')
		++number;
	uint64_t parsed;
	if (!decimal(&number, end, SIZE_MAX, UINT32_MAX, &parsed) || number != end ||
	    !parse_date(start, (size_t)(separator - start - 1), day))
		return false;
	*index = parsed;
	return true;
}

static int current_day(char day[32], int64_t *ordinal)
{
	time_t now = time(NULL);
	struct tm tm;
	if (!localtime_r(&now, &tm) || !strftime(day, 32, "%Y-%m-%d", &tm) ||
	    !parse_date((const unsigned char *)day, strlen(day), ordinal)) {
		errno = EOVERFLOW;
		return -1;
	}
	return 0;
}

static int config_value(const char *directory, const char *key, const char *fallback,
			uint64_t *value)
{
	struct ksu_config_view view;
	if (ksu_config_view_load(directory, &view))
		return -1;
	const struct ksu_string_pair *entry =
	    ksu_strings_find(&view.merged, (const unsigned char *)key, strlen(key));
	const unsigned char *data = entry ? entry->value : (const unsigned char *)fallback;
	size_t length = entry ? entry->value_length : strlen(fallback);
	int result = 0;
	if (!entry)
		result = ksu_config_run(KSU_CONFIG_SET, directory, 0, (const unsigned char *)key,
					strlen(key), data, length);
	if (!result) {
		ksu_trim_space(&data, &length);
		const unsigned char *end = data + length;
		if (data < end && *data == '+')
			++data;
		if (!decimal(&data, end, SIZE_MAX, UINT64_MAX, value) || data != end || !*value) {
			__android_log_print(ANDROID_LOG_ERROR, "KernelSU", "invalid %s value", key);
			errno = EINVAL;
			result = -1;
		}
	}
	int error = errno;
	ksu_config_view_free(&view);
	errno = error;
	return result;
}

static int scan_logs(const char *directory, int64_t today, uint64_t retention, uint32_t *highest)
{
	*highest = 0;
	int64_t minimum;
	parse_date((const unsigned char *)"-262143-01-01", 13, &minimum);
	if (retention - 1 > (uint64_t)(today - minimum)) {
		errno = EOVERFLOW;
		return -1;
	}
	int64_t cutoff = today - (int64_t)(retention - 1);
	DIR *dir = opendir(directory);
	if (!dir)
		return -1;
	int result = 0;
	for (;;) {
		errno = 0;
		struct dirent *entry = readdir(dir);
		if (!entry) {
			result = errno ? -1 : 0;
			break;
		}
		int64_t day;
		uint32_t index;
		if (!parse_log_name(entry->d_name, &day, &index))
			continue;
		if (day == today && index > *highest)
			*highest = index;
		if (day >= cutoff)
			continue;
		if (unlinkat(dirfd(dir), entry->d_name, 0)) {
			result = -1;
			break;
		}
		__android_log_print(ANDROID_LOG_INFO, "KernelSU",
				    "removed expired sulog log %s/%s, retention_days=%" PRIu64,
				    directory, entry->d_name, retention);
	}
	int error = errno;
	closedir(dir);
	errno = error;
	return result;
}

static char *log_path(const struct log_writer *writer, const char *day, uint32_t index)
{
	char name[64];
	if (index)
		snprintf(name, sizeof(name), "sulog-%s-%" PRIu32 ".log", day, index);
	else
		snprintf(name, sizeof(name), "sulog-%s.log", day);
	return ksu_join_path(writer->directory, name);
}

static FILE *open_log(const char *path)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
	if (fd < 0)
		return NULL;
	FILE *file = NULL;
	if (!fchmod(fd, 0600))
		file = fdopen(fd, "a");
	if (!file) {
		int error = errno;
		close(fd);
		errno = error;
	}
	return file;
}

static int open_day(struct log_writer *writer, const char *day, int64_t today)
{
	uint64_t retention, maximum;
	uint32_t highest;
	if (config_value(writer->config_directory, "log.retention.days", "3", &retention) ||
	    config_value(writer->config_directory, "log.max_file_size", "10485760", &maximum) ||
	    scan_logs(writer->directory, today, retention, &highest))
		return -1;
	int error;
	char *path = log_path(writer, day, highest);
	if (!path)
		return -1;
	struct stat st;
	uint64_t size = stat(path, &st) ? 0 : (uint64_t)st.st_size;
	if (size && size >= maximum) {
		if (highest < UINT32_MAX)
			++highest;
		free(path);
		path = log_path(writer, day, highest);
		if (!path)
			return -1;
		size = stat(path, &st) ? 0 : (uint64_t)st.st_size;
	}
	FILE *file = open_log(path);
	error = errno;
	free(path);
	if (!file) {
		errno = error;
		return -1;
	}
	if (writer->file)
		fclose(writer->file);
	writer->file = file;
	strcpy(writer->day, day);
	writer->index = highest;
	writer->size = size;
	writer->maximum = maximum;
	return 0;
}

static int write_line(struct log_writer *writer, const char *line, size_t length)
{
	char day[32];
	int64_t today;
	if (current_day(day, &today))
		return -1;
	if (strcmp(day, writer->day)) {
		if (open_day(writer, day, today))
			return -1;
	} else if (writer->size && (length + 1 > writer->maximum ||
				    writer->size > writer->maximum - (length + 1))) {
		uint32_t index = writer->index == UINT32_MAX ? UINT32_MAX : writer->index + 1;
		char *path = log_path(writer, day, index);
		if (!path)
			return -1;
		FILE *file = open_log(path);
		int error = errno;
		free(path);
		if (!file) {
			errno = error;
			return -1;
		}
		fclose(writer->file);
		writer->file = file;
		writer->index = index;
		writer->size = 0;
	}
	if (fwrite(line, 1, length, writer->file) != length || fputc('\n', writer->file) == EOF ||
	    fflush(writer->file))
		return -1;
	writer->size =
	    length + 1 > UINT64_MAX - writer->size ? UINT64_MAX : writer->size + length + 1;
	return 0;
}

static void escape_field(FILE *out, const void *bytes, size_t length)
{
	const unsigned char *data = bytes, *end = memchr(data, 0, length);
	if (!end)
		end = data + length;
	while (data < end) {
		const unsigned char *start = data;
		while (data < end && *data >= 32 && *data < 127 && *data != '\\' && *data != '"')
			++data;
		if (data != start)
			fwrite(start, 1, (size_t)(data - start), out);
		if (data == end)
			break;
		uint32_t c = ksu_utf8_next(&data, end);
		switch (c) {
		case '\\':
			fputs("\\\\", out);
			break;
		case '"':
			fputs("\\\"", out);
			break;
		case '\n':
			fputs("\\n", out);
			break;
		case '\r':
			fputs("\\r", out);
			break;
		case '\t':
			fputs("\\t", out);
			break;
		default:
			if (c < 32 || (c >= 127 && c <= 159)) {
				fprintf(out, "\\x%02" PRIx32, c);
			} else {
				unsigned char utf8[4];
				fwrite(utf8, 1, ksu_utf8_put(utf8, c), out);
			}
		}
	}
}

static int format_record(const struct event_header *header, const unsigned char *payload,
			 char *line, size_t capacity)
{
	struct ksu_sulog_event event;
	struct dropped_info dropped;
	if (header->type == UINT16_MAX) {
		if (!(header->flags & 1) || header->length < sizeof(dropped))
			goto invalid;
		memcpy(&dropped, payload, sizeof(dropped));
	} else {
		if (header->length < sizeof(event))
			goto invalid;
		memcpy(&event, payload, sizeof(event));
		if (sizeof(event) + (uint64_t)event.filename_len + event.argv_len != header->length)
			goto invalid;
	}
	FILE *out = fmemopen(line, capacity, "w");
	if (!out)
		return -1;
	fprintf(out, "ts_ns=%" PRIu64 " seq=%" PRIu64 " type=", header->timestamp,
		header->sequence);
	if (header->type == UINT16_MAX) {
		fprintf(out, "dropped dropped=%" PRIu64 " first_seq=%" PRIu64 " last_seq=%" PRIu64,
			dropped.dropped, dropped.first, dropped.last);
	} else {
		const char *name = event.event_type == 1   ? "root_execve"
				   : event.event_type == 2 ? "sucompat"
				   : event.event_type == 3 ? "ioctl_grant_root"
							   : "unknown";
		fprintf(out,
			"%s version=%u retval=%d pid=%u tgid=%u ppid=%u uid=%u euid=%u comm=\"",
			name, event.version, event.retval, event.pid, event.tgid, event.ppid,
			event.uid, event.euid);
		escape_field(out, event.comm, sizeof(event.comm));
		fputs("\" file=\"", out);
		escape_field(out, payload + sizeof(event), event.filename_len);
		fputs("\" argv=\"", out);
		escape_field(out, payload + sizeof(event) + event.filename_len, event.argv_len);
		putc('"', out);
	}
	int failed = ferror(out);
	if (fclose(out))
		failed = 1;
	return failed ? -1 : 0;
invalid:
	errno = EINVAL;
	return -1;
}

static int read_queue(int fd, struct log_writer *writer)
{
	unsigned char buffer[8192];
	/* Escapes use at most four bytes per input byte; numeric fields fit in 512. */
	char line[sizeof(buffer) * 4 + 512];
	for (;;) {
		ssize_t length = read(fd, buffer, sizeof(buffer));
		if (length < 0) {
			if (errno == EINTR)
				continue;
			return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
		}
		if (!length)
			return 1;
		size_t offset = 0;
		while (offset < (size_t)length) {
			size_t remaining = (size_t)length - offset;
			if (remaining < sizeof(struct event_header)) {
				__android_log_print(
				    ANDROID_LOG_WARN, "KernelSU",
				    "dropping truncated sulog frame header: %zu bytes remaining",
				    remaining);
				break;
			}
			struct event_header header;
			memcpy(&header, buffer + offset, sizeof(header));
			size_t frame = sizeof(header) + (size_t)header.length;
			if (remaining < frame) {
				__android_log_print(
				    ANDROID_LOG_WARN, "KernelSU",
				    "dropping truncated sulog frame payload: need %zu, got %zu",
				    frame, remaining);
				break;
			}
			if (format_record(&header, buffer + offset + sizeof(header), line,
					  sizeof(line))) {
				__android_log_print(ANDROID_LOG_WARN, "KernelSU",
						    "dropping malformed sulog record seq=%" PRIu64
						    " type=%u: %s",
						    header.sequence, header.type, strerror(errno));
			} else if (write_line(writer, line, strlen(line)))
				return -1;
			offset += frame;
		}
	}
}

static int open_queue(void)
{
	struct ksu_get_sulog_fd_cmd command = {0};
	int fd = ksu_ioctl(KSU_IOCTL_GET_SULOG_FD, &command);
	if (fd < 0)
		return -1;
	int flags = fcntl(fd, F_GETFL);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
		int error = errno;
		close(fd);
		errno = error;
		return -1;
	}
	return fd;
}

static int private_directory(const char *path)
{
	return ksu_mkdirs_mode(path, 0700) || chmod(path, 0700) ? -1 : 0;
}

static int sulog_session(const char *directory, const char *config_directory, uint64_t restart)
{
	int fd = open_queue();
	if (fd < 0)
		return -1;
	struct log_writer writer = {.directory = directory, .config_directory = config_directory};
	int epoll = -1, result = -1;
	char day[32], *marker = NULL;
	int64_t today;
	if (private_directory(directory) || current_day(day, &today) ||
	    open_day(&writer, day, today))
		goto out;
	size_t boot_length;
	unsigned char *boot_id = ksu_read_file("/proc/sys/kernel/random/boot_id", &boot_length);
	if (!boot_id)
		goto out;
	if (!ksu_valid_utf8(boot_id, boot_length)) {
		free(boot_id);
		errno = EILSEQ;
		goto out;
	}
	const unsigned char *boot = boot_id;
	ksu_trim_space(&boot, &boot_length);
	size_t marker_length;
	FILE *out = open_memstream(&marker, &marker_length);
	if (!out) {
		free(boot_id);
		goto out;
	}
	fprintf(out, "type=%s boot_id=\"", restart ? "daemon_restart" : "daemon_start");
	escape_field(out, boot, boot_length);
	putc('"', out);
	if (restart)
		fprintf(out, " restart=%" PRIu64, restart);
	int failed = ferror(out);
	if (fclose(out))
		failed = 1;
	free(boot_id);
	if (failed)
		goto out;
	epoll = epoll_create1(EPOLL_CLOEXEC);
	if (epoll < 0)
		goto out;
	struct epoll_event event = {.events = EPOLLIN | EPOLLERR | EPOLLHUP, .data.fd = fd};
	if (epoll_ctl(epoll, EPOLL_CTL_ADD, fd, &event))
		goto out;
	__android_log_print(ANDROID_LOG_INFO, "KernelSU",
			    "sulogd session started, restart=%" PRIu64, restart);
	if (write_line(&writer, marker, marker_length))
		goto out;
	for (;;) {
		struct epoll_event events[4];
		int ready = epoll_wait(epoll, events, 4, -1);
		if (ready < 0) {
			if (errno == EINTR)
				continue;
			goto out;
		}
		for (int i = 0; i < ready; ++i) {
			if (events[i].events & EPOLLIN) {
				int state = read_queue(fd, &writer);
				if (state < 0)
					goto out;
				if (state == 1) {
					result = 1;
					goto out;
				}
			}
			if (events[i].events & (EPOLLHUP | EPOLLERR)) {
				if (read_queue(fd, &writer) < 0)
					goto out;
				result = 2;
				goto out;
			}
		}
	}
out:
	int error = errno;
	free(marker);
	if (epoll >= 0)
		close(epoll);
	if (writer.file)
		fclose(writer.file);
	close(fd);
	errno = error;
	return result;
}

int ksu_start_sulogd(void)
{
	int child = ksu_create_daemon(false);
	if (child <= 0)
		return child;
	if (!chdir("/"))
		execl("/proc/self/exe", "/proc/self/exe", "sulogd", (char *)NULL);
	__android_log_print(ANDROID_LOG_ERROR, "KernelSU", "failed to exec sulogd: %s",
			    strerror(errno));
	_exit(1);
}

void ksu_ensure_sulogd(void)
{
	if (ksu_start_sulogd())
		__android_log_print(ANDROID_LOG_WARN, "KernelSU",
				    "failed to ensure sulogd is running after feature init: %s",
				    strerror(errno));
}

int ksu_sulog_daemon(const char *working_directory, const char *log_directory,
		     const char *config_directory)
{
	if (private_directory(working_directory))
		return -1;
	char *lock_path = ksu_join_path(working_directory, "sulogd.lock");
	if (!lock_path)
		return -1;
	int lock = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600), error = errno;
	free(lock_path);
	if (lock < 0) {
		errno = error;
		return -1;
	}
	if (flock(lock, LOCK_EX | LOCK_NB)) {
		error = errno;
		close(lock);
		if (error == EWOULDBLOCK || error == EAGAIN) {
			__android_log_print(ANDROID_LOG_INFO, "KernelSU",
					    "sulogd lock is held, skipping start");
			return 0;
		}
		errno = error;
		return -1;
	}
	uint64_t restart = 0;
	for (;;) {
		int result = sulog_session(log_directory, config_directory, restart);
		if (result < 0)
			__android_log_print(ANDROID_LOG_WARN, "KernelSU",
					    "sulogd session failed: %s; restarting in 3s",
					    strerror(errno));
		else
			__android_log_print(ANDROID_LOG_WARN, "KernelSU",
					    "restarting sulogd session after %s in 3s",
					    result == 1 ? "fd close" : "hangup");
		if (restart < UINT64_MAX)
			++restart;
		struct timespec delay = {.tv_sec = 3};
		while (nanosleep(&delay, &delay) && errno == EINTR)
			;
	}
}
