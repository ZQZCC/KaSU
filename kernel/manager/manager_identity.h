#ifndef __KSU_H_MANAGER_IDENTITY
#define __KSU_H_MANAGER_IDENTITY

#define KSU_INVALID_APPID -1
#define KSU_PER_USER_RANGE 100000

void ksu_manager_identity_init(void);
bool is_uid_manager(uid_t uid);
uid_t ksu_get_manager_appid(void);
struct file *ksu_get_manager_directory(uid_t uid);

static inline bool is_manager()
{
	return is_uid_manager(current_uid().val);
}

#endif
