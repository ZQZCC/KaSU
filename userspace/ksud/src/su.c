// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "driver.h"
#include "process.h"
#include "uapi/supercall.h"

#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <grp.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define KSU_BIN "/data/adb/ksu/bin"
#define KSU_RC "/data/adb/ksu/.ksurc"
#define NO_NEW_PRIVS 256

#define SU_OPTION(name, argument, code)                                                            \
	{name, argument, NULL, code},                                                              \
	{                                                                                          \
		(const char[]){code, 0}, argument, NULL, code                                      \
	}

/* Single-letter long aliases are accepted, but names must never be abbreviated. */
static const struct option su_options[] = {
    SU_OPTION("command", required_argument, 'c'),
    SU_OPTION("help", no_argument, 'h'),
    SU_OPTION("login", no_argument, 'l'),
    SU_OPTION("preserve-environment", no_argument, 'p'),
    SU_OPTION("shell", required_argument, 's'),
    SU_OPTION("version", no_argument, 'v'),
    {"V", no_argument, NULL, 'V'},
    SU_OPTION("mount-master", no_argument, 'M'),
    SU_OPTION("group", required_argument, 'g'),
    SU_OPTION("supp-group", required_argument, 'G'),
    SU_OPTION("no-wrapper", no_argument, 'W'),
    {"ksu-no-new-privs", no_argument, NULL, NO_NEW_PRIVS},
    SU_OPTION("context", required_argument, 'Z'),
    {0},
};
#undef SU_OPTION

static void usage(const char *program)
{
	printf("KernelSU\n\nUsage: %s [options] [-] [user [argument...]]\n\n", program);
	puts("Options:\n"
	     "    -c, --command COMMAND     pass COMMAND to the invoked shell\n"
	     "    -h, --help                display this help message and exit\n"
	     "    -l, --login               pretend the shell to be a login shell\n"
	     "    -p, --preserve-environment preserve the entire environment\n"
	     "    -s, --shell SHELL         use SHELL instead of /system/bin/sh\n"
	     "    -v, --version             display version number and exit\n"
	     "    -V                        display version code and exit\n"
	     "    -M, --mount-master        force run in the global mount namespace\n"
	     "    -g, --group GROUP         specify the primary group\n"
	     "    -G, --supp-group GROUP    supplementary group (repeatable); first is\n"
	     "                              primary when -g is not specified\n"
	     "    -W, --no-wrapper          don't use ksu fd wrapper\n"
	     "        --ksu-no-new-privs    prevent KernelSU privilege re-escalation\n"
	     "    -Z, --context CONTEXT     specify the SELinux context");
}

static bool excludes_direct_program(const char *arg)
{
	return !strncmp(arg, "-g", 2) || !strncmp(arg, "-G", 2) || !strncmp(arg, "-s", 2) ||
	       !strncmp(arg, "-Z", 2) || !strcmp(arg, "--group") || !strcmp(arg, "--supp-group=") ||
	       !strcmp(arg, "--shell=") || !strcmp(arg, "--context=");
}

static char *join_command(int argc, char **argv)
{
	size_t length = 1;
	for (int i = 0; i < argc; ++i)
		length += strlen(argv[i]) + (i != 0);
	char *command = malloc(length);
	if (!command)
		return NULL;
	char *cursor = command;
	for (int i = 0; i < argc; ++i) {
		if (i)
			*cursor++ = ' ';
		size_t size = strlen(argv[i]);
		memcpy(cursor, argv[i], size);
		cursor += size;
	}
	*cursor = '\0';
	return command;
}

static int parse_id(const char *value, uint32_t *id)
{
	const char *digits = value + (*value == '+');
	if (!*digits || strspn(digits, "0123456789") != strlen(digits))
		return -1;
	char *end;
	errno = 0;
	unsigned long number = strtoul(value, &end, 10);
	if (errno || *end || number > UINT32_MAX)
		return -1;
	*id = (uint32_t)number;
	return 0;
}

static int add_path(void)
{
	const char *old = getenv("PATH");
	char *path;
	if (asprintf(&path, "%s%s%s", old ? old : "", old ? ":" : "", KSU_BIN) < 0)
		return -1;
	int result = setenv("PATH", path, 1);
	free(path);
	return result;
}

static void wrap_tty(int fd)
{
	if (!isatty(fd) && errno != EACCES)
		return;
	struct ksu_get_wrapper_fd_cmd cmd = {.fd = (uint32_t)fd, .flags = 0};
	int wrapped = ksu_ioctl(KSU_IOCTL_GET_WRAPPER_FD, &cmd);
	if (wrapped < 0)
		goto error;
	if (!isatty(wrapped)) {
		close(wrapped);
		return;
	}
	int result = dup2(wrapped, fd);
	int error = errno;
	close(wrapped);
	if (result >= 0)
		return;
	errno = error;
error:
	__android_log_print(ANDROID_LOG_ERROR, "KernelSU", "wrap tty %d: %s", fd, strerror(errno));
}

static int set_context(const char *context)
{
	int fd =
	    open("/proc/thread-self/attr/current", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
	if (fd < 0)
		return -1;
	size_t remaining = strlen(context);
	while (remaining) {
		ssize_t size = write(fd, context, remaining);
		if (size < 0 && errno == EINTR)
			continue;
		if (size <= 0) {
			int error = size < 0 ? errno : EIO;
			close(fd);
			errno = error;
			return -1;
		}
		context += size;
		remaining -= (size_t)size;
	}
	close(fd);
	return 0;
}

int ksu_su_main(int argc, char **argv)
{
	if (ksu_claim_driver_fd()) {
		perror("claim inherited KernelSU driver fd");
		return 1;
	}
	char **storage = malloc((size_t)(argc + 1) * 3 * sizeof(char *));
	if (!storage) {
		perror("su arguments");
		return 1;
	}
	char **args = storage, **free_args = args + argc + 1, **group_args = free_args + argc + 1;
	char *command = NULL, *joined = NULL;
	const char *program = argv[0], *shell = "/system/bin/sh", *executable = NULL;
	const char *group = NULL, *context = NULL;
	int first_c = argc, first_program = argc, parse_argc = argc;
	int free_count = 0, group_count = 0, result = 1;
	enum { option_count = sizeof(su_options) / sizeof(su_options[0]) - 1 };
	unsigned counts[option_count] = {0};
	bool login = false, preserve = false, mount_master = false, wrapper = true,
	     no_new_privs = false;
	bool help = false, version = false, version_code = false;
	gid_t *groups = NULL;
	char **exec_args = NULL;

	/* Preserve the existing su preprocessing before getopt sees the arguments. */
	for (int i = 1; i < argc; ++i)
		if (!strcmp(argv[i], "-c")) {
			first_c = i;
			break;
		}
	for (int i = 0; i + 2 < argc; ++i)
		if (argv[i + 1][0] != '-' && argv[i + 2][0] != '-' &&
		    !excludes_direct_program(argv[i])) {
			first_program = i + 1;
			break;
		}
	memcpy(args, argv, (size_t)argc * sizeof(char *));
	if (first_program < first_c) {
		executable = argv[first_program + 1];
		exec_args = argv + first_program + 2;
		parse_argc = first_program + 1;
	} else if (first_c < first_program) {
		parse_argc = first_c + 1;
		if (first_c + 1 < argc) {
			joined = join_command(argc - first_c - 1, argv + first_c + 1);
			if (!joined)
				goto system_error;
			args[parse_argc++] = joined;
		}
	}
	args[parse_argc] = NULL;
	for (int i = 0; i < parse_argc; ++i)
		if (!strcmp(args[i], "-mm"))
			args[i] = "-M";
		else if (!strcmp(args[i], "-cn"))
			args[i] = "-z";

	opterr = 0;
	optind = 1;
	int option;
	while (optind < parse_argc) {
		const char *next = args[optind];
		if (!strncmp(next, "--", 2) && next[2]) {
			size_t length = strcspn(next + 2, "=");
			size_t i = 0;
			for (; i < option_count; ++i)
				if (strlen(su_options[i].name) == length &&
				    !strncmp(su_options[i].name, next + 2, length))
					break;
			if (i == option_count) {
				printf("Unrecognized option: '%.*s'\n", (int)length, next + 2);
				goto parse_error;
			}
		}
		option = getopt_long(parse_argc, args, "-:c:hlps:vVMg:G:WZ:", su_options, NULL);
		if (option == -1)
			break;
		if (option == 1) {
			free_args[free_count++] = optarg;
			continue;
		}
		if (option == '?' || option == ':') {
			printf("Invalid option or missing argument: %s\n", next);
			goto parse_error;
		}
		for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); ++i)
			if (su_options[i].val == option) {
				++counts[i];
				break;
			}
		switch (option) {
		case 'c':
			command = optarg;
			break;
		case 'h':
			help = true;
			break;
		case 'l':
			login = true;
			break;
		case 'p':
			preserve = true;
			break;
		case 's':
			shell = optarg;
			break;
		case 'v':
			version = true;
			break;
		case 'V':
			version_code = true;
			break;
		case 'M':
			mount_master = true;
			break;
		case 'g':
			group = optarg;
			break;
		case 'G':
			group_args[group_count++] = optarg;
			break;
		case 'W':
			wrapper = false;
			break;
		case 'Z':
			context = optarg;
			break;
		case NO_NEW_PRIVS:
			no_new_privs = true;
			break;
		}
	}
	while (optind < parse_argc)
		free_args[free_count++] = args[optind++];
	for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); ++i)
		if (counts[i] > 1 && su_options[i].val != 'G') {
			printf("Option '%s' given more than once\n", su_options[i].name);
			goto parse_error;
		}
	if (help) {
		usage(program);
		result = 0;
		goto done;
	}
	if (version || version_code) {
		if (version)
			puts(KSU_VERSION_NAME ":KernelSU");
		else
			printf("%u\n", (unsigned)KSU_VERSION_CODE);
		result = 0;
		goto done;
	}
	if (group_count) {
		groups = malloc((size_t)group_count * sizeof(gid_t));
		if (!groups)
			goto system_error;
	}
	uint32_t gid = 0;
	for (int i = 0; i < group_count; ++i) {
		uint32_t value;
		if (parse_id(group_args[i], &value)) {
			fprintf(stderr, "Invalid GID: %s\n", group_args[i]);
			goto done;
		}
		groups[i] = value;
	}
	if (group && parse_id(group, &gid)) {
		fprintf(stderr, "Invalid GID: %s\n", group);
		goto done;
	}
	int user_index = 0;
	if (free_count && !strcmp(free_args[0], "-")) {
		login = true;
		++user_index;
	}
	bool identity_requested = user_index < free_count || group || group_count;
	uint32_t uid = getuid();
	if (user_index < free_count) {
		struct passwd *pw = getpwnam(free_args[user_index]);
		if (pw)
			uid = pw->pw_uid;
		else if (parse_id(free_args[user_index], &uid)) {
			fprintf(stderr, "Unknown user: %s\n", free_args[user_index]);
			goto done;
		}
	}
	if (!group)
		gid = group_count ? groups[0] : uid;
	if (!executable)
		executable = shell;
	if (!preserve) {
		struct passwd *pw = getpwuid(uid);
		if (pw && (setenv("HOME", pw->pw_dir, 1) || setenv("USER", pw->pw_name, 1) ||
			   setenv("LOGNAME", pw->pw_name, 1) || setenv("SHELL", shell, 1)))
			goto system_error;
	}
	if (add_path())
		goto system_error;
	if (!access(KSU_RC, F_OK) && !getenv("ENV") && setenv("ENV", KSU_RC, 1))
		goto system_error;
	if (no_new_privs) {
		int ret = ksu_ioctl(KSU_IOCTL_DISABLE_ESCAPE_TO_ROOT, NULL);
		if (ret < 0)
			goto system_error;
		if (ret) {
			fprintf(stderr, "unexpected result: %d\n", ret);
			goto done;
		}
	}
	umask(022);
	ksu_switch_cgroups();
	if (mount_master)
		(void)ksu_switch_mnt_ns(1);
	if (wrapper)
		for (int i = 0; i < 3; ++i)
			wrap_tty(i);
	if (identity_requested &&
	    (syscall(SYS_setgroups, group_count, groups) || syscall(SYS_setresgid, gid, gid, gid) ||
	     syscall(SYS_setresuid, uid, uid, uid)))
		goto system_error;
	if (context && set_context(context))
		goto system_error;
	char *command_args[] = {login ? "-" : (char *)executable, "-c", command, NULL};
	if (exec_args) {
		/* The pointer preceding direct arguments belongs to the borrowed argv. */
		exec_args[-1] = command_args[0];
		execvp(executable, exec_args - 1);
	} else {
		if (!command)
			command_args[1] = NULL;
		execvp(executable, command_args);
	}
system_error:
	perror("su");
	goto done;
parse_error:
	usage(program);
	result = 255;
done:
	free(groups);
	free(joined);
	free(storage);
	return result;
}

int ksu_debug_su(int global_mnt)
{
	if (ksu_ioctl(KSU_IOCTL_GRANT_ROOT, NULL) < 0 || add_path())
		return -1;
	if (global_mnt)
		(void)ksu_switch_mnt_ns(1);
	char *args[] = {"sh", NULL};
	return execvp("sh", args);
}
