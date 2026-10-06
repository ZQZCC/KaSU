/* Included after allowlist.c: storage transport shares its policy lock. */
static DECLARE_WAIT_QUEUE_HEAD(ksu_policy_wait);
static u64 ksu_policy_generation = 1;
static u64 ksu_policy_saved_generation;
static bool ksu_policy_initialized;
static bool ksu_policy_fd_active;

struct ksu_policy_context {
	struct mutex lock;
	u64 exported_generation;
};

struct ksu_policy_staging {
	DECLARE_HASHTABLE(profiles, ALLOW_LIST_BITS);
};

bool ksu_policy_ready(void)
{
	return smp_load_acquire(&ksu_policy_initialized);
}

void ksu_policy_changed(void)
{
	lockdep_assert_held(&allowlist_mutex);
	WRITE_ONCE(ksu_policy_generation, ksu_policy_generation + 1);
}

void ksu_policy_notify(void)
{
	wake_up_interruptible(&ksu_policy_wait);
}

static bool ksu_policy_authorized(void)
{
	return current_uid().val == 0 && is_ksu_domain();
}

static int ksu_policy_snapshot(struct ksu_policy_context *context, void __user *arg)
{
	struct ksu_policy_snapshot_cmd cmd;
	struct app_profile *profiles = NULL;
	struct perm_data *p;
	u32 capacity, index = 0;
	int bucket, ret = 0;

	if (copy_from_user(&cmd, arg, sizeof(cmd)))
		return -EFAULT;
	capacity = cmd.count;
	mutex_lock(&allowlist_mutex);
	cmd.count = allow_list_count;
	cmd.generation = ksu_policy_generation;
	cmd.flags = ksu_policy_initialized ? KSU_POLICY_INITIALIZED : 0;
	if (!cmd.profiles)
		goto unlock;
	if (!ksu_policy_initialized) {
		ret = -EAGAIN;
		goto unlock;
	}
	if (capacity < cmd.count) {
		ret = -ENOSPC;
		goto unlock;
	}
	if (cmd.count) {
		profiles = kvcalloc(cmd.count, sizeof(*profiles), GFP_KERNEL);
		if (!profiles) {
			ret = -ENOMEM;
			goto unlock;
		}
		hash_for_each (allow_list, bucket, p, list)
			memcpy(&profiles[index++], &p->profile, sizeof(*profiles));
	}
unlock:
	mutex_unlock(&allowlist_mutex);
	if (copy_to_user(arg, &cmd, sizeof(cmd)))
		ret = -EFAULT;
	if (!ret && cmd.profiles) {
		if (cmd.count && copy_to_user(u64_to_user_ptr(cmd.profiles), profiles,
					     cmd.count * sizeof(*profiles)))
			ret = -EFAULT;
		else
			context->exported_generation = cmd.generation;
	}
	kvfree(profiles);
	return ret;
}

static int ksu_policy_restore(void __user *arg)
{
	struct ksu_policy_restore_cmd cmd;
	struct ksu_policy_staging *staging;
	struct app_profile *profiles = NULL;
	struct perm_data *p, *other;
	struct hlist_node *tmp;
	u32 index;
	int bucket, ret = 0;

	if (copy_from_user(&cmd, arg, sizeof(cmd)))
		return -EFAULT;
	if (cmd.count > U16_MAX || cmd.flags & ~KSU_POLICY_RESTORE_SAVED)
		return -EINVAL;
	if (cmd.count) {
		profiles = memdup_user(u64_to_user_ptr(cmd.profiles),
				      cmd.count * sizeof(*profiles));
		if (IS_ERR(profiles))
			return PTR_ERR(profiles);
	}
	staging = kzalloc(sizeof(*staging), GFP_KERNEL);
	if (!staging) {
		kvfree(profiles);
		return -ENOMEM;
	}
	hash_init(staging->profiles);
	/* Allocate and validate the entire batch before publishing any grant. */
	for (index = 0; index < cmd.count; index++) {
		struct app_profile *profile = &profiles[index];
		if (!profile_valid(profile) ||
		    (profile->curr_uid == KSU_APP_PROFILE_PRESERVE_UID &&
		     strcmp(profile->key, "$") != 0)) {
			ret = -EINVAL;
			goto free_staging;
		}
		hash_for_each_possible (staging->profiles, other, list, profile->curr_uid) {
			if (other->profile.curr_uid == profile->curr_uid) {
				ret = -EEXIST;
				goto free_staging;
			}
		}
		p = kzalloc(sizeof(*p), GFP_KERNEL);
		if (!p) {
			ret = -ENOMEM;
			goto free_staging;
		}
		kref_init(&p->ref);
		memcpy(&p->profile, profile, sizeof(*profile));
		hash_add(staging->profiles, &p->list, profile->curr_uid);
	}
	mutex_lock(&allowlist_mutex);
	if (ksu_policy_initialized || allow_list_count) {
		ret = -EALREADY;
		goto unlock;
	}
	hash_for_each_safe (staging->profiles, bucket, tmp, p, list) {
		hash_del(&p->list);
		hash_add_rcu(allow_list, &p->list, p->profile.curr_uid);
		if (p->profile.curr_uid == KSU_APP_PROFILE_PRESERVE_UID)
			default_non_root_profile = p->profile.nrp_config.profile;
	}
	allow_list_count = cmd.count;
	ksu_policy_changed();
	if (cmd.flags & KSU_POLICY_RESTORE_SAVED)
		WRITE_ONCE(ksu_policy_saved_generation, ksu_policy_generation);
	smp_store_release(&ksu_policy_initialized, true);
unlock:
	mutex_unlock(&allowlist_mutex);
	if (!ret)
		ksu_policy_notify();
free_staging:
	hash_for_each_safe (staging->profiles, bucket, tmp, p, list) {
		hash_del(&p->list);
		kfree(p);
	}
	kfree(staging);
	kvfree(profiles);
	return ret;
}

static long ksu_policy_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct ksu_policy_context *context = file->private_data;
	void __user *argp = (void __user *)arg;
	u64 generation;
	int ret;

	if (!ksu_policy_authorized())
		return -EPERM;
	mutex_lock(&context->lock);
	switch (cmd) {
	case KSU_IOCTL_POLICY_SNAPSHOT:
		ret = ksu_policy_snapshot(context, argp);
		break;
	case KSU_IOCTL_POLICY_RESTORE:
		ret = ksu_policy_restore(argp);
		break;
	case KSU_IOCTL_POLICY_ACK:
		ret = -EFAULT;
		if (copy_from_user(&generation, argp, sizeof(generation)))
			break;
		mutex_lock(&allowlist_mutex);
		if (!ksu_policy_initialized || generation != context->exported_generation ||
		    generation < ksu_policy_saved_generation || generation > ksu_policy_generation)
			ret = -ESTALE;
		else {
			WRITE_ONCE(ksu_policy_saved_generation, generation);
			ret = 0;
		}
		mutex_unlock(&allowlist_mutex);
		break;
	default:
		ret = -ENOTTY;
	}
	mutex_unlock(&context->lock);
	return ret;
}

static __poll_t ksu_policy_poll(struct file *file, poll_table *wait)
{
	if (!ksu_policy_authorized())
		return EPOLLERR;
	poll_wait(file, &ksu_policy_wait, wait);
	if (ksu_policy_ready() && READ_ONCE(ksu_policy_generation) !=
				  READ_ONCE(ksu_policy_saved_generation))
		return EPOLLIN | EPOLLRDNORM;
	return 0;
}

static int ksu_policy_release(struct inode *inode, struct file *file)
{
	mutex_lock(&allowlist_mutex);
	ksu_policy_fd_active = false;
	mutex_unlock(&allowlist_mutex);
	kfree(file->private_data);
	return 0;
}

static const struct file_operations ksu_policy_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = ksu_policy_ioctl,
	.poll = ksu_policy_poll,
	.release = ksu_policy_release,
	.llseek = no_llseek,
};

int ksu_install_policy_fd(void)
{
	struct ksu_policy_context *context;
	struct file *file;
	int fd;

	if (!ksu_policy_authorized())
		return -EPERM;
	context = kzalloc(sizeof(*context), GFP_KERNEL);
	if (!context)
		return -ENOMEM;
	mutex_init(&context->lock);
	mutex_lock(&allowlist_mutex);
	if (ksu_policy_fd_active) {
		fd = ksu_policy_initialized ? -EALREADY : -EBUSY;
		goto fail;
	}
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0)
		goto fail;
	file = anon_inode_getfile("[ksu_policy]", &ksu_policy_fops, context, O_RDWR);
	if (IS_ERR(file)) {
		put_unused_fd(fd);
		fd = PTR_ERR(file);
		goto fail;
	}
	ksu_policy_fd_active = true;
	fd_install(fd, file);
	mutex_unlock(&allowlist_mutex);
	return fd;
fail:
	mutex_unlock(&allowlist_mutex);
	kfree(context);
	return fd;
}
