static const struct {
	int (*get)(u64 *value);
	int (*set)(u64 value);
} feature_handlers[KSU_FEATURE_MAX] = {
	[KSU_FEATURE_SU_COMPAT] = { su_compat_get, su_compat_set },
	[KSU_FEATURE_SELINUX_HIDE] = { selinux_hide_feature_get, selinux_hide_feature_set },
#ifdef CONFIG_KSU_FEATURE_SULOG
	[KSU_FEATURE_SULOG] = { sulog_feature_get, sulog_feature_set },
#endif
};

static DEFINE_MUTEX(feature_mutex);

int ksu_get_feature(u32 feature_id, u64 *value, bool *supported)
{
	int ret;

	if (feature_id >= KSU_FEATURE_MAX)
		return -EINVAL;
	*supported = feature_handlers[feature_id].get != NULL;
	if (!*supported) {
		*value = 0;
		return 0;
	}
	mutex_lock(&feature_mutex);
	ret = feature_handlers[feature_id].get(value);
	mutex_unlock(&feature_mutex);
	if (ret)
		pr_err("feature: get_handler for %u failed: %d\n", feature_id, ret);
	return ret;
}

int ksu_set_feature(u32 feature_id, u64 value)
{
	int ret;

	if (feature_id >= KSU_FEATURE_MAX)
		return -EINVAL;
	if (!feature_handlers[feature_id].set)
		return -EOPNOTSUPP;
	mutex_lock(&feature_mutex);
	ret = feature_handlers[feature_id].set(value);
	mutex_unlock(&feature_mutex);
	if (ret)
		pr_err("feature: set_handler for %u failed: %d\n", feature_id, ret);
	return ret;
}
