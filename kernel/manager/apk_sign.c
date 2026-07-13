#ifndef KSU_PACKAGE_NAME
#define KSU_PACKAGE_NAME "ka.super"
#endif

// /data/app/XXXXX/<PACKAGE_NAME>-YYY, which contains base.apk
int get_pkg_from_apk_dir_path(char *pkg, const char *path)
{
	int len = strlen(path);
	if (len >= KSU_MAX_PACKAGE_NAME || len < 1)
		return -1;

	const char *last_slash = strrchr(path, '/');
	if (!last_slash)
		return -1;

	const char *last_hyphen = strchr(last_slash, '-');
	if (!last_hyphen)
		return -1;

	int pkg_len = last_hyphen - last_slash - 1;
	if (pkg_len >= KSU_MAX_PACKAGE_NAME || pkg_len <= 0)
		return -1;

	// Copying the package name
	memcpy(pkg, last_slash + 1, pkg_len);
	pkg[pkg_len] = '\0';

	return 0;
}

bool is_manager_apk(char *path)
{
	const size_t package_len = sizeof(KSU_PACKAGE_NAME) - 1;
	const char *end = strrchr(path, '/');
	const char *name = end;

	if (!end || strlen(path) >= KSU_MAX_PACKAGE_NAME)
		return false;

	while (name > path && name[-1] != '/')
		name--;
	if (name == path || (size_t)(end - name) <= package_len)
		return false;

	return name[package_len] == '-' && !memcmp(name, KSU_PACKAGE_NAME, package_len);
}
