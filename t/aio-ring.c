/*
 * gcc -D_GNU_SOURCE -Wall -O2 -o aio-ring aio-ring.c  -lpthread -laio
 */
#include <stdio.h>
#include <errno.h>
#include <assert.h>
#include <stdlib.h>
#include <stddef.h>
#include <signal.h>
#include <inttypes.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <linux/fs.h>
#include <fcntl.h>
#include <unistd.h>
#include <libaio.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>

#include "../arch/arch.h"

#define IOCTX_FLAG_SCQRING	(1 << 0)	/* Use SQ/CQ rings */
#define IOCTX_FLAG_IOPOLL	(1 << 1)
#define IOCTX_FLAG_FIXEDBUFS	(1 << 2)
#define IOCTX_FLAG_SQTHREAD	(1 << 3)	/* Use SQ thread */
#define IOCTX_FLAG_SQWQ		(1 << 4)	/* Use SQ wq */
#define IOCTX_FLAG_SQPOLL	(1 << 5)

#define IOEV_RES2_CACHEHIT	(1 << 0)

#define barrier()	__asm__ __volatile__("": : :"memory")

#define min(a, b)		((a < b) ? (a) : (b))

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;

#define IORING_OFF_SQ_RING	0ULL
#define IORING_OFF_CQ_RING	0x8000000ULL
#define IORING_OFF_IOCB		0x10000000ULL

struct aio_sqring_offsets {
	u32 head;
	u32 tail;
	u32 ring_mask;
	u32 ring_entries;
	u32 flags;
	u32 array;
};

struct aio_cqring_offsets {
	u32 head;
	u32 tail;
	u32 ring_mask;
	u32 ring_entries;
	u32 overflow;
	u32 events;
};

struct aio_uring_params {
	u32 sq_entries;
	u32 cq_entries;
	u32 flags;
	u16 sq_thread_cpu;
	u16 resv[9];
	struct aio_sqring_offsets sq_off;
	struct aio_cqring_offsets cq_off;
};

struct aio_sq_ring {
	u32 *head;
	u32 *tail;
	u32 *ring_mask;
	u32 *ring_entries;
	u32 *array;
};

struct aio_cq_ring {
	u32 *head;
	u32 *tail;
	u32 *ring_mask;
	u32 *ring_entries;
	struct io_event *events;
};

#define IORING_ENTER_GETEVENTS	(1 << 0)

#define DEPTH			32

#define BATCH_SUBMIT		8
#define BATCH_COMPLETE		8

#define BS			4096

static unsigned sq_ring_mask, cq_ring_mask;

struct submitter {
	pthread_t thread;
	unsigned long max_blocks;
	int fd;
	struct drand48_data rand;
	struct aio_sq_ring sq_ring;
	struct iocb *iocbs;
	struct iovec iovecs[DEPTH];
	struct aio_cq_ring cq_ring;
	int inflight;
	unsigned long reaps;
	unsigned long done;
	unsigned long calls;
	unsigned long cachehit, cachemiss;
	volatile int finish;
	char filename[128];
};

static struct submitter submitters[1];
static volatile int finish;

static int polled = 0;		/* use IO polling */
static int fixedbufs = 0;	/* use fixed user buffers */
static int buffered = 1;	/* use buffered IO, not O_DIRECT */
static int sq_thread = 0;	/* use kernel submission thread */
static int sq_thread_cpu = 0;	/* pin above thread to this CPU */

static int io_uring_setup(unsigned entries, struct iovec *iovecs,
			  struct aio_uring_params *p)
{
	return syscall(__NR_sys_io_uring_setup, entries, iovecs, p);
}

static int io_uring_enter(struct submitter *s, unsigned int to_submit,
			  unsigned int min_complete, unsigned int flags)
{
	return syscall(__NR_sys_io_uring_enter, s->fd, to_submit, min_complete,
			flags);
}

static int gettid(void)
{
	return syscall(__NR_gettid);
}

static void init_io(struct submitter *s, int fd, unsigned index)
{
	struct iocb *iocb = &s->iocbs[index];
	unsigned long offset;
	long r;

	lrand48_r(&s->rand, &r);
	offset = (r % (s->max_blocks - 1)) * BS;

	iocb->aio_fildes = fd;
	iocb->aio_lio_opcode = IO_CMD_PREAD;
	iocb->u.c.buf = s->iovecs[index].iov_base;
	iocb->u.c.nbytes = BS;
	iocb->u.c.offset = offset;
}

static int prep_more_ios(struct submitter *s, int fd, int max_ios)
{
	struct aio_sq_ring *ring = &s->sq_ring;
	u32 index, tail, next_tail, prepped = 0;

	next_tail = tail = *ring->tail;
	do {
		next_tail++;
		barrier();
		if (next_tail == *ring->head)
			break;

		index = tail & sq_ring_mask;
		init_io(s, fd, index);
		ring->array[index] = index;
		prepped++;
		tail = next_tail;
	} while (prepped < max_ios);

	if (*ring->tail != tail) {
		/* order tail store with writes to iocbs above */
		barrier();
		*ring->tail = tail;
		barrier();
	}
	return prepped;
}

static int get_file_size(int fd, unsigned long *blocks)
{
	struct stat st;

	if (fstat(fd, &st) < 0)
		return -1;
	if (S_ISBLK(st.st_mode)) {
		unsigned long long bytes;

		if (ioctl(fd, BLKGETSIZE64, &bytes) != 0)
			return -1;

		*blocks = bytes / BS;
		return 0;
	} else if (S_ISREG(st.st_mode)) {
		*blocks = st.st_size / BS;
		return 0;
	}

	return -1;
}

static int reap_events(struct submitter *s)
{
	struct aio_cq_ring *ring = &s->cq_ring;
	struct io_event *ev;
	u32 head, reaped = 0;

	head = *ring->head;
	do {
		barrier();
		if (head == *ring->tail)
			break;
		ev = &ring->events[head & cq_ring_mask];
		if (ev->res != BS) {
			struct iocb *iocb = ev->obj;

			printf("io: unexpected ret=%ld\n", ev->res);
			printf("offset=%lu, size=%lu\n", (unsigned long) iocb->u.c.offset, (unsigned long) iocb->u.c.nbytes);
			return -1;
		}
		if (ev->res2 & IOEV_RES2_CACHEHIT)
			s->cachehit++;
		else
			s->cachemiss++;
		reaped++;
		head++;
	} while (1);

	s->inflight -= reaped;
	*ring->head = head;
	barrier();
	return reaped;
}

static void *submitter_fn(void *data)
{
	struct submitter *s = data;
	int fd, ret, prepped, flags;

	printf("submitter=%d\n", gettid());

	flags = O_RDONLY;
	if (!buffered)
		flags |= O_DIRECT;
	fd = open(s->filename, flags);
	if (fd < 0) {
		perror("open");
		goto done;
	}

	if (get_file_size(fd, &s->max_blocks)) {
		printf("failed getting size of device/file\n");
		goto err;
	}
	if (!s->max_blocks) {
		printf("Zero file/device size?\n");
		goto err;
	}

	s->max_blocks--;

	srand48_r(pthread_self(), &s->rand);

	prepped = 0;
	do {
		int to_wait, to_submit, this_reap;

		if (!prepped && s->inflight < DEPTH)
			prepped = prep_more_ios(s, fd, min(DEPTH - s->inflight, BATCH_SUBMIT));
		s->inflight += prepped;
submit_more:
		to_submit = prepped;
submit:
		if (s->inflight + BATCH_SUBMIT < DEPTH)
			to_wait = 0;
		else
			to_wait = min(s->inflight + to_submit, BATCH_COMPLETE);

		ret = io_uring_enter(s, to_submit, to_wait, IORING_ENTER_GETEVENTS);
		s->calls++;

		this_reap = reap_events(s);
		if (this_reap == -1)
			break;
		s->reaps += this_reap;

		if (ret >= 0) {
			if (!ret) {
				to_submit = 0;
				if (s->inflight)
					goto submit;
				continue;
			} else if (ret < to_submit) {
				int diff = to_submit - ret;

				s->done += ret;
				prepped -= diff;
				goto submit_more;
			}
			s->done += ret;
			prepped = 0;
			continue;
		} else if (ret < 0) {
			if (errno == EAGAIN) {
				if (s->finish)
					break;
				if (this_reap)
					goto submit;
				to_submit = 0;
				goto submit;
			}
			printf("io_submit: %s\n", strerror(errno));
			break;
		}
	} while (!s->finish);
err:
	close(fd);
done:
	finish = 1;
	return NULL;
}

static void sig_int(int sig)
{
	printf("Exiting on signal %d\n", sig);
	submitters[0].finish = 1;
	finish = 1;
}

static void arm_sig_int(void)
{
	struct sigaction act;

	memset(&act, 0, sizeof(act));
	act.sa_handler = sig_int;
	act.sa_flags = SA_RESTART;
	sigaction(SIGINT, &act, NULL);
}

static int setup_ring(struct submitter *s)
{
	struct aio_sq_ring *sring = &s->sq_ring;
	struct aio_cq_ring *cring = &s->cq_ring;
	struct aio_uring_params p;
	void *ptr;
	int fd;

	memset(&p, 0, sizeof(p));

	p.flags = IOCTX_FLAG_SCQRING;
	if (polled)
		p.flags |= IOCTX_FLAG_IOPOLL;
	if (fixedbufs)
		p.flags |= IOCTX_FLAG_FIXEDBUFS;
	if (buffered)
		p.flags |= IOCTX_FLAG_SQWQ;
	else if (sq_thread) {
		p.flags |= IOCTX_FLAG_SQTHREAD;
		p.sq_thread_cpu = sq_thread_cpu;
	}

	if (fixedbufs)
		fd = io_uring_setup(DEPTH, s->iovecs, &p);
	else
		fd = io_uring_setup(DEPTH, NULL, &p);
	if (fd < 0) {
		perror("io_uring_setup");
		return 1;
	}

	s->fd = fd;

	ptr = mmap(0, p.sq_off.array + p.sq_entries * sizeof(u32),
			PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
			fd, IORING_OFF_SQ_RING);
	printf("sq_ring ptr = 0x%p\n", ptr);
	sring->head = ptr + p.sq_off.head;
	sring->tail = ptr + p.sq_off.tail;
	sring->ring_mask = ptr + p.sq_off.ring_mask;
	sring->ring_entries = ptr + p.sq_off.ring_entries;
	sring->array = ptr + p.sq_off.array;
	sq_ring_mask = *sring->ring_mask;

	s->iocbs = mmap(0, p.sq_entries * sizeof(struct iocb), PROT_READ | PROT_WRITE,
			MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_IOCB);
	printf("iocbs ptr   = 0x%p\n", s->iocbs);

	ptr = mmap(0, p.cq_off.events + p.cq_entries * sizeof(struct io_event),
			PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
			fd, IORING_OFF_CQ_RING);
	printf("cq_ring ptr = 0x%p\n", ptr);
	cring->head = ptr + p.cq_off.head;
	cring->tail = ptr + p.cq_off.tail;
	cring->ring_mask = ptr + p.cq_off.ring_mask;
	cring->ring_entries = ptr + p.cq_off.ring_entries;
	cring->events = ptr + p.cq_off.events;
	cq_ring_mask = *cring->ring_mask;
	return 0;
}

int main(int argc, char *argv[])
{
	struct submitter *s = &submitters[0];
	unsigned long done, calls, reap, cache_hit, cache_miss;
	int err, i;
	struct rlimit rlim;
	void *ret;

	if (argc < 2) {
		printf("%s: filename\n", argv[0]);
		return 1;
	}

	rlim.rlim_cur = RLIM_INFINITY;
	rlim.rlim_max = RLIM_INFINITY;
	if (setrlimit(RLIMIT_MEMLOCK, &rlim) < 0) {
		perror("setrlimit");
		return 1;
	}

	arm_sig_int();

	for (i = 0; i < DEPTH; i++) {
		void *buf;

		if (posix_memalign(&buf, BS, BS)) {
			printf("failed alloc\n");
			return 1;
		}
		s->iovecs[i].iov_base = buf;
		s->iovecs[i].iov_len = BS;
	}

	err = setup_ring(s);
	if (err) {
		printf("ring setup failed: %s, %d\n", strerror(errno), err);
		return 1;
	}
	printf("polled=%d, fixedbufs=%d, buffered=%d", polled, fixedbufs, buffered);
	printf(" QD=%d, sq_ring=%d, cq_ring=%d\n", DEPTH, *s->sq_ring.ring_entries, *s->cq_ring.ring_entries);
	strcpy(s->filename, argv[1]);

	pthread_create(&s->thread, NULL, submitter_fn, s);

	cache_hit = cache_miss = reap = calls = done = 0;
	do {
		unsigned long this_done = 0;
		unsigned long this_reap = 0;
		unsigned long this_call = 0;
		unsigned long this_cache_hit = 0;
		unsigned long this_cache_miss = 0;
		unsigned long rpc = 0, ipc = 0;
		double hit = 0.0;

		sleep(1);
		this_done += s->done;
		this_call += s->calls;
		this_reap += s->reaps;
		this_cache_hit += s->cachehit;
		this_cache_miss += s->cachemiss;
		if (this_cache_hit && this_cache_miss) {
			unsigned long hits, total;

			hits = this_cache_hit - cache_hit;
			total = hits + this_cache_miss - cache_miss;
			hit = (double) hits / (double) total;
			hit *= 100.0;
		}
		if (this_call - calls) {
			rpc = (this_done - done) / (this_call - calls);
			ipc = (this_reap - reap) / (this_call - calls);
		}
		printf("IOPS=%lu, IOS/call=%lu/%lu, inflight=%u (head=%u tail=%u), Cachehit=%0.2f%%\n",
				this_done - done, rpc, ipc, s->inflight,
				*s->cq_ring.head, *s->cq_ring.tail, hit);
		done = this_done;
		calls = this_call;
		reap = this_reap;
		cache_hit = s->cachehit;
		cache_miss = s->cachemiss;
	} while (!finish);

	pthread_join(s->thread, &ret);
	return 0;
}