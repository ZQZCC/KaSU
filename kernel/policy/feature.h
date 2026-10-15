#ifndef __KSU_H_FEATURE
#define __KSU_H_FEATURE

int ksu_get_feature(u32 feature_id, u64 *value, bool *supported);

int ksu_set_feature(u32 feature_id, u64 value);

#endif // __KSU_H_FEATURE
