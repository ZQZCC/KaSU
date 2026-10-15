#define KSU_SULOG_MAX_QUEUED 256U
#define KSU_SULOG_MAX_ARGV_LEN 128U
#define KSU_EVENT_RECORD_FLAG_INTERNAL (1U << 0)
#define KSU_EVENT_QUEUE_TYPE_DROPPED ((__u16)0xFFFF)

struct ksu_event_record_hdr {
	__u16 type;
	__u16 flags;
	__u32 len;
	__u64 seq;
	__u64 ts_ns;
};

struct ksu_event_queue_dropped_info {
	__u64 dropped;
	__u64 first_seq;
	__u64 last_seq;
};

struct ksu_sulog_record {
	struct ksu_event_record_hdr hdr;
	u8 payload[sizeof(struct ksu_sulog_event) + KSU_SULOG_MAX_ARGV_LEN];
};

static DEFINE_SPINLOCK(sulog_queue_lock);
static DEFINE_MUTEX(sulog_read_lock);
static DECLARE_WAIT_QUEUE_HEAD(sulog_read_wait);
static struct {
	struct ksu_sulog_record records[KSU_SULOG_MAX_QUEUED];
	u32 head;
	u32 queued;
	u64 next_seq;
	struct ksu_event_queue_dropped_info pending;
	struct ksu_event_queue_dropped_info inflight;
} sulog_queue = { .next_seq = 1 };

static bool ksu_sulog_has_data_locked(void)
{
	return sulog_queue.queued || sulog_queue.pending.dropped || sulog_queue.inflight.dropped;
}

static bool ksu_sulog_has_data(void)
{
	unsigned long flags;
	bool ready;

	spin_lock_irqsave(&sulog_queue_lock, flags);
	ready = ksu_sulog_has_data_locked();
	spin_unlock_irqrestore(&sulog_queue_lock, flags);
	return ready;
}

static void ksu_sulog_queue_push(const void *payload, size_t len)
{
	const struct ksu_sulog_event *event = payload;
	unsigned long flags;
	u64 seq;

	spin_lock_irqsave(&sulog_queue_lock, flags);
	seq = sulog_queue.next_seq++;
	if (sulog_queue.queued == KSU_SULOG_MAX_QUEUED) {
		if (!sulog_queue.pending.dropped)
			sulog_queue.pending.first_seq = seq;
		sulog_queue.pending.dropped++;
		sulog_queue.pending.last_seq = seq;
	} else {
		struct ksu_sulog_record *record = &sulog_queue.records[
			(sulog_queue.head + sulog_queue.queued) % KSU_SULOG_MAX_QUEUED];

		memcpy(record->payload, payload, len);
		record->hdr = (struct ksu_event_record_hdr){
			.type = event->event_type,
			.len = len,
			.seq = seq,
			.ts_ns = ktime_get_ns(),
		};
		sulog_queue.queued++;
	}
	spin_unlock_irqrestore(&sulog_queue_lock, flags);
	wake_up_interruptible_poll(&sulog_read_wait, EPOLLIN | EPOLLRDNORM);
}

static ssize_t ksu_sulog_read_drop(char __user *buf, size_t count)
{
	struct {
		struct ksu_event_record_hdr hdr;
		struct ksu_event_queue_dropped_info info;
	} record;
	unsigned long flags;

	spin_lock_irqsave(&sulog_queue_lock, flags);
	if (!sulog_queue.pending.dropped) {
		spin_unlock_irqrestore(&sulog_queue_lock, flags);
		return 0;
	}
	if (count < sizeof(record)) {
		spin_unlock_irqrestore(&sulog_queue_lock, flags);
		return -EMSGSIZE;
	}
	record.info = sulog_queue.pending;
	record.hdr = (struct ksu_event_record_hdr){
		.type = KSU_EVENT_QUEUE_TYPE_DROPPED,
		.flags = KSU_EVENT_RECORD_FLAG_INTERNAL,
		.len = sizeof(record.info),
		.seq = record.info.first_seq,
		.ts_ns = ktime_get_ns(),
	};
	sulog_queue.inflight = record.info;
	sulog_queue.pending = (struct ksu_event_queue_dropped_info){0};
	spin_unlock_irqrestore(&sulog_queue_lock, flags);

	if (copy_to_user(buf, &record, sizeof(record))) {
		spin_lock_irqsave(&sulog_queue_lock, flags);
		if (!sulog_queue.pending.dropped) {
			sulog_queue.pending = record.info;
		} else {
			sulog_queue.pending.dropped += record.info.dropped;
			sulog_queue.pending.first_seq = record.info.first_seq;
		}
		sulog_queue.inflight = (struct ksu_event_queue_dropped_info){0};
		spin_unlock_irqrestore(&sulog_queue_lock, flags);
		return -EFAULT;
	}
	spin_lock_irqsave(&sulog_queue_lock, flags);
	sulog_queue.inflight = (struct ksu_event_queue_dropped_info){0};
	spin_unlock_irqrestore(&sulog_queue_lock, flags);
	return sizeof(record);
}

static ssize_t ksu_sulog_read_record(char __user *buf, size_t count)
{
	struct ksu_sulog_record *record;
	size_t size;
	unsigned long flags;

	spin_lock_irqsave(&sulog_queue_lock, flags);
	if (!sulog_queue.queued) {
		spin_unlock_irqrestore(&sulog_queue_lock, flags);
		return 0;
	}
	record = &sulog_queue.records[sulog_queue.head];
	size = sizeof(record->hdr) + record->hdr.len;
	if (count < size) {
		spin_unlock_irqrestore(&sulog_queue_lock, flags);
		return -EMSGSIZE;
	}
	spin_unlock_irqrestore(&sulog_queue_lock, flags);

	/* Full queues drop new records, so the head stays valid until the reader consumes it. */
	if (copy_to_user(buf, record, size))
		return -EFAULT;
	spin_lock_irqsave(&sulog_queue_lock, flags);
	sulog_queue.head = (sulog_queue.head + 1) % KSU_SULOG_MAX_QUEUED;
	sulog_queue.queued--;
	spin_unlock_irqrestore(&sulog_queue_lock, flags);
	return size;
}

static ssize_t ksu_sulog_queue_read(char __user *buf, size_t count, int file_flags)
{
	ssize_t ret, copied = 0;

	if (!count)
		return 0;
	ret = mutex_lock_interruptible(&sulog_read_lock);
	if (ret)
		return ret;
	if (!ksu_sulog_has_data()) {
		ret = file_flags & O_NONBLOCK ? -EAGAIN :
			wait_event_interruptible(sulog_read_wait, ksu_sulog_has_data());
		if (ret) {
			copied = ret;
			goto out;
		}
	}
	while (count) {
		ret = ksu_sulog_read_drop(buf, count);
		if (!ret)
			ret = ksu_sulog_read_record(buf, count);
		if (ret <= 0) {
			if (!copied)
				copied = ret;
			break;
		}
		copied += ret;
		buf += ret;
		count -= ret;
	}
out:
	mutex_unlock(&sulog_read_lock);
	return copied;
}

static unsigned __bitwise ksu_sulog_queue_poll(struct file *file, poll_table *wait)
{
	unsigned __bitwise mask = 0;
	unsigned long flags;

	poll_wait(file, &sulog_read_wait, wait);
	spin_lock_irqsave(&sulog_queue_lock, flags);
	if (ksu_sulog_has_data_locked())
		mask = POLLIN | POLLRDNORM;
	spin_unlock_irqrestore(&sulog_queue_lock, flags);
	return mask;
}
