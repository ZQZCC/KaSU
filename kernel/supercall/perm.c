bool only_manager(bool manager)
{
	return manager;
}

bool only_root(bool manager)
{
	return current_uid().val == 0;
}

bool manager_or_root(bool manager)
{
	return current_uid().val == 0 || manager;
}

bool always_allow(bool manager)
{
	return true;
}
