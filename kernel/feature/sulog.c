static bool ksu_sulog_enabled __read_mostly = false;

static int sulog_feature_get(u64 *value)
{
	*value = ksu_sulog_enabled ? 1 : 0;
	return 0;
}

static int sulog_feature_set(u64 value)
{
	bool enable = value != 0;

	ksu_sulog_enabled = enable;
	if (ksu_sulog_enabled)
		ksu_sulog_branch_enable();
	else
		ksu_sulog_branch_disable();

	pr_info("sulog: set to %d\n", enable);
	return 0;
}

bool ksu_sulog_is_enabled(void)
{
	return ksu_sulog_enabled;
}
