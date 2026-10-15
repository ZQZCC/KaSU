#define KSU_SULOG_MAX_QUEUED 256U
#define KSU_SULOG_MAX_PAYLOAD_LEN 2048U
#define KSU_SULOG_MAX_ARGV_LEN 128U

static struct ksu_event_queue sulog_queue;

static void ksu_sulog_fill_task_info(struct ksu_sulog_event *event, __u16 event_type, int retval)
{
	event->version = KSU_SULOG_EVENT_VERSION;
	event->event_type = event_type;
	event->retval = retval;
	event->pid = task_pid_nr(current);
	event->tgid = task_tgid_nr(current);
	event->ppid = task_ppid_nr(current);
	event->uid = current_uid().val;
	event->euid = current_euid().val;
	get_task_comm(event->comm, current);
}

void ksu_sulog_events_init(void)
{
	ksu_event_queue_init(&sulog_queue, KSU_SULOG_MAX_QUEUED, KSU_SULOG_MAX_PAYLOAD_LEN);
}

void ksu_sulog_events_exit(void)
{
	ksu_event_queue_destroy(&sulog_queue);
}

static int ksu_sulog_emit_grant_root(int retval, __u32 uid, __u32 euid, gfp_t gfp)
{
	struct ksu_sulog_event event = {0};

	if (!ksu_sulog_is_enabled())
		return 0;

	ksu_sulog_fill_task_info(&event, KSU_SULOG_EVENT_IOCTL_GRANT_ROOT, retval);
	event.uid = uid;
	event.euid = euid;
	ksu_event_queue_push(&sulog_queue, event.event_type, 0, &event, sizeof(event), gfp);
	return 0;
}

static noinline void do_ksu_sulog_emit_bprm(const char *filename)
{
	if (!is_ksu_domain())
		return;

	if (!current->mm)
		return;

	unsigned long arg_start = current->mm->arg_start;
	unsigned long arg_end = current->mm->arg_end;
	size_t arg_len = arg_end - arg_start;

	if (!arg_len)
		return;

	struct {
		struct ksu_sulog_event event;
		char args[KSU_SULOG_MAX_ARGV_LEN];
	} record = {0};
	char *args = record.args;
	size_t argv_copy_len = min_t(size_t, arg_len, sizeof(record.args));

	if (copy_from_user_retry(args, (void __user *)arg_start, argv_copy_len))
		return;

	args[argv_copy_len - 1] = '\0';
	size_t argv0_len = strnlen(args, argv_copy_len);
	char *buf = args + argv0_len + 1;

flatten:
	if (buf >= args + argv_copy_len - 1)
		goto flatten_done;

	int len = strlen(buf);
	if (!len)
		goto flatten_done;

	*(buf + len) = ' ';
	buf = buf + len + 1;

	if (buf - args < argv_copy_len - argv0_len - 1)
		goto flatten;

flatten_done:
	if (!ksu_sulog_is_enabled())
		return;

	ksu_sulog_fill_task_info(&record.event, KSU_SULOG_EVENT_ROOT_EXECVE, 0);
	record.event.filename_len = argv0_len + 1;
	record.event.argv_len = argv_copy_len - record.event.filename_len;
	ksu_event_queue_push(&sulog_queue, record.event.event_type, 0, &record,
			     sizeof(record.event) + argv_copy_len, GFP_KERNEL);
}

struct ksu_event_queue *ksu_sulog_get_queue(void)
{
	return &sulog_queue;
}
