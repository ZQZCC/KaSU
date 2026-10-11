bool only_manager(const struct file *filp)
{
	return ksu_is_manager_fd(filp);
}

bool only_root(const struct file *filp)
{
	return current_uid().val == 0;
}

bool manager_or_root(const struct file *filp)
{
	return current_uid().val == 0 || ksu_is_manager_fd(filp);
}

bool always_allow(const struct file *filp)
{
	return true;
}
