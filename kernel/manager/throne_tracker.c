static DEFINE_MUTEX(throne_tracker_mutex);

#define SYSTEM_PACKAGES_LIST_PATH "/data/system/packages.list"

struct uid_data {
	struct list_head list;
	u32 uid;
	char package[KSU_MAX_PACKAGE_NAME];
};

static bool is_uid_exist(uid_t uid, char *package, void *data)
{
	struct list_head *list = (struct list_head *)data;
	struct uid_data *np;

	bool exist = false;
	list_for_each_entry (np, list, list) {
		if (np->uid == uid % PER_USER_RANGE && strncmp(np->package, package, KSU_MAX_PACKAGE_NAME) == 0) {
			exist = true;
			break;
		}
	}
	return exist;
}

static void throne_tracker_fn(void)
{
	struct file *fp = filp_open(SYSTEM_PACKAGES_LIST_PATH, O_RDONLY, 0);
	if (IS_ERR(fp)) {
		pr_err("%s: open " SYSTEM_PACKAGES_LIST_PATH " failed: %ld\n", __func__, PTR_ERR(fp));
		return;
	}

	struct list_head uid_list;
	INIT_LIST_HEAD(&uid_list);

	char chr = 0;
	loff_t pos = 0;
	loff_t line_start = 0;
	char buf[KSU_MAX_PACKAGE_NAME];
	for (;;) {
		ssize_t count = kernel_read(fp, &chr, sizeof(chr), &pos);
		if (count != sizeof(chr))
			break;
		if (chr != '\n')
			continue;

		count = kernel_read(fp, buf, sizeof(buf) - 1, &line_start);
		if (count <= 0) {
			break;
		}
		buf[count] = '\0';

		struct uid_data *data = kzalloc(sizeof(struct uid_data), GFP_KERNEL);
		if (!data) {
			filp_close(fp, 0);
			goto out;
		}

		char *tmp = buf;
		const char *delim = " ";
		char *package = strsep(&tmp, delim);
		char *uid = strsep(&tmp, delim);
		if (!uid || !package) {
			kfree(data);
			pr_err("update_uid: package or uid is NULL!\n");
			break;
		}

		u32 res;
		if (kstrtou32(uid, 10, &res)) {
			kfree(data);
			pr_err("update_uid: uid parse err\n");
			break;
		}
		data->uid = res;
		strscpy(data->package, package, sizeof(data->package));
		list_add_tail(&data->list, &uid_list);
		// reset line start
		line_start = pos;
	}
	filp_close(fp, 0);

	struct uid_data *np;
	struct uid_data *n;

	// then prune the allowlist
	ksu_prune_allowlist(is_uid_exist, &uid_list);
out:
	// free uid_list
	list_for_each_entry_safe (np, n, &uid_list, list) {
		list_del(&np->list);
		kfree(np);
	}	
}

static int throne_tracker_thread(void *data)
{
	pr_info("throne_tracker: pid: %d started\n", current->pid);

	guarded_mutex_lock(&throne_tracker_mutex);

test_tmp:
	if (!is_file_existing("/data/system/packages.list.tmp"))
		goto test_list;

	if (IS_ENABLED(CONFIG_KSU_DEBUG))
		pr_info("throne_tracker: rename not finished! retry!\n");

	msleep(20); // yield
	goto test_tmp;

test_list:
	if (is_file_stable(SYSTEM_PACKAGES_LIST_PATH))
		goto start_tt;

	if (IS_ENABLED(CONFIG_KSU_DEBUG))
		pr_info("throne_tracker: rename not finished! retry!\n");

	msleep(20); // yield
	goto test_list;	

start_tt:
	// this in exchange of override creds, we escape this whole thread.
	escape_to_root_forced();
	throne_tracker_fn();

	pr_info("throne_tracker: pid: %d exit!\n", current->pid);
	return 0;
}

void track_throne(void)
{
#ifndef CONFIG_KSU_THRONE_TRACKER_ALWAYS_THREADED
	static void *label = &&first_run;
	goto *label;

first_run:
	if (guarded_mutex_lock(&throne_tracker_mutex))
		throne_tracker_fn();
	
	label = &&threaded;
	return;
threaded:
#endif
	kthread_run(throne_tracker_thread, NULL, "kthread");
}

void __init ksu_throne_tracker_init()
{
	// nothing to do
}

void __exit ksu_throne_tracker_exit()
{
	// nothing to do
}
