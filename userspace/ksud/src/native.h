// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef KSUD_NATIVE_H
#define KSUD_NATIVE_H

#include <stddef.h>
#include <stdint.h>

#define KSU_CONFIG_MAX_KEY 256
#define KSU_CONFIG_MAX_VALUE (1024 * 1024)
#define KSU_CONFIG_MAX_COUNT 32

struct ksu_string_pair {
	const unsigned char *key;
	size_t key_length;
	const unsigned char *value;
	size_t value_length;
};

struct ksu_string_map {
	unsigned char *data;
	struct ksu_string_pair *entries;
	size_t count;
};

void ksu_strings_free(struct ksu_string_map *map);
int ksu_valid_identifier(const unsigned char *text, size_t length);
int ksu_text_true(const unsigned char *text, size_t length);
int ksu_remove_tree(const char *path);
int ksu_ensure_file(const char *path);
/* Consumes a heap buffer with room for a trailing NUL, including on failure. */
int ksu_properties_parse(unsigned char *data, size_t length, struct ksu_string_map *map);
int ksu_properties_load(const char *path, struct ksu_string_map *map);
struct ksu_zip;
struct ksu_zip *ksu_zip_open(const char *path);
void ksu_zip_close(struct ksu_zip *archive);
int ksu_zip_properties(struct ksu_zip *archive, struct ksu_string_map *properties);
int ksu_zip_size(struct ksu_zip *archive, uint64_t *size);
int ksu_zip_extract(struct ksu_zip *archive, const char *directory);
int ksu_setfilecon(const char *path, const char *context);
int ksu_setsyscon(const char *path);
int ksu_restore_syscon(const char *root, int only_unlabeled);
/* message, when present, is owned by the caller and freed with free(). */
int ksu_install_module(const char *zip, const char *shell, const char *module_directory,
		       const char *update_directory, const char *metamodule_link,
		       const char *library, char **message);
int ksu_policy_restore(const char *path);
int ksu_policy_daemon(const char *path);
int ksu_sepolicy(const unsigned char *data, size_t length, int check_only, char **message);
int ksu_sepolicy_file(const char *path, int check_only, char **message);
int ksu_sulog_daemon(const char *working_directory, const char *log_directory,
		     const char *config_directory);
int ksu_start_sulogd(void);
int ksu_config_load(const char *path, struct ksu_string_map *config);
int ksu_config_save(const char *directory, const char *path, const struct ksu_string_pair *entries,
		    size_t count);
int ksu_config_clear(const char *path);
int ksu_clear_temporary_configs(const char *root);
int ksu_config_valid_key(const unsigned char *key, size_t length);
enum ksu_config_command {
	KSU_CONFIG_GET,
	KSU_CONFIG_SET,
	KSU_CONFIG_LIST,
	KSU_CONFIG_DELETE,
	KSU_CONFIG_CLEAR,
};
int ksu_config_run(enum ksu_config_command command, const char *directory, int temporary,
		   const unsigned char *key, size_t key_length, const unsigned char *value,
		   size_t value_length);

int ksu_profile_get(const char *path);
int ksu_profile_set(const char *directory, const char *path, const void *data, size_t length);
int ksu_profile_delete(const char *path);
int ksu_profile_list(const char *directory);

enum ksu_module_state {
	KSU_MODULE_ENABLE,
	KSU_MODULE_DISABLE,
	KSU_MODULE_REMOVE,
	KSU_MODULE_UNDO_REMOVE,
};
int ksu_module_mark(const char *path, enum ksu_module_state state);

#define KSU_MOD_ENABLED 1u
#define KSU_MOD_UPDATE 2u
#define KSU_MOD_REMOVE 4u
#define KSU_MOD_WEB 8u
#define KSU_MOD_ACTION 16u
#define KSU_MOD_MOUNT 32u
int ksu_module_flags(const char *path, unsigned mask);
int ksu_module_active(const char *path);
int ksu_apply_module_update(const char *updated_path, const char *module_path);
char *ksu_find_metamodule(const char *link, const char *module_directory);
int ksu_is_metamodule(const char *directory);
int ksu_set_metamodule_link(const char *target, const char *link);
int ksu_remove_metamodule_link(const char *link);
enum ksu_meta_command {
	KSU_META_MOUNT,
	KSU_META_UNINSTALL,
};
/* 0 skips an absent/disabled script; 1 supplies its wait status. */
int ksu_exec_metamodule(enum ksu_meta_command command, const char *shell, const char *link,
			const char *module_directory, const char *value, int *status);
/* 0 is safe, 1 disabled only, 2 another unstable state; -1 is an I/O error. */
int ksu_install_safety(const char *link, const char *module_directory,
		       const char *update_directory);
char *ksu_installer_script(const char *link, const char *module_directory, const char *library,
			   int is_metamodule);
enum ksu_module_command {
	KSU_MODULE_CMD_INSTALL,
	KSU_MODULE_CMD_ACTION,
	KSU_MODULE_CMD_LIST,
	KSU_MODULE_CMD_ENABLE,
	KSU_MODULE_CMD_DISABLE,
	KSU_MODULE_CMD_REMOVE,
	KSU_MODULE_CMD_UNDO_REMOVE,
};
int ksu_module_run(enum ksu_module_command command, const char *value, char **message);
int ksu_install_userspace(void);
int ksu_prune_modules(const char *shell, const char *module_directory, const char *metamodule_link,
		      const char *config_directory);
int ksu_list_modules(const char *directory, const char *config_directory);
int ksu_regenerate_rc(const char *common_directory, const char *module_directory,
		      const char *update_directory, const char *preinit_directory);

/* timeout: -1 waits forever, 0 returns after exec; positive values are nanoseconds. */
int ksu_exec_script(const char *shell, const char *path, const char *directory,
		    const char *module_id, char *const overrides[], int64_t timeout);
int ksu_exec_shell(const char *shell, const char *script, int inline_script, const char *directory,
		   const char *module_id, char *const overrides[], int *status);
int ksu_exec_common_scripts(const char *shell, const char *directory, int64_t timeout);
int ksu_exec_module_stage(const char *shell, const char *directory, const char *metamodule_link,
			  const char *stage, int64_t timeout);
int ksu_apply_module_updates(const char *update_directory, const char *module_directory);

struct ksu_managed_feature {
	const unsigned char *module;
	size_t module_length;
	const unsigned char *feature;
	size_t feature_length;
};

enum ksu_feature_command {
	KSU_FEATURE_GET,
	KSU_FEATURE_GET_CONFIG,
	KSU_FEATURE_SET,
	KSU_FEATURE_LIST,
	KSU_FEATURE_CHECK,
	KSU_FEATURE_LOAD,
	KSU_FEATURE_SAVE,
	KSU_FEATURE_INIT,
};
int ksu_feature_run(enum ksu_feature_command command, const char *name, uint64_t value,
		    const char *directory, const struct ksu_managed_feature *managed, size_t count);
int ksu_feature_modules_run(enum ksu_feature_command command, const char *name, uint64_t value,
			    const char *directory, const char *module_directory,
			    const char *config_directory);
void ksu_ensure_sulogd(void);

#endif
