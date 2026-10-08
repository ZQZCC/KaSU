#include <jni.h>

#include <linux/capability.h>
#include <pwd.h>

#include <string.h>

#include "ksu.h"
#include "logging.h"

JNIEXPORT jint JNICALL
Java_ka_su_Natives_getVersion(JNIEnv *env, jobject thiz) {
    int version = get_version();
    if (version > 0) {
        return version;
    }
    // try legacy method as fallback
    return legacy_get_version();
}

JNIEXPORT jint JNICALL
Java_ka_su_Natives_getKernelUAPIVersion(JNIEnv *env, jobject thiz) {
    return get_kernel_uapi_version();
}

JNIEXPORT jint JNICALL
Java_ka_su_Natives_getManagerUAPIVersion(JNIEnv *env, jobject thiz) {
    return get_manager_uapi_version();
}

JNIEXPORT jint JNICALL
Java_ka_su_Natives_getSuperuserCount(JNIEnv *env, jobject thiz) {
    struct ksu_new_get_allow_list_cmd cmd = {
        .count = 0
    };
    bool result = get_allow_list(&cmd);
    return result ? cmd.total_count : 0;
}

JNIEXPORT jboolean JNICALL
Java_ka_su_Natives_isSafeMode(JNIEnv *env, jclass clazz) {
    return is_safe_mode();
}

JNIEXPORT jboolean JNICALL
Java_ka_su_Natives_isManager(JNIEnv *env, jclass clazz) {
    return is_manager();
}

JNIEXPORT jboolean JNICALL
Java_ka_su_Natives_isPrBuild(JNIEnv *env, jclass clazz) {
    return is_pr_build();
}

static void fillIntArray(JNIEnv *env, jobject list, int *data, int count) {
    jclass cls = (*env)->GetObjectClass(env, list);
    jmethodID add = (*env)->GetMethodID(env, cls, "add", "(Ljava/lang/Object;)Z");
    jclass integerCls = (*env)->FindClass(env, "java/lang/Integer");
    jmethodID constructor = (*env)->GetMethodID(env, integerCls, "<init>", "(I)V");
    for (int i = 0; i < count; ++i) {
        jobject integer = (*env)->NewObject(env, integerCls, constructor, data[i]);
        (*env)->CallBooleanMethod(env, list, add, integer);
    }
}

static void addIntToList(JNIEnv *env, jobject list, int ele) {
    jclass cls = (*env)->GetObjectClass(env, list);
    jmethodID add = (*env)->GetMethodID(env, cls, "add", "(Ljava/lang/Object;)Z");
    jclass integerCls = (*env)->FindClass(env, "java/lang/Integer");
    jmethodID constructor = (*env)->GetMethodID(env, integerCls, "<init>", "(I)V");
    jobject integer = (*env)->NewObject(env, integerCls, constructor, ele);
    (*env)->CallBooleanMethod(env, list, add, integer);
}

static uint64_t capListToBits(JNIEnv *env, jobject list) {
    jclass cls = (*env)->GetObjectClass(env, list);
    jmethodID get = (*env)->GetMethodID(env, cls, "get", "(I)Ljava/lang/Object;");
    jmethodID size = (*env)->GetMethodID(env, cls, "size", "()I");
    jint listSize = (*env)->CallIntMethod(env, list, size);
    jclass integerCls = (*env)->FindClass(env, "java/lang/Integer");
    jmethodID intValue = (*env)->GetMethodID(env, integerCls, "intValue", "()I");
    uint64_t result = 0;
    for (int i = 0; i < listSize; ++i) {
        jobject integer = (*env)->CallObjectMethod(env, list, get, i);
        int data = (*env)->CallIntMethod(env, integer, intValue);

        if (cap_valid(data)) {
            result |= (1ULL << data);
        }
    }

    return result;
}

static int getListSize(JNIEnv *env, jobject list) {
    jclass cls = (*env)->GetObjectClass(env, list);
    jmethodID size = (*env)->GetMethodID(env, cls, "size", "()I");
    return (*env)->CallIntMethod(env, list, size);
}

static void fillArrayWithList(JNIEnv *env, jobject list, int *data, int count) {
    jclass cls = (*env)->GetObjectClass(env, list);
    jmethodID get = (*env)->GetMethodID(env, cls, "get", "(I)Ljava/lang/Object;");
    jclass integerCls = (*env)->FindClass(env, "java/lang/Integer");
    jmethodID intValue = (*env)->GetMethodID(env, integerCls, "intValue", "()I");
    for (int i = 0; i < count; ++i) {
        jobject integer = (*env)->CallObjectMethod(env, list, get, i);
        data[i] = (*env)->CallIntMethod(env, integer, intValue);
    }
}

JNIEXPORT jobject JNICALL
Java_ka_su_Natives_getAppProfile(JNIEnv *env, jobject thiz, jstring pkg, jint uid) {
    if ((*env)->GetStringLength(env, pkg) > KSU_MAX_PACKAGE_NAME) {
        return NULL;
    }

    p_key_t key = {0};
    const char *cpkg = (*env)->GetStringUTFChars(env, pkg, NULL);
    strcpy(key, cpkg);
    (*env)->ReleaseStringUTFChars(env, pkg, cpkg);

    struct app_profile profile = {0};
    profile.version = KSU_APP_PROFILE_VER;

    strcpy(profile.key, key);
    profile.curr_uid = uid;

    bool useDefaultProfile = get_app_profile(&profile) != 0;

    jclass cls = (*env)->FindClass(env, "ka/su/Natives$Profile");
    jmethodID constructor = (*env)->GetMethodID(env, cls, "<init>", "()V");
    jobject obj = (*env)->NewObject(env, cls, constructor);
    jfieldID keyField = (*env)->GetFieldID(env, cls, "name", "Ljava/lang/String;");
    jfieldID currentUidField = (*env)->GetFieldID(env, cls, "currentUid", "I");
    jfieldID allowSuField = (*env)->GetFieldID(env, cls, "allowSu", "Z");

    jfieldID rootUseDefaultField = (*env)->GetFieldID(env, cls, "rootUseDefault", "Z");
    jfieldID rootTemplateField = (*env)->GetFieldID(env, cls, "rootTemplate", "Ljava/lang/String;");

    jfieldID uidField = (*env)->GetFieldID(env, cls, "uid", "I");
    jfieldID gidField = (*env)->GetFieldID(env, cls, "gid", "I");
    jfieldID groupsField = (*env)->GetFieldID(env, cls, "groups", "Ljava/util/List;");
    jfieldID capabilitiesField = (*env)->GetFieldID(env, cls, "capabilities", "Ljava/util/List;");
    jfieldID domainField = (*env)->GetFieldID(env, cls, "context", "Ljava/lang/String;");
    jfieldID namespacesField = (*env)->GetFieldID(env, cls, "namespace", "I");
    jfieldID flagsField = (*env)->GetFieldID(env, cls, "flags", "J");

    jfieldID nonRootUseDefaultField = (*env)->GetFieldID(env, cls, "nonRootUseDefault", "Z");
    jfieldID umountModulesField = (*env)->GetFieldID(env, cls, "umountModules", "Z");

    (*env)->SetObjectField(env, obj, keyField, (*env)->NewStringUTF(env, profile.key));
    (*env)->SetIntField(env, obj, currentUidField, profile.curr_uid);

    if (useDefaultProfile) {
        // no profile found, so just use default profile:
        // don't allow root and use default profile!
        LOGD("use default profile for: %s, %d", key, uid);

        // allow_su = false
        // non root use default = true
        (*env)->SetBooleanField(env, obj, allowSuField, false);
        (*env)->SetBooleanField(env, obj, nonRootUseDefaultField, true);

        return obj;
    }

    bool allowSu = profile.allow_su;

    if (allowSu) {
        (*env)->SetBooleanField(env, obj, rootUseDefaultField, (jboolean) profile.rp_config.use_default);
        if (strlen(profile.rp_config.template_name) > 0) {
            (*env)->SetObjectField(env, obj, rootTemplateField,
                    (*env)->NewStringUTF(env, profile.rp_config.template_name));
        }

        (*env)->SetIntField(env, obj, uidField, profile.rp_config.profile.uid);
        (*env)->SetIntField(env, obj, gidField, profile.rp_config.profile.gid);

        jobject groupList = (*env)->GetObjectField(env, obj, groupsField);
        int groupCount = profile.rp_config.profile.groups_count;
        if (groupCount > KSU_MAX_GROUPS) {
            LOGD("kernel group count too large: %d???", groupCount);
            groupCount = KSU_MAX_GROUPS;
        }
        fillIntArray(env, groupList, profile.rp_config.profile.groups, groupCount);

        jobject capList = (*env)->GetObjectField(env, obj, capabilitiesField);
        for (int i = 0; i <= CAP_LAST_CAP; i++) {
            if (profile.rp_config.profile.capabilities.effective & (1ULL << i)) {
                addIntToList(env, capList, i);
            }
        }

        (*env)->SetObjectField(env, obj, domainField,
                (*env)->NewStringUTF(env, profile.rp_config.profile.selinux_domain));
        (*env)->SetIntField(env, obj, namespacesField, profile.rp_config.profile.namespaces);
        (*env)->SetBooleanField(env, obj, allowSuField, profile.allow_su);
        (*env)->SetLongField(env, obj, flagsField, (jlong) profile.rp_config.profile.flags);
    } else {
        (*env)->SetBooleanField(env, obj, nonRootUseDefaultField,
                (jboolean) profile.nrp_config.use_default);
        (*env)->SetBooleanField(env, obj, umountModulesField, profile.nrp_config.profile.umount_modules);
    }

    return obj;
}

JNIEXPORT jboolean JNICALL
Java_ka_su_Natives_setAppProfile(JNIEnv *env, jobject clazz, jobject profile) {
    jclass cls = (*env)->FindClass(env, "ka/su/Natives$Profile");

    jfieldID keyField = (*env)->GetFieldID(env, cls, "name", "Ljava/lang/String;");
    jfieldID currentUidField = (*env)->GetFieldID(env, cls, "currentUid", "I");
    jfieldID allowSuField = (*env)->GetFieldID(env, cls, "allowSu", "Z");

    jfieldID rootUseDefaultField = (*env)->GetFieldID(env, cls, "rootUseDefault", "Z");
    jfieldID rootTemplateField = (*env)->GetFieldID(env, cls, "rootTemplate", "Ljava/lang/String;");

    jfieldID uidField = (*env)->GetFieldID(env, cls, "uid", "I");
    jfieldID gidField = (*env)->GetFieldID(env, cls, "gid", "I");
    jfieldID groupsField = (*env)->GetFieldID(env, cls, "groups", "Ljava/util/List;");
    jfieldID capabilitiesField = (*env)->GetFieldID(env, cls, "capabilities", "Ljava/util/List;");
    jfieldID domainField = (*env)->GetFieldID(env, cls, "context", "Ljava/lang/String;");
    jfieldID namespacesField = (*env)->GetFieldID(env, cls, "namespace", "I");
    jfieldID flagsField = (*env)->GetFieldID(env, cls, "flags", "J");

    jfieldID nonRootUseDefaultField = (*env)->GetFieldID(env, cls, "nonRootUseDefault", "Z");
    jfieldID umountModulesField = (*env)->GetFieldID(env, cls, "umountModules", "Z");

    jobject key = (*env)->GetObjectField(env, profile, keyField);
    if (!key) {
        return false;
    }
    if ((*env)->GetStringLength(env, (jstring) key) > KSU_MAX_PACKAGE_NAME) {
        return false;
    }

    const char *cpkg = (*env)->GetStringUTFChars(env, (jstring) key, NULL);
    p_key_t p_key = {0};
    strcpy(p_key, cpkg);
    (*env)->ReleaseStringUTFChars(env, (jstring) key, cpkg);

    jint currentUid = (*env)->GetIntField(env, profile, currentUidField);

    jint uid = (*env)->GetIntField(env, profile, uidField);
    jint gid = (*env)->GetIntField(env, profile, gidField);
    jobject groups = (*env)->GetObjectField(env, profile, groupsField);
    jobject capabilities = (*env)->GetObjectField(env, profile, capabilitiesField);
    jobject domain = (*env)->GetObjectField(env, profile, domainField);
    jboolean allowSu = (*env)->GetBooleanField(env, profile, allowSuField);
    jboolean umountModules = (*env)->GetBooleanField(env, profile, umountModulesField);

    struct app_profile p = {0};
    p.version = KSU_APP_PROFILE_VER;

    strcpy(p.key, p_key);
    p.allow_su = allowSu;
    p.curr_uid = currentUid;

    if (allowSu) {
        p.rp_config.use_default = (*env)->GetBooleanField(env, profile, rootUseDefaultField);
        jobject templateName = (*env)->GetObjectField(env, profile, rootTemplateField);
        if (templateName) {
            const char *ctemplateName = (*env)->GetStringUTFChars(env, (jstring) templateName, NULL);
            strcpy(p.rp_config.template_name, ctemplateName);
            (*env)->ReleaseStringUTFChars(env, (jstring) templateName, ctemplateName);
        }

        p.rp_config.profile.uid = uid;
        p.rp_config.profile.gid = gid;

        int groups_count = getListSize(env, groups);
        if (groups_count > KSU_MAX_GROUPS) {
            LOGD("groups count too large: %d", groups_count);
            return false;
        }
        p.rp_config.profile.groups_count = groups_count;
        fillArrayWithList(env, groups, p.rp_config.profile.groups, groups_count);

        p.rp_config.profile.capabilities.effective = capListToBits(env, capabilities);

        const char *cdomain = (*env)->GetStringUTFChars(env, (jstring) domain, NULL);
        strcpy(p.rp_config.profile.selinux_domain, cdomain);
        (*env)->ReleaseStringUTFChars(env, (jstring) domain, cdomain);

        p.rp_config.profile.namespaces = (*env)->GetIntField(env, profile, namespacesField);

        p.rp_config.profile.flags = (*env)->GetLongField(env, profile, flagsField);
    } else {
        p.nrp_config.use_default = (*env)->GetBooleanField(env, profile, nonRootUseDefaultField);
        p.nrp_config.profile.umount_modules = umountModules;
    }

    return set_app_profile(&p);
}
JNIEXPORT jboolean JNICALL
Java_ka_su_Natives_uidShouldUmount(JNIEnv *env, jobject thiz, jint uid) {
    return uid_should_umount(uid);
}
JNIEXPORT jboolean JNICALL
Java_ka_su_Natives_isSuEnabled(JNIEnv *env, jobject thiz) {
    return is_su_enabled();
}
JNIEXPORT jboolean JNICALL
Java_ka_su_Natives_setSuEnabled(JNIEnv *env, jobject thiz, jboolean enabled) {
    return set_su_enabled(enabled);
}

JNIEXPORT jboolean JNICALL
Java_ka_su_Natives_isKernelUmountEnabled(JNIEnv *env, jobject thiz) {
    return is_kernel_umount_enabled();
}

JNIEXPORT jboolean JNICALL
Java_ka_su_Natives_isKernelUmountSupported(JNIEnv *env, jobject thiz) {
    return is_kernel_umount_supported();
}

JNIEXPORT jboolean JNICALL
Java_ka_su_Natives_setKernelUmountEnabled(JNIEnv *env, jobject thiz, jboolean enabled) {
    return set_kernel_umount_enabled(enabled);
}

JNIEXPORT jboolean JNICALL
Java_ka_su_Natives_isSelinuxHideEnabled(JNIEnv *env, jobject thiz) {
    return is_selinux_hide_enabled();
}

JNIEXPORT jint JNICALL
Java_ka_su_Natives_setSelinuxHideEnabled(JNIEnv *env, jobject thiz, jboolean enabled) {
    return set_selinux_hide_enabled(enabled);
}

JNIEXPORT jstring JNICALL
Java_ka_su_Natives_getUserName(JNIEnv *env, jobject thiz, jint uid) {
    struct passwd *pw = getpwuid((uid_t) uid);
    if (pw && pw->pw_name && pw->pw_name[0] != '\0') {
        return (*env)->NewStringUTF(env, pw->pw_name);
    }
    return NULL;
}
