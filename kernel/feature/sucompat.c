static bool ksu_su_compat_enabled __read_mostly = true;

static int su_compat_get(u64 *value)
{
	*value = READ_ONCE(ksu_su_compat_enabled);
	return 0;
}

static int su_compat_set(u64 value)
{
	WRITE_ONCE(ksu_su_compat_enabled, value != 0);
	return 0;
}

static const struct ksu_feature_handler su_compat_handler = {
	.feature_id = KSU_FEATURE_SU_COMPAT,
	.name = "su_compat",
	.get_handler = su_compat_get,
	.set_handler = su_compat_set,
};

void ksu_sucompat_init(void)
{
	ksu_register_feature_handler(&su_compat_handler);
	tiny_sulog_init_heap();
}
