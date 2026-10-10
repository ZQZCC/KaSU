// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "cli.h"
#include "assets.h"
#include "boot.h"
#include "driver.h"
#include "file.h"
#include "native.h"
#include "paths.h"
#include "process.h"
#include "text.h"
#include "uapi/ksu.h"
#include <android/log.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { OPT_TEMP = 1, OPT_STDIN = 2, OPT_CONFIG = 4, OPT_GLOBAL = 8, OPT_FLAGS = 16 };

struct arguments {
	char *value[2];
	unsigned count, flags, umount_flags;
};

static int fail(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	fputs("ksud: ", stderr);
	vfprintf(stderr, format, args);
	fputc('\n', stderr);
	va_end(args);
	errno = 0;
	return -1;
}

static int number(const char *text, uint64_t maximum, uint64_t *value)
{
	char *end;
	errno = 0;
	unsigned long long parsed = strtoull(text, &end, 10);
	if ((*text < '0' || *text > '9') && *text != '+')
		return fail("invalid number: %s", text);
	if (!*text || *text == '-' || errno || *end || parsed > maximum)
		return fail("invalid number: %s", text);
	*value = parsed;
	return 0;
}

bool ksu_option_known(const char *arg, const char *shorts, const struct option *options)
{
	if (arg[0] != '-' || !arg[1] || !strcmp(arg, "--"))
		return true;
	if (arg[1] == '-') {
		size_t length = strcspn(arg + 2, "=");
		for (; options->name; ++options)
			if (strlen(options->name) == length &&
			    !strncmp(arg + 2, options->name, length))
				return true;
		return false;
	}
	for (++arg; *arg; ++arg) {
		const char *option = strchr(shorts, *arg);
		if (!option || *arg == ':' || *arg == '+' || *arg == '-')
			return false;
		if (option[1] == ':')
			return true;
	}
	return true;
}

static bool is_help(const char *arg)
{
	return !strcmp(arg, "--help") || !strcmp(arg, "-h");
}

static void help_command(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "help")) {
		memmove(argv + 1, argv + 2, (size_t)(argc - 2) * sizeof(*argv));
		argv[argc - 1] = "--help";
	}
}

static int usage(const char *command, const char *arguments, bool error)
{
	fprintf(error ? stderr : stdout, "Usage: ksud %s%s%s\n", command, *arguments ? " " : "",
		arguments);
	return error ? -2 : 1;
}

/* Borrow argv in order: only stdin values and constructed paths need ownership. */
static int parse(int argc, char **argv, const char *command, const char *syntax, unsigned allowed,
		 unsigned minimum, unsigned maximum, struct arguments *args)
{
	static const struct option options[] = {{"help", no_argument, NULL, 'h'},
						{"temp", no_argument, NULL, 't'},
						{"stdin", no_argument, NULL, 's'},
						{"config", no_argument, NULL, 'c'},
						{"global-mnt", no_argument, NULL, 'g'},
						{"flags", required_argument, NULL, 'f'},
						{NULL, 0, NULL, 0}};
	*args = (struct arguments){0};
	unsigned seen = 0;
	opterr = 0;
	optind = 0;
	int option;
	for (;;) {
		int next = optind ? optind : 1;
		if (next < argc && !ksu_option_known(argv[next], "htcgf:", options))
			return usage(command, syntax, true);
		option = getopt_long(argc, argv, "-:htcgf:", options, NULL);
		if (option == -1)
			break;
		if (option == 'h')
			return usage(command, syntax, false);
		if (option == 1) {
			if (args->count == maximum)
				return usage(command, syntax, true);
			args->value[args->count++] = optarg;
			continue;
		}
		unsigned flag;
		switch (option) {
		case 't':
			flag = OPT_TEMP;
			break;
		case 's':
			flag = OPT_STDIN;
			break;
		case 'c':
			flag = OPT_CONFIG;
			break;
		case 'g':
			flag = OPT_GLOBAL;
			break;
		case 'f':
			flag = OPT_FLAGS;
			break;
		default:
			return usage(command, syntax, true);
		}
		if (!(allowed & flag) || (seen & flag))
			return usage(command, syntax, true);
		seen |= flag;
		args->flags |= flag;
		if (flag == OPT_FLAGS) {
			uint64_t value;
			if (number(optarg, UINT32_MAX, &value))
				return -2;
			args->umount_flags = (unsigned)value;
		}
	}
	while (optind < argc) {
		if (args->count == maximum)
			return usage(command, syntax, true);
		args->value[args->count++] = argv[optind++];
	}
	return args->count < minimum ? usage(command, syntax, true) : 0;
}

static int command_index(const char *name, const char *const names[], size_t count)
{
	for (size_t i = 0; i < count; ++i)
		if (!strcmp(name, names[i]))
			return (int)i;
	return -1;
}

#define COMMAND_INDEX(name, names) command_index(name, names, sizeof(names) / sizeof(*names))

static unsigned char *read_stdin(size_t *length)
{
	unsigned char *data = ksu_read_stream(stdin, SIZE_MAX, length);
	if (!data)
		return NULL;
	if (!ksu_valid_utf8(data, *length)) {
		free(data);
		errno = EILSEQ;
		return NULL;
	}
	return data;
}

static int config_command(int argc, char **argv)
{
	const char *internal = NULL;
	int i = 1;
	if (i < argc && !strcmp(argv[i], "--internal")) {
		if (++i == argc)
			return usage("module config", "[--internal NAME] <COMMAND>", true);
		internal = argv[i++];
	} else if (i < argc && !strncmp(argv[i], "--internal=", 11)) {
		internal = argv[i++] + 11;
	}
	if (i < argc)
		help_command(argc - i + 1, argv + i - 1);
	static const char *const names[] = {"get", "set", "list", "delete", "clear"};
	int cmd = i < argc ? COMMAND_INDEX(argv[i], names) : -1;
	if (cmd < 0)
		return usage("module config", "[--internal NAME] <get|set|list|delete|clear>",
			     i == argc || !is_help(argv[i]));
	static const char *const syntax[] = {"KEY", "KEY [VALUE] [--stdin] [-t]", "", "KEY [-t]",
					     "[-t]"};
	unsigned minimum =
	    cmd == KSU_CONFIG_GET || cmd == KSU_CONFIG_SET || cmd == KSU_CONFIG_DELETE;
	unsigned maximum = cmd == KSU_CONFIG_SET ? 2 : minimum;
	unsigned allowed = cmd == KSU_CONFIG_SET ? OPT_TEMP | OPT_STDIN
			   : cmd == KSU_CONFIG_DELETE || cmd == KSU_CONFIG_CLEAR ? OPT_TEMP
										 : 0;
	struct arguments args;
	int result = parse(argc - i, argv + i, "module config", syntax[cmd], allowed, minimum,
			   maximum, &args);
	if (result)
		return result;
	if (ksu_switch_mnt_ns(1))
		return -1;
	char *owned_id = NULL;
	const char *id = getenv("KSU_MODULE");
	if (internal) {
		if (asprintf(&owned_id, "internal.%s", internal) < 0)
			return -1;
		id = owned_id;
	}
	if (!id) {
		free(owned_id);
		return fail("This command must be run in the context of a module or passed "
			    "--internal <name>");
	}
	if (!ksu_valid_identifier((const unsigned char *)id, strlen(id))) {
		result = fail("Invalid module id: %s", id);
		goto done;
	}
	const unsigned char *key = (const unsigned char *)(args.value[0] ? args.value[0] : "");
	size_t key_length = strlen((const char *)key);
	if (cmd == KSU_CONFIG_SET && !ksu_config_valid_key(key, key_length)) {
		result = fail("Invalid config key: %s", key);
		goto done;
	}
	unsigned char *owned_value = NULL;
	const unsigned char *value = (const unsigned char *)(args.value[1] ? args.value[1] : "");
	size_t value_length = strlen((const char *)value);
	if (cmd == KSU_CONFIG_SET && (!args.value[1] || (args.flags & OPT_STDIN))) {
		owned_value = read_stdin(&value_length);
		if (!owned_value) {
			result = -1;
			goto done;
		}
		value = owned_value;
	}
	char *directory = ksu_join_path(KSU_MODULE_CONFIG_DIR, id);
	if (!directory) {
		result = -1;
	} else if (cmd == KSU_CONFIG_SET && value_length > KSU_CONFIG_MAX_VALUE) {
		result = fail("Config value too long: %zu bytes (max: %u)", value_length,
			      KSU_CONFIG_MAX_VALUE);
	} else {
		result =
		    ksu_config_run((enum ksu_config_command)cmd, directory,
				   !!(args.flags & OPT_TEMP), key, key_length, value, value_length);
		if (result < 0 && errno == ENOENT &&
		    (cmd == KSU_CONFIG_GET || cmd == KSU_CONFIG_DELETE))
			result = fail("Key '%s' not found%s", key,
				      cmd == KSU_CONFIG_DELETE ? " in config" : "");
	}
	free(directory);
	free(owned_value);
done:
	free(owned_id);
	return result;
}

static int module_command(int argc, char **argv, char **message)
{
	help_command(argc, argv);
	static const char *const names[] = {"install", "action",    "list",	      "enable",
					    "disable", "uninstall", "undo-uninstall", "config"};
	int cmd = argc > 1 ? COMMAND_INDEX(argv[1], names) : -1;
	if (cmd < 0)
		return usage("module",
			     "<install|action|list|enable|disable|uninstall|undo-uninstall|config>",
			     argc == 1 || !is_help(argv[1]));
	if (cmd == 7) {
		return config_command(argc - 1, argv + 1);
	}
	struct arguments args;
	unsigned count = cmd != KSU_MODULE_CMD_LIST;
	int result = parse(argc - 1, argv + 1, "module", count ? "<COMMAND> VALUE" : "list", 0,
			   count, count, &args);
	if (result)
		return result;
	if (ksu_switch_mnt_ns(1))
		return -1;
	return ksu_module_run((enum ksu_module_command)cmd, args.value[0], message);
}

static int sepolicy_command(int argc, char **argv, char **message)
{
	help_command(argc, argv);
	static const char *const names[] = {"patch", "apply", "check"};
	int cmd = argc > 1 ? COMMAND_INDEX(argv[1], names) : -1;
	if (cmd < 0)
		return usage("sepolicy", "<patch|apply|check>", argc == 1 || !is_help(argv[1]));
	struct arguments args;
	int result = parse(argc - 1, argv + 1, "sepolicy", "<COMMAND> VALUE", 0, 1, 1, &args);
	if (result)
		return result;
	if (cmd == 1)
		return ksu_sepolicy_file(args.value[0], 0, message);
	return ksu_sepolicy((const unsigned char *)args.value[0], strlen(args.value[0]), cmd == 2,
			    message);
}

static int profile_command(int argc, char **argv, char **message)
{
	help_command(argc, argv);
	static const char *const names[] = {"get-sepolicy", "set-sepolicy",    "get-template",
					    "set-template", "delete-template", "list-templates"};
	int cmd = argc > 1 ? COMMAND_INDEX(argv[1], names) : -1;
	if (cmd < 0)
		return usage("profile",
			     "<get-sepolicy|set-sepolicy|get-template|set-template|delete-template|"
			     "list-templates>",
			     argc == 1 || !is_help(argv[1]));
	struct arguments args;
	unsigned count = cmd == 5 ? 0 : cmd == 1 || cmd == 3 ? 2 : 1;
	int result =
	    parse(argc - 1, argv + 1, "profile", "<COMMAND> [ID] [VALUE]", 0, count, count, &args);
	if (result)
		return result;
	const char *directory = cmd < 2 ? KSU_PROFILE_SELINUX_DIR : KSU_PROFILE_TEMPLATE_DIR;
	if (cmd == 5)
		return ksu_profile_list(directory);
	char *path = ksu_join_path(directory, args.value[0]);
	if (!path)
		return -1;
	if (cmd == 1 || cmd == 3) {
		result = ksu_profile_set(directory, path, args.value[1], strlen(args.value[1]));
		if (!result && cmd == 1)
			result = ksu_sepolicy_file(path, 0, message);
	} else if (cmd == 4) {
		result = ksu_profile_delete(path);
	} else {
		result = ksu_profile_get(path);
	}
	free(path);
	return result;
}

static int feature_command(int argc, char **argv)
{
	help_command(argc, argv);
	static const char *const names[] = {"get", "set", "list", "check", "load", "save"};
	static const enum ksu_feature_command commands[] = {KSU_FEATURE_GET,  KSU_FEATURE_SET,
							    KSU_FEATURE_LIST, KSU_FEATURE_CHECK,
							    KSU_FEATURE_LOAD, KSU_FEATURE_SAVE};
	int cmd = argc > 1 ? COMMAND_INDEX(argv[1], names) : -1;
	if (cmd < 0)
		return usage("feature", "<get|set|list|check|load|save>",
			     argc == 1 || !is_help(argv[1]));
	struct arguments args;
	unsigned count = cmd == 1 ? 2 : cmd == 0 || cmd == 3 ? 1 : 0;
	int result = parse(argc - 1, argv + 1, "feature", "<COMMAND> [ID] [VALUE] [--config]",
			   cmd == 0 ? OPT_CONFIG : 0, count, count, &args);
	if (result)
		return result;
	uint64_t value = 0;
	if (cmd == 1 && number(args.value[1], UINT64_MAX, &value))
		return -2;
	enum ksu_feature_command command =
	    args.flags & OPT_CONFIG ? KSU_FEATURE_GET_CONFIG : commands[cmd];
	result = ksu_feature_modules_run(command, args.value[0], value, KSU_WORKING_DIR,
					 KSU_MODULE_DIR, KSU_MODULE_CONFIG_DIR);
	if (command == KSU_FEATURE_LIST && result < 0) {
		__android_log_print(ANDROID_LOG_ERROR, "KernelSU", "list features: %s",
				    strerror(errno));
		return 0;
	}
	return result;
}

static int mark_command(int argc, char **argv)
{
	help_command(argc, argv);
	static const char *const names[] = {"get", "mark", "unmark", "refresh"};
	static const unsigned operations[] = {KSU_MARK_GET, KSU_MARK_MARK, KSU_MARK_UNMARK,
					      KSU_MARK_REFRESH};
	int cmd = argc > 1 ? COMMAND_INDEX(argv[1], names) : -1;
	if (cmd < 0)
		return usage("debug mark", "<get|mark|unmark|refresh> [PID]",
			     argc == 1 || !is_help(argv[1]));
	struct arguments args;
	int result =
	    parse(argc - 1, argv + 1, "debug mark", "<COMMAND> [PID]", 0, 0, cmd != 3, &args);
	if (result)
		return result;
	int32_t pid = 0;
	if (args.count) {
		char *end;
		errno = 0;
		long long value = strtoll(args.value[0], &end, 10);
		char first = args.value[0][0];
		if (((first < '0' || first > '9') && first != '+' && first != '-') || errno ||
		    *end || end == args.value[0] || value < INT32_MIN || value > INT32_MAX) {
			fail("invalid pid: %s", args.value[0]);
			return -2;
		}
		pid = (int32_t)value;
	}
	return ksu_debug_mark(operations[cmd], pid);
}

static int debug_command(int argc, char **argv)
{
	help_command(argc, argv);
	static const char *const names[] = {"set-manager", "get-sign",	     "su",   "version",
					    "test",	   "extract-binary", "mark", "sulogd",
					    "info",	   "package"};
	int cmd = argc > 1 ? COMMAND_INDEX(argv[1], names) : -1;
	if (cmd < 0)
		return usage("debug",
			     "<set-manager|get-sign|su|version|test|extract-binary|mark|sulogd|"
			     "info|package>",
			     argc == 1 || !is_help(argv[1]));
	if (cmd == 6)
		return mark_command(argc - 1, argv + 1);
	struct arguments args;
	unsigned minimum = cmd == 5 ? 2 : cmd == 1;
	unsigned maximum = cmd == 0 ? 1 : minimum;
	int result = parse(argc - 1, argv + 1, "debug", "<COMMAND> [VALUE]",
			   cmd == 2 ? OPT_GLOBAL : 0, minimum, maximum, &args);
	if (result)
		return result;
	switch (cmd) {
	case 0:
		return ksu_debug_manager(args.count ? args.value[0] : KSU_PACKAGE_NAME);
	case 1: {
		uint32_t size;
		char hash[65];
		if (ksu_apk_signature(args.value[0], &size, hash))
			return -1;
		printf("size: 0x%x, hash: %s\n", size, hash);
		return 0;
	}
	case 2:
		return ksu_debug_su(!!(args.flags & OPT_GLOBAL));
	case 3:
		printf("Kernel Version: %d\n", (int32_t)ksu_get_driver_info()->version);
		return 0;
	case 4:
		return ksu_assets_ensure(KSU_BINARY_DIR, KSU_DAEMON_PATH, false);
	case 5:
		return ksu_asset_extract(args.value[0], args.value[1], false);
	case 7:
		return ksu_start_sulogd();
	case 8: {
		const struct ksu_get_info_cmd *info = ksu_get_driver_info();
		printf("version: %u\nflags: 0x%x\nuapi_version: %u\nfeatures: 0x%" PRIx64
		       "\nbundled: %s\nruntime_mode: built-in\npr_build: %s\n",
		       info->version, info->flags, info->uapi_version, (uint64_t)info->features,
		       info->flags & KSU_GET_INFO_FLAG_BUNDLED ? "true" : "false",
		       info->flags & KSU_GET_INFO_FLAG_PR_BUILD ? "true" : "false");
		return 0;
	}
	case 9:
		puts(KSU_PACKAGE_NAME);
		return 0;
	}
	return -1;
}

static int kernel_command(int argc, char **argv)
{
	help_command(argc, argv);
	if (argc < 2)
		return usage("kernel", "<nuke-ext4-sysfs|umount|notify-module-mounted>", true);
	struct arguments args;
	if (is_help(argv[1]))
		return usage("kernel", "<nuke-ext4-sysfs|umount|notify-module-mounted>", false);
	if (!strcmp(argv[1], "nuke-ext4-sysfs")) {
		int result =
		    parse(argc - 1, argv + 1, "kernel", "nuke-ext4-sysfs MNT", 0, 1, 1, &args);
		if (result)
			return result;
		struct ksu_nuke_ext4_sysfs_cmd cmd = {.arg = (uintptr_t)args.value[0]};
		return ksu_ioctl(KSU_IOCTL_NUKE_EXT4_SYSFS, &cmd) < 0 ? -1 : 0;
	}
	if (!strcmp(argv[1], "notify-module-mounted")) {
		int result =
		    parse(argc - 1, argv + 1, "kernel", "notify-module-mounted", 0, 0, 0, &args);
		if (result)
			return result;
		struct ksu_report_event_cmd cmd = {.event = EVENT_MODULE_MOUNTED};
		(void)ksu_ioctl(KSU_IOCTL_REPORT_EVENT, &cmd);
		return 0;
	}
	if (argc > 2 && !strcmp(argv[1], "umount"))
		help_command(argc - 1, argv + 1);
	static const char *const names[] = {"add", "del", "wipe"};
	static const unsigned modes[] = {KSU_UMOUNT_ADD, KSU_UMOUNT_DEL, KSU_UMOUNT_WIPE};
	int cmd = argc > 2 && !strcmp(argv[1], "umount") ? COMMAND_INDEX(argv[2], names) : -1;
	if (cmd < 0)
		return usage("kernel umount", "<add|del|wipe> [MNT] [-f FLAGS]",
			     argc < 3 || !is_help(argv[2]));
	unsigned count = cmd != 2;
	int result = parse(argc - 2, argv + 2, "kernel umount", "<COMMAND> [MNT] [-f FLAGS]",
			   cmd == 0 ? OPT_FLAGS : 0, count, count, &args);
	if (result)
		return result;
	struct ksu_add_try_umount_cmd request = {
	    .arg = (uintptr_t)args.value[0], .flags = args.umount_flags, .mode = modes[cmd]};
	return ksu_ioctl(KSU_IOCTL_ADD_TRY_UMOUNT, &request) < 0 ? -1 : 0;
}

static int bootloader_command(int argc, char **argv)
{
	struct arguments args;
	int result = parse(argc, argv, "hide-bootloader", "[0|1]", 0, 0, 1, &args);
	if (result)
		return result;
	const char *value = args.value[0];
	if (!value) {
		result = ksu_bootloader_hide_enabled();
		if (result < 0)
			return -1;
		printf("%d\n", result);
		return 0;
	}
	if (!strcmp(value, "0"))
		return ksu_write_file(KSU_BOOTLOADER_HIDE_DISABLED, "", 0);
	if (strcmp(value, "1"))
		return fail("Expected 0 or 1");
	return !unlink(KSU_BOOTLOADER_HIDE_DISABLED) || errno == ENOENT ? 0 : -1;
}

int ksu_cli_main(int argc, char **argv)
{
	ksu_setup_sigsys();
	const char *name = strrchr(argv[0], '/');
	name = name ? name + 1 : argv[0];
	if (!strcmp(name, "su"))
		return ksu_su_main(argc, argv);
	size_t name_length = strlen(name);
	if (name_length >= 9 && !strcmp(name + name_length - 9, "resetprop"))
		return ksu_resetprop_main(argc, argv);
	help_command(argc, argv);
	if (argc > 1 && !strcmp(argv[1], "resetprop"))
		return ksu_resetprop_main(argc - 1, argv + 1);
	if (argc > 2 && !strcmp(argv[1], "initrc"))
		help_command(argc - 1, argv + 1);
	static const char *const names[] = {
	    "module",  "sepolicy",     "profile",	"feature",	  "debug",
	    "kernel",  "post-fs-data", "services",	"boot-completed", "soft-reboot",
	    "install", "sulogd",       "policy-daemon", "initrc",	  "hide-bootloader"};
	if (argc == 2 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-V"))) {
		printf("ksud %s (uapi: %u)\n", KSU_VERSION_NAME, KERNEL_SU_UAPI_VERSION);
		return 0;
	}
	int cmd = argc > 1 ? COMMAND_INDEX(argv[1], names) : -1;
	if (cmd < 0) {
		bool help = argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h") ||
					 !strcmp(argv[1], "help"));
		usage("", "<COMMAND>", !help);
		for (size_t i = 0; i < sizeof(names) / sizeof(*names); ++i)
			if (i != 11 && i != 12)
				fprintf(help ? stdout : stderr, "  %s\n", names[i]);
		return help ? 0 : 2;
	}
	__android_log_print(ANDROID_LOG_INFO, "KernelSU", "command: %s", argv[1]);
	char *message = NULL;
	int result;
	errno = 0;
	switch (cmd) {
	case 0:
		result = module_command(argc - 1, argv + 1, &message);
		break;
	case 1:
		result = sepolicy_command(argc - 1, argv + 1, &message);
		break;
	case 2:
		result = profile_command(argc - 1, argv + 1, &message);
		break;
	case 3:
		result = feature_command(argc - 1, argv + 1);
		break;
	case 4:
		result = debug_command(argc - 1, argv + 1);
		break;
	case 5:
		result = kernel_command(argc - 1, argv + 1);
		break;
	case 14:
		result = bootloader_command(argc - 1, argv + 1);
		break;
	default: {
		if (cmd == 13) {
			if (argc < 3 || strcmp(argv[2], "refresh")) {
				result = usage("initrc", "refresh", argc < 3 || !is_help(argv[2]));
				break;
			}
			--argc;
			++argv;
		}
		struct arguments args;
		result = parse(argc - 1, argv + 1, argv[1], "", 0, 0, 0, &args);
		if (result)
			break;
		switch (cmd) {
		case 6:
			result = ksu_post_fs_data();
			break;
		case 7:
			if ((int32_t)ksu_get_driver_info()->version > 0)
				ksu_services();
			else
				__android_log_print(ANDROID_LOG_INFO, "KernelSU",
						    "KernelSU not available, exiting services");
			break;
		case 8:
			ksu_boot_completed();
			break;
		case 9:
			result = ksu_soft_reboot();
			break;
		case 10:
			result = ksu_install_userspace();
			break;
		case 11:
			result =
			    ksu_sulog_daemon(KSU_WORKING_DIR, KSU_LOG_DIR, KSU_MODULE_CONFIG_DIR);
			break;
		case 12:
			result = ksu_policy_daemon(KSU_POLICY_PATH);
			break;
		case 13:
			result = ksu_regenerate_boot_rc();
			break;
		}
		break;
	}
	}
	if (result == -1 && (message || errno)) {
		const char *error = message ? message : strerror(errno);
		fprintf(stderr, "Error: %s\n", error);
		__android_log_print(ANDROID_LOG_ERROR, "KernelSU", "Error: %s", error);
	}
	free(message);
	return result == -2 ? 2 : result < 0 ? 1 : 0;
}
