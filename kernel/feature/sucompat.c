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
