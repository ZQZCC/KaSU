// SPDX-License-Identifier: GPL-3.0-or-later
#include "cli.h"
#include "prop.h"
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int help(bool error)
{
	fputs("Usage: resetprop [OPTIONS] [NAME [VALUE]]\n"
	      "  -n, --skip-svc       Skip property_service\n"
	      "  -p, --persistent     Also use persistent storage\n"
	      "  -P                   Read only persistent storage\n"
	      "  -d, --delete         Delete property\n"
	      "  -v, --verbose        Verbose output\n"
	      "  -w, --wait           Wait for property change\n"
	      "      --timeout SEC    Wait timeout in seconds\n"
	      "  -f, --file FILE      Load properties from file\n"
	      "  -c, --rebuild        Rebuild property areas (--compact)\n"
	      "  -Z                   Show SELinux context\n"
	      "      --force          Force rebuild\n",
	      error ? stderr : stdout);
	return error ? 1 : 0;
}

int ksu_resetprop_main(int argc, char **argv)
{
	static const struct option options[] = {{"skip-svc", no_argument, NULL, 'n'},
						{"persistent", no_argument, NULL, 'p'},
						{"delete", no_argument, NULL, 'd'},
						{"verbose", no_argument, NULL, 'v'},
						{"wait", no_argument, NULL, 'w'},
						{"timeout", required_argument, NULL, 'T'},
						{"file", required_argument, NULL, 'f'},
						{"rebuild", no_argument, NULL, 'c'},
						{"compact", no_argument, NULL, 'c'},
						{"force", no_argument, NULL, 'F'},
						{"help", no_argument, NULL, 'h'},
						{"version", no_argument, NULL, 'V'},
						{NULL, 0, NULL, 0}};
	struct ksu_resetprop args = {0};
	struct timespec timeout;
	unsigned seen = 0;
	optind = 0;
	opterr = 0;
	int option;
	for (;;) {
		int next = optind ? optind : 1;
		/* Clap's trailing hyphen values require the whole short group to be known. */
		if (next < argc && !ksu_option_known(argv[next], "npPdvwcZhVf", options)) {
			optind = next;
			break;
		}
		option = getopt_long(argc, argv, "+:npPdvwcZhVf:", options, NULL);
		if (option == -1)
			break;
		unsigned flag;
		switch (option) {
		case 'h':
			return help(false);
		case 'V':
			puts("resetprop 0.1.0");
			return 0;
		case 'n':
			flag = KSU_PROP_SKIP_SVC;
			break;
		case 'p':
			flag = KSU_PROP_PERSISTENT;
			break;
		case 'P':
			flag = KSU_PROP_PERSIST_ONLY;
			break;
		case 'd':
			flag = KSU_PROP_DELETE;
			break;
		case 'v':
			flag = KSU_PROP_VERBOSE;
			break;
		case 'w':
			flag = KSU_PROP_WAIT;
			break;
		case 'c':
			flag = KSU_PROP_REBUILD;
			break;
		case 'Z':
			flag = KSU_PROP_CONTEXT;
			break;
		case 'F':
			flag = KSU_PROP_FORCE;
			break;
		case 'f':
			if (args.file)
				return help(true);
			args.file = optarg;
			continue;
		case 'T': {
			char *end;
			errno = 0;
			double seconds = strtod(optarg, &end);
			if (args.timeout || !*optarg ||
			    strspn(optarg, "+-0123456789.eE") != strlen(optarg) || *end ||
			    (errno && errno != ERANGE) || !isfinite(seconds) || seconds < 0 ||
			    seconds >= 0x1p63)
				return help(true);
			timeout.tv_sec = (time_t)seconds;
			timeout.tv_nsec =
			    (long)nearbyintl(((long double)seconds - timeout.tv_sec) * 1e9L);
			if (timeout.tv_nsec == 1000000000) {
				++timeout.tv_sec;
				timeout.tv_nsec = 0;
			}
			args.timeout = &timeout;
			continue;
		}
		default:
			return help(true);
		}
		if (seen & flag)
			return help(true);
		seen |= flag;
		args.flags |= flag;
	}
	if (argc - optind > 2)
		return help(true);
	if (optind < argc) {
		args.name = (const unsigned char *)argv[optind++];
		args.name_length = strlen((const char *)args.name);
	}
	if (optind < argc) {
		args.value = (const unsigned char *)argv[optind];
		args.value_length = strlen((const char *)args.value);
	}
	return ksu_resetprop_run(&args);
}
