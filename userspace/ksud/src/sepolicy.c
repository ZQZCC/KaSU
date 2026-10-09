// SPDX-License-Identifier: GPL-3.0-or-later
#include "driver.h"
#include "file.h"
#include "native.h"
#include "text.h"
#include "uapi/selinux.h"
#include "uapi/supercall.h"
#include <android/log.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct policy_span {
	const unsigned char *data;
	size_t length;
};

struct policy_object {
	struct policy_span values[100];
	size_t count;
};

struct policy_syntax {
	const char *name, *type, *recipe;
	const char *fields[5];
	const __u32 *command, *subcommand;
};

/* S: wildcard/group, G: group, W: word, d: default domain, o: optional word. */
static const struct policy_syntax syntax[] = {
    {"allow",
     "NormalPerm",
     "SSSS",
     {"source", "target", "class", "perm"},
     &KSU_SEPOLICY_CMD_NORMAL_PERM,
     &KSU_SEPOLICY_SUBCMD_NORMAL_PERM_ALLOW},
    {"deny",
     "NormalPerm",
     "SSSS",
     {"source", "target", "class", "perm"},
     &KSU_SEPOLICY_CMD_NORMAL_PERM,
     &KSU_SEPOLICY_SUBCMD_NORMAL_PERM_DENY},
    {"auditallow",
     "NormalPerm",
     "SSSS",
     {"source", "target", "class", "perm"},
     &KSU_SEPOLICY_CMD_NORMAL_PERM,
     &KSU_SEPOLICY_SUBCMD_NORMAL_PERM_AUDITALLOW},
    {"dontaudit",
     "NormalPerm",
     "SSSS",
     {"source", "target", "class", "perm"},
     &KSU_SEPOLICY_CMD_NORMAL_PERM,
     &KSU_SEPOLICY_SUBCMD_NORMAL_PERM_DONTAUDIT},
    {"allowxperm",
     "XPerm",
     "SSSWS",
     {"source", "target", "class", "operation", "perm_set"},
     &KSU_SEPOLICY_CMD_XPERM,
     &KSU_SEPOLICY_SUBCMD_XPERM_ALLOW},
    {"auditallowxperm",
     "XPerm",
     "SSSWS",
     {"source", "target", "class", "operation", "perm_set"},
     &KSU_SEPOLICY_CMD_XPERM,
     &KSU_SEPOLICY_SUBCMD_XPERM_AUDITALLOW},
    {"dontauditxperm",
     "XPerm",
     "SSSWS",
     {"source", "target", "class", "operation", "perm_set"},
     &KSU_SEPOLICY_CMD_XPERM,
     &KSU_SEPOLICY_SUBCMD_XPERM_DONTAUDIT},
    {"permissive",
     "TypeState",
     "G",
     {"stype"},
     &KSU_SEPOLICY_CMD_TYPE_STATE,
     &KSU_SEPOLICY_SUBCMD_TYPE_STATE_PERMISSIVE},
    {"enforce",
     "TypeState",
     "G",
     {"stype"},
     &KSU_SEPOLICY_CMD_TYPE_STATE,
     &KSU_SEPOLICY_SUBCMD_TYPE_STATE_ENFORCE},
    {"type", "Type", "Wd", {"name", "attrs"}, &KSU_SEPOLICY_CMD_TYPE, NULL},
    {"typeattribute", "TypeAttr", "GG", {"stype", "sattr"}, &KSU_SEPOLICY_CMD_TYPE_ATTR, NULL},
    {"attradd", "TypeAttr", "GG", {"stype", "sattr"}, &KSU_SEPOLICY_CMD_TYPE_ATTR, NULL},
    {"attribute", "Attr", "W", {"name"}, &KSU_SEPOLICY_CMD_ATTR, NULL},
    {"type_transition",
     "TypeTransition",
     "WWWWo",
     {"source", "target", "class", "default_type", "object_name"},
     &KSU_SEPOLICY_CMD_TYPE_TRANSITION,
     NULL},
    {"name_transition",
     "TypeTransition",
     "WWWWo",
     {"source", "target", "class", "default_type", "object_name"},
     &KSU_SEPOLICY_CMD_TYPE_TRANSITION,
     NULL},
    {"type_change",
     "TypeChange",
     "WWWW",
     {"source", "target", "class", "default_type"},
     &KSU_SEPOLICY_CMD_TYPE_CHANGE,
     &KSU_SEPOLICY_SUBCMD_TYPE_CHANGE_CHANGE},
    {"type_member",
     "TypeChange",
     "WWWW",
     {"source", "target", "class", "default_type"},
     &KSU_SEPOLICY_CMD_TYPE_CHANGE,
     &KSU_SEPOLICY_SUBCMD_TYPE_CHANGE_MEMBER},
    {"genfscon",
     "GenFsCon",
     "WWW",
     {"fs_name", "partial_path", "fs_context"},
     &KSU_SEPOLICY_CMD_GENFSCON,
     NULL},
};

struct policy_rule {
	const struct policy_syntax *syntax;
	struct policy_object objects[5];
};

struct policy_payload {
	unsigned char *data;
	size_t length, capacity, count;
};

static bool word_char(unsigned char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
	       c == '_' || c == '-';
}

static struct policy_span parse_word(const unsigned char **cursor, const unsigned char *end)
{
	const unsigned char *start = *cursor;
	while (*cursor < end && word_char(**cursor))
		++*cursor;
	return (struct policy_span){start, (size_t)(*cursor - start)};
}

static bool parse_group(const unsigned char **cursor, const unsigned char *end,
			struct policy_object *object)
{
	const unsigned char *start = ++*cursor, *p = start;
	size_t characters = 0;
	while (p < end && *p != '}' && characters < 100) {
		uint32_t c = ksu_utf8_next(&p, end);
		if (!(c <= 127 && word_char(c)) && !ksu_unicode_space(c))
			return false;
		++characters;
	}
	if (!characters || p == end || *p != '}')
		return false;
	*cursor = p + 1;
	while (start < p) {
		const unsigned char *next = start;
		if (ksu_unicode_space(ksu_utf8_next(&next, p))) {
			start = next;
			continue;
		}
		object->values[object->count++] = parse_word(&start, p);
	}
	return object->count != 0;
}

static bool parse_rule(struct policy_span line, struct policy_rule *rule)
{
	const unsigned char *p = line.data, *end = p + line.length;
	struct policy_span name = parse_word(&p, end);
	rule->syntax = NULL;
	for (size_t i = 0; i < sizeof(syntax) / sizeof(*syntax); ++i) {
		if (name.length == strlen(syntax[i].name) &&
		    !memcmp(name.data, syntax[i].name, name.length)) {
			rule->syntax = &syntax[i];
			break;
		}
	}
	if (!rule->syntax)
		return false;
	for (size_t i = 0; rule->syntax->recipe[i]; ++i) {
		char kind = rule->syntax->recipe[i];
		struct policy_object *object = &rule->objects[i];
		object->count = 0;
		if (p == end && (kind == 'd' || kind == 'o')) {
			object->count = 1;
			object->values[0] =
			    kind == 'd' ? (struct policy_span){(const unsigned char *)"domain", 6}
					: (struct policy_span){0};
			continue;
		}
		const unsigned char *separator = p;
		while (p < end && (*p == ' ' || *p == '\t'))
			++p;
		if (p == separator || p == end)
			return false;
		if ((kind == 'S' || kind == 'G' || kind == 'd') && *p == '{') {
			if (!parse_group(&p, end, object))
				return false;
		} else {
			struct policy_span word;
			if (kind == 'S' && *p == '*')
				word = (struct policy_span){p++, 1};
			else
				word = parse_word(&p, end);
			if (!word.length)
				return false;
			object->values[0] = word;
			object->count = 1;
		}
	}
	return true;
}

static void print_word(struct policy_span word)
{
	putchar('"');
	fwrite(word.data, 1, word.length, stdout);
	putchar('"');
}

static void print_rule(const struct policy_rule *rule)
{
	const struct policy_syntax *s = rule->syntax;
	printf("%s(%s { ", s->type, s->type);
	if (s->subcommand)
		printf("op: \"%s\", ", s->name);
	for (size_t i = 0; s->recipe[i]; ++i) {
		const struct policy_object *object = &rule->objects[i];
		char kind = s->recipe[i];
		if (i)
			fputs(", ", stdout);
		printf("%s: ", s->fields[i]);
		if (kind == 'o' && !object->values[0].length) {
			fputs("None", stdout);
		} else if (kind == 'W' || kind == 'o') {
			if (kind == 'o')
				fputs("Some(", stdout);
			print_word(object->values[0]);
			if (kind == 'o')
				putchar(')');
		} else {
			putchar('[');
			for (size_t j = 0; j < object->count; ++j) {
				if (j)
					fputs(", ", stdout);
				print_word(object->values[j]);
			}
			putchar(']');
		}
	}
	fputs(" })\n", stdout);
}

static unsigned char *payload_reserve(struct policy_payload *payload, size_t length)
{
	if (length > SIZE_MAX - payload->length) {
		errno = EOVERFLOW;
		return NULL;
	}
	size_t required = payload->length + length;
	if (required > payload->capacity) {
		size_t capacity = payload->capacity ? payload->capacity : 256;
		while (capacity < required && capacity <= SIZE_MAX / 2)
			capacity *= 2;
		if (capacity < required)
			capacity = required;
		unsigned char *grown = realloc(payload->data, capacity);
		if (!grown)
			return NULL;
		payload->data = grown;
		payload->capacity = capacity;
	}
	unsigned char *position = payload->data + payload->length;
	payload->length = required;
	return position;
}

static int append_rule(struct policy_payload *payload, const struct policy_rule *rule)
{
	size_t indices[5] = {0}, count = strlen(rule->syntax->recipe);
	__u32 command[] = {*rule->syntax->command,
			   rule->syntax->subcommand ? *rule->syntax->subcommand : 0};
	for (;;) {
		struct policy_span values[5];
		size_t required = sizeof(command);
		for (size_t i = 0; i < count; ++i) {
			struct policy_span value = rule->objects[i].values[indices[i]];
			if (value.length == 1 && *value.data == '*')
				value.length = 0;
			if (value.length > UINT32_MAX ||
			    required > SIZE_MAX - sizeof(uint32_t) - 1 ||
			    value.length > SIZE_MAX - required - sizeof(uint32_t) - 1) {
				errno = EOVERFLOW;
				return -1;
			}
			values[i] = value;
			required += sizeof(uint32_t) + value.length + 1;
		}
		unsigned char *position = payload_reserve(payload, required);
		if (!position)
			return -1;
		memcpy(position, command, sizeof(command));
		position += sizeof(command);
		for (size_t i = 0; i < count; ++i) {
			uint32_t length = values[i].length;
			memcpy(position, &length, sizeof(length));
			position += sizeof(length);
			if (length)
				memcpy(position, values[i].data, length);
			position += length;
			*position++ = 0;
		}
		++payload->count;
		/* The last object varies fastest, matching the original nested loops. */
		size_t i = count;
		while (i && ++indices[i - 1] == rule->objects[i - 1].count)
			indices[--i] = 0;
		if (!i)
			return 0;
	}
}

int ksu_sepolicy(const unsigned char *data, size_t length, int check_only, char **message)
{
	struct policy_payload payload = {0};
	struct policy_rule rule;
	*message = NULL;
	if (!ksu_valid_utf8(data, length)) {
		errno = EILSEQ;
		return -1;
	}
	ksu_trim_space(&data, &length);
	const unsigned char *cursor = data, *end = data + length;
	int result = 0;
	while (cursor < end) {
		const unsigned char *start = cursor;
		while (cursor < end && *cursor != '\n' && *cursor != ';')
			++cursor;
		struct policy_span line = {start, (size_t)(cursor - start)};
		if (cursor < end)
			++cursor;
		struct policy_span trimmed = line;
		ksu_trim_space(&trimmed.data, &trimmed.length);
		if (!trimmed.length || *trimmed.data == '#')
			continue;
		if (!parse_rule(trimmed, &rule)) {
			if (!check_only)
				continue;
			static const char prefix[] = "Failed to parse policy statement: ";
			if (line.length <= SIZE_MAX - sizeof(prefix))
				*message = malloc(sizeof(prefix) + line.length);
			if (*message) {
				memcpy(*message, prefix, sizeof(prefix) - 1);
				memcpy(*message + sizeof(prefix) - 1, line.data, line.length);
				(*message)[sizeof(prefix) - 1 + line.length] = 0;
			}
			errno = EINVAL;
			result = -1;
			break;
		}
		if (!check_only) {
			print_rule(&rule);
			if (append_rule(&payload, &rule)) {
				result = -1;
				break;
			}
		}
	}
	if (!result && payload.count) {
		struct ksu_set_sepolicy_cmd command = {.data = (uintptr_t)payload.data,
						       .data_len = payload.length};
		int applied = ksu_ioctl(KSU_IOCTL_SET_SEPOLICY, &command);
		if (applied < 0)
			__android_log_print(ANDROID_LOG_WARN, "KernelSU",
					    "apply sepolicy batch failed: %s", strerror(errno));
		else if ((size_t)applied < payload.count)
			__android_log_print(ANDROID_LOG_WARN, "KernelSU",
					    "apply sepolicy batch partially succeeded: %d/%zu",
					    applied, payload.count);
	}
	int error = errno;
	free(payload.data);
	errno = error;
	return result;
}

int ksu_sepolicy_file(const char *path, int check_only, char **message)
{
	size_t length;
	*message = NULL;
	unsigned char *data = ksu_read_file(path, &length);
	if (!data)
		return -1;
	int result = ksu_sepolicy(data, length, check_only, message), error = errno;
	free(data);
	errno = error;
	return result;
}
