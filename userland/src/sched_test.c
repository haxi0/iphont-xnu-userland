/*
 * The scheduler measured from user space, with repeatable workloads (rc.jobs lines, tools/t sched*): the same runs
 * under the Edge scheduler and the Clutch baseline (boot-arg sched=clutch) are compared by tools/sched-test/amp.py.
 * Every result is one "t-sched: NAME key=value ..." line.
 *
 *   sched_test amp LABEL            the kernel's view (kern.hackios.amp), each line as "t-amp LABEL: ..."
 *   sched_test check                the kernel's own checks, now (kern.hackios.sched_check: bsd/dev/hk_sched_check.c)
 *   sched_test spin QOS N SECS      N busy threads at QOS (ui in df ut bg): work done, how evenly, where they ran
 *   sched_test fair SECS NA NB      two thread groups (work-interval groups), NA and NB busy default-QoS threads:
 *                                   the processor time each group got (the Clutch scheduler shares between groups)
 *   sched_test frames PERIOD_MS WORK_US SECS [BGN [BGQOS]]
 *                                   a user-interactive "render" thread in a group of its own: WORK_US of work (as
 *                                   timed on the fastest core) every PERIOD_MS, with a work interval telling the
 *                                   kernel each frame's deadline; BGN busy threads at BGQOS (default ut) in another
 *                                   group. Frames late, frame time p50/p95/p99, wake lateness, where it ran
 *   sched_test lat QOS N SECS [BGN [BGQOS]]
 *                                   N threads at QOS sleeping 1-4 ms at random: how late they wake, with load
 *   sched_test mix SECS             the usual mixture at once: a 60 Hz render thread (4 ms of work), 2 default-QoS
 *                                   workers, 3 utility and 2 background spinners, each kind its own group
 *   sched_test pi SECS HOLD_US      priority inheritance: a default-QoS thread of a light group (so on E) takes a lock
 *                                   every 15 ms and holds it for HOLD_US of work (as timed on the fastest core); a
 *                                   user-interactive thread of another group waits for that lock now and then. How long
 *                                   it waits, and where the holder ran while it held the lock with a waiter
 */
#include <errno.h>
#include <os/lock.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

/* libsystem's private work interval interface (xnu bsd/sys/work_interval.h) */
typedef struct work_interval *work_interval_t;
extern int work_interval_create(work_interval_t *interval_handle, uint32_t create_flags);
extern int work_interval_notify(work_interval_t interval_handle, uint64_t start, uint64_t finish, uint64_t deadline,
    uint64_t next_start, uint32_t notify_flags);
extern int work_interval_join(work_interval_t interval_handle);
extern int work_interval_leave(void);
#define WORK_INTERVAL_FLAG_JOINABLE     0x1
#define WORK_INTERVAL_FLAG_GROUP        0x2
#define WORK_INTERVAL_TYPE_DEFAULT      0x0

#define MAX_THREADS     32
#define MAX_CPUS        8

static mach_timebase_info_data_t tb;
static int cpu_small[MAX_CPUS];         /* by cpu number: 1 a small (E) core, from kern.hackios.amp's "core" lines */
static int ncpus;

static uint64_t
ns_of(uint64_t mach)
{
	return mach * tb.numer / tb.denom;
}

static uint64_t
mach_of_ns(uint64_t ns)
{
	return ns * tb.denom / tb.numer;
}

static int
cpu_now(void)
{
	uint64_t v;

	__asm__ volatile ("mrs %0, TPIDR_EL0" : "=r"(v));
	return (int)(v & 0xfff);
}

static char *
amp_text(void)
{
	/* (the report grows between the two calls, groups come and go: room to spare, and a second try) */
	for (int attempt = 0; attempt < 3; attempt++) {
		size_t size = 0;
		char *text;

		if (sysctlbyname("kern.hackios.amp", NULL, &size, NULL, 0) != 0 || size == 0) {
			return NULL;
		}
		size += 4096;
		text = calloc(1, size + 16);
		if (text != NULL && sysctlbyname("kern.hackios.amp", text, &size, NULL, 0) == 0) {
			return text;
		}
		free(text);
	}
	return NULL;
}

/* The energy model's total (kern.hackios.amp "energy mj ... total N"), mJ since boot; 0 without a controller */
static uint64_t
energy_mj(void)
{
	char *text = amp_text(), *at;
	unsigned long long mj = 0;

	if (text != NULL && (at = strstr(text, " total ")) != NULL) {
		sscanf(at, " total %llu", &mj);
	}
	free(text);
	return mj;
}

static void
read_cores(void)
{
	char *text = amp_text(), *line;
	int cpu, domain;
	char kind[16];

	for (line = text; line != NULL && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
		if (sscanf(line, "core %d domain %d %15s", &cpu, &domain, kind) == 3 && cpu >= 0 && cpu < MAX_CPUS) {
			cpu_small[cpu] = strncmp(kind, "small", 5) == 0;
			ncpus = cpu + 1 > ncpus ? cpu + 1 : ncpus;
		}
	}
	free(text);
}

static qos_class_t
qos_of(const char *name)
{
	return strcmp(name, "ui") == 0 ? QOS_CLASS_USER_INTERACTIVE : strcmp(name, "in") == 0 ? QOS_CLASS_USER_INITIATED :
	       strcmp(name, "ut") == 0 ? QOS_CLASS_UTILITY : strcmp(name, "bg") == 0 ? QOS_CLASS_BACKGROUND : QOS_CLASS_DEFAULT;
}

/* A new thread group (a joinable work interval with a group of its own), for threads that join it */
static work_interval_t
new_group(void)
{
	work_interval_t wi = NULL;

	if (work_interval_create(&wi, WORK_INTERVAL_TYPE_DEFAULT | WORK_INTERVAL_FLAG_GROUP | WORK_INTERVAL_FLAG_JOINABLE) != 0) {
		printf("sched_test: work_interval_create: %s (threads stay in the process's group)\n", strerror(errno));
		return NULL;
	}
	return wi;
}

/* ---- busy threads ---------------------------------------------------------------------------------------------- */

struct spinner {
	pthread_t thread;
	qos_class_t qos;
	work_interval_t group;
	_Atomic int *stop;
	uint64_t iterations, samples, small_samples, cpu_ns;
	uint32_t cpus_seen;
};

static void *
spin_main(void *arg)
{
	struct spinner *s = arg;
	volatile uint64_t sink = 0;

	pthread_set_qos_class_self_np(s->qos, 0);
	if (s->group != NULL) {
		work_interval_join(s->group);
	}
	while (!atomic_load_explicit(s->stop, memory_order_relaxed)) {
		for (int i = 0; i < 20000; i++) {
			sink += (uint64_t)i * 2654435761u;
		}
		s->iterations++;
		int cpu = cpu_now();
		s->samples++;
		if (cpu < MAX_CPUS) {
			s->small_samples += cpu_small[cpu];
			s->cpus_seen |= 1u << cpu;
		}
	}
	struct timespec ts;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	s->cpu_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
	if (s->group != NULL) {
		work_interval_leave();
	}
	return NULL;
}

static void
spin_start(struct spinner *s, int n, qos_class_t qos, work_interval_t group, _Atomic int *stop)
{
	for (int i = 0; i < n; i++) {
		memset(&s[i], 0, sizeof(s[i]));
		s[i].qos = qos;
		s[i].group = group;
		s[i].stop = stop;
		pthread_create(&s[i].thread, NULL, spin_main, &s[i]);
	}
}

static void
spin_join(struct spinner *s, int n)
{
	for (int i = 0; i < n; i++) {
		pthread_join(s[i].thread, NULL);
	}
}

static uint64_t report_mj;       /* the system's energy over the run being reported, mJ (energy model) */

static void
spin_report(const char *name, struct spinner *s, int n, double secs)
{
	uint64_t total = 0, min = UINT64_MAX, max = 0, samples = 0, small = 0, cpu_ns = 0;
	uint32_t seen = 0;

	for (int i = 0; i < n; i++) {
		total += s[i].iterations;
		min = s[i].iterations < min ? s[i].iterations : min;
		max = s[i].iterations > max ? s[i].iterations : max;
		samples += s[i].samples;
		small += s[i].small_samples;
		seen |= s[i].cpus_seen;
		cpu_ns += s[i].cpu_ns;
	}
	printf("t-sched: %s threads=%d work=%.0f/s min/max=%.2f cpu=%.2f E%%=%.0f cpus=0x%x energy_mj=%llu work_per_mj=%.1f\n", name, n,
	    total / secs, max ? (double)min / (double)max : 0, cpu_ns / 1e9 / secs, samples ? 100.0 * small / samples : 0, seen,
	    report_mj, report_mj ? (double)total / report_mj : 0);
}

static int
cmd_spin(int argc, char **argv)
{
	static struct spinner s[MAX_THREADS];
	_Atomic int stop = 0;
	int n = argc > 3 ? atoi(argv[3]) : 2;
	double secs = argc > 4 ? atof(argv[4]) : 3;
	char name[32];

	n = n < 1 ? 1 : n > MAX_THREADS ? MAX_THREADS : n;
	uint64_t e0 = energy_mj();
	spin_start(s, n, qos_of(argv[2]), new_group(), &stop);
	usleep((useconds_t)(secs * 1e6));
	atomic_store(&stop, 1);
	spin_join(s, n);
	report_mj = energy_mj() - e0;
	snprintf(name, sizeof(name), "spin-%s", argv[2]);
	spin_report(name, s, n, secs);
	return 0;
}

static int
cmd_fair(int argc, char **argv)
{
	static struct spinner a[MAX_THREADS], b[MAX_THREADS];
	_Atomic int stop = 0;
	double secs = argc > 2 ? atof(argv[2]) : 4;
	int na = argc > 3 ? atoi(argv[3]) : 1, nb = argc > 4 ? atoi(argv[4]) : 8;

	na = na < 1 ? 1 : na > MAX_THREADS ? MAX_THREADS : na;
	nb = nb < 1 ? 1 : nb > MAX_THREADS ? MAX_THREADS : nb;
	uint64_t e0 = energy_mj();
	spin_start(a, na, QOS_CLASS_DEFAULT, new_group(), &stop);
	spin_start(b, nb, QOS_CLASS_DEFAULT, new_group(), &stop);
	usleep((useconds_t)(secs * 1e6));
	atomic_store(&stop, 1);
	spin_join(a, na);
	spin_join(b, nb);
	report_mj = energy_mj() - e0;
	uint64_t ca = 0, cb = 0;
	for (int i = 0; i < na; i++) {
		ca += a[i].cpu_ns;
	}
	for (int i = 0; i < nb; i++) {
		cb += b[i].cpu_ns;
	}
	spin_report("fair-a", a, na, secs);
	spin_report("fair-b", b, nb, secs);
	printf("t-sched: fair groups=%d/%d cpu_a=%.2f cpu_b=%.2f share_a=%.0f%% per_thread_a/b=%.2f\n", na, nb, ca / 1e9 / secs,
	    cb / 1e9 / secs, ca + cb ? 100.0 * ca / (ca + cb) : 0, cb && na ? ((double)ca / na) / ((double)cb / nb) : 0);
	return 0;
}

/* ---- a render loop with deadlines --------------------------------------------------------------------------------- */

static double loop_per_us;              /* iterations of work() a microsecond on the fastest core seen */

static void
work(uint64_t iterations)
{
	volatile uint64_t sink = 0;

	for (uint64_t i = 0; i < iterations; i++) {
		sink += i * 2654435761u;
	}
}

static void
calibrate(void)
{
	if (loop_per_us > 0) {
		return;
	}
	for (int r = 0; r < 60; r++) {
		uint64_t t0 = mach_absolute_time();
		work(20000);
		double us = ns_of(mach_absolute_time() - t0) / 1000.0;
		if (us > 0 && 20000 / us > loop_per_us) {
			loop_per_us = 20000 / us;
		}
		usleep(2000);
	}
}

static int
cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return x < y ? -1 : x > y;
}

static uint64_t
pct(uint64_t *v, size_t n, int p)
{
	return n ? v[(n - 1) * (size_t)p / 100] : 0;
}

struct render {
	uint64_t period_ns, work_us;
	double secs;
	const char *name;
	/* out */
	uint64_t frames, late, small_frames;
	uint64_t *frame_us, *wake_us;
};

static void *
render_main(void *arg)
{
	struct render *r = arg;
	work_interval_t wi = NULL;
	uint64_t period = mach_of_ns(r->period_ns), iterations = (uint64_t)(r->work_us * loop_per_us);
	uint64_t n = (uint64_t)(r->secs * 1e9 / r->period_ns);

	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
	if (work_interval_create(&wi, WORK_INTERVAL_TYPE_DEFAULT | WORK_INTERVAL_FLAG_GROUP) != 0) {
		wi = NULL;
	}
	r->frame_us = calloc(n + 1, sizeof(uint64_t));
	r->wake_us = calloc(n + 1, sizeof(uint64_t));
	uint64_t start = mach_absolute_time() + period;
	for (uint64_t i = 0; i < n; i++, start += period) {
		mach_wait_until(start);
		uint64_t woke = mach_absolute_time();
		work(iterations);
		uint64_t done = mach_absolute_time(), deadline = start + period;
		if (wi != NULL) {
			work_interval_notify(wi, start, done, deadline, deadline, 0);
		}
		r->wake_us[r->frames] = ns_of(woke - start) / 1000;
		r->frame_us[r->frames] = ns_of(done - start) / 1000;
		r->late += done > deadline;
		int cpu = cpu_now();
		r->small_frames += cpu < MAX_CPUS && cpu_small[cpu];
		r->frames++;
		if (done > start + period) {
			start = done - period;      /* (a late frame: the next starts from now, as a display link does) */
		}
	}
	return NULL;
}

static void
render_report(struct render *r)
{
	qsort(r->frame_us, r->frames, sizeof(uint64_t), cmp_u64);
	qsort(r->wake_us, r->frames, sizeof(uint64_t), cmp_u64);
	printf("t-sched: %s frames=%llu late=%llu late%%=%.1f frame_us_p50=%llu p95=%llu p99=%llu max=%llu wake_us_p50=%llu p99=%llu "
	    "max=%llu E%%=%.0f period_us=%llu work_us=%llu energy_mj=%llu\n", r->name, r->frames, r->late, r->frames ? 100.0 * r->late / r->frames : 0,
	    pct(r->frame_us, r->frames, 50), pct(r->frame_us, r->frames, 95), pct(r->frame_us, r->frames, 99),
	    r->frames ? r->frame_us[r->frames - 1] : 0, pct(r->wake_us, r->frames, 50), pct(r->wake_us, r->frames, 99),
	    r->frames ? r->wake_us[r->frames - 1] : 0, r->frames ? 100.0 * r->small_frames / r->frames : 0, r->period_ns / 1000, r->work_us,
	    report_mj);
	free(r->frame_us);
	free(r->wake_us);
}

static int
cmd_frames(int argc, char **argv)
{
	static struct spinner bg[MAX_THREADS];
	_Atomic int stop = 0;
	struct render r = { .name = "frames" };
	pthread_t t;
	int bgn = argc > 5 ? atoi(argv[5]) : 0;

	r.period_ns = (uint64_t)(atof(argc > 2 ? argv[2] : "16.67") * 1e6);
	r.work_us = (uint64_t)atoi(argc > 3 ? argv[3] : "4000");
	r.secs = argc > 4 ? atof(argv[4]) : 5;
	bgn = bgn < 0 ? 0 : bgn > MAX_THREADS ? MAX_THREADS : bgn;
	calibrate();
	uint64_t e0 = energy_mj();
	if (bgn) {
		spin_start(bg, bgn, qos_of(argc > 6 ? argv[6] : "ut"), new_group(), &stop);
	}
	pthread_create(&t, NULL, render_main, &r);
	pthread_join(t, NULL);
	atomic_store(&stop, 1);
	spin_join(bg, bgn);
	report_mj = energy_mj() - e0;
	render_report(&r);
	if (bgn) {
		spin_report("frames-load", bg, bgn, r.secs);
	}
	return 0;
}

/* ---- wake latency ------------------------------------------------------------------------------------------------ */

struct sleeper {
	pthread_t thread;
	qos_class_t qos;
	double secs;
	uint64_t n, *late_us;
	unsigned seed;
};

static void *
sleeper_main(void *arg)
{
	struct sleeper *s = arg;
	uint64_t end = mach_absolute_time() + mach_of_ns((uint64_t)(s->secs * 1e9));

	pthread_set_qos_class_self_np(s->qos, 0);
	s->late_us = calloc((size_t)(s->secs * 1000) + 1, sizeof(uint64_t));
	while (mach_absolute_time() < end && s->n < (uint64_t)(s->secs * 1000)) {
		uint64_t when = mach_absolute_time() + mach_of_ns((uint64_t)(1000 + rand_r(&s->seed) % 3000) * 1000);
		mach_wait_until(when);
		s->late_us[s->n++] = ns_of(mach_absolute_time() - when) / 1000;
		work(200);
	}
	return NULL;
}

static int
cmd_lat(int argc, char **argv)
{
	static struct spinner bg[MAX_THREADS];
	static struct sleeper sl[MAX_THREADS];
	_Atomic int stop = 0;
	int n = argc > 3 ? atoi(argv[3]) : 4, bgn = argc > 5 ? atoi(argv[5]) : 0;
	double secs = argc > 4 ? atof(argv[4]) : 3;
	uint64_t *all, count = 0;

	n = n < 1 ? 1 : n > MAX_THREADS ? MAX_THREADS : n;
	bgn = bgn < 0 ? 0 : bgn > MAX_THREADS ? MAX_THREADS : bgn;
	if (bgn) {
		spin_start(bg, bgn, qos_of(argc > 6 ? argv[6] : "df"), new_group(), &stop);
	}
	for (int i = 0; i < n; i++) {
		sl[i] = (struct sleeper){ .qos = qos_of(argv[2]), .secs = secs, .seed = (unsigned)(i * 7919 + 1) };
		pthread_create(&sl[i].thread, NULL, sleeper_main, &sl[i]);
	}
	all = calloc((size_t)n * ((size_t)(secs * 1000) + 1), sizeof(uint64_t));
	for (int i = 0; i < n; i++) {
		pthread_join(sl[i].thread, NULL);
		memcpy(all + count, sl[i].late_us, sl[i].n * sizeof(uint64_t));
		count += sl[i].n;
		free(sl[i].late_us);
	}
	atomic_store(&stop, 1);
	spin_join(bg, bgn);
	qsort(all, count, sizeof(uint64_t), cmp_u64);
	printf("t-sched: lat-%s threads=%d load=%d wakes=%llu late_us_p50=%llu p90=%llu p99=%llu max=%llu\n", argv[2], n, bgn, count,
	    pct(all, count, 50), pct(all, count, 90), pct(all, count, 99), count ? all[count - 1] : 0);
	free(all);
	return 0;
}

static int
cmd_mix(int argc, char **argv)
{
	static struct spinner df[2], ut[3], bg[2];
	_Atomic int stop = 0;
	struct render r = { .name = "mix-frames", .period_ns = 16666667, .work_us = 4000 };
	pthread_t t;

	r.secs = argc > 2 ? atof(argv[2]) : 6;
	calibrate();
	uint64_t e0 = energy_mj();
	spin_start(df, 2, QOS_CLASS_DEFAULT, new_group(), &stop);
	spin_start(ut, 3, QOS_CLASS_UTILITY, new_group(), &stop);
	spin_start(bg, 2, QOS_CLASS_BACKGROUND, new_group(), &stop);
	pthread_create(&t, NULL, render_main, &r);
	pthread_join(t, NULL);
	atomic_store(&stop, 1);
	spin_join(df, 2);
	spin_join(ut, 3);
	spin_join(bg, 2);
	report_mj = energy_mj() - e0;
	render_report(&r);
	spin_report("mix-df", df, 2, r.secs);
	spin_report("mix-ut", ut, 3, r.secs);
	spin_report("mix-bg", bg, 2, r.secs);
	return 0;
}

/* ---- priority inheritance through a lock ------------------------------------------------------------------------ */

static os_unfair_lock pi_lock = OS_UNFAIR_LOCK_INIT;
static _Atomic int pi_stop, pi_waiting;
static uint64_t pi_hold_iterations, pi_holds, pi_holds_small, pi_holds_waited;

static void *
pi_holder(void *arg)
{
	work_interval_t group = arg;

	pthread_set_qos_class_self_np(QOS_CLASS_DEFAULT, 0);
	if (group != NULL) {
		work_interval_join(group);
	}
	while (!atomic_load(&pi_stop)) {
		os_unfair_lock_lock(&pi_lock);
		work(pi_hold_iterations * 3 / 4);
		int waited = atomic_load(&pi_waiting), cpu = cpu_now();        /* (late in the hold: after any push arrived) */
		work(pi_hold_iterations / 4);
		pi_holds++;
		if (waited) {
			pi_holds_waited++;
			pi_holds_small += cpu < MAX_CPUS && cpu_small[cpu];
		}
		os_unfair_lock_unlock(&pi_lock);
		usleep(15000);
	}
	if (group != NULL) {
		work_interval_leave();
	}
	return NULL;
}

static int
cmd_pi(int argc, char **argv)
{
	double secs = argc > 2 ? atof(argv[2]) : 4;
	uint64_t hold_us = argc > 3 ? (uint64_t)atoi(argv[3]) : 2000, n = 0, *wait_us;
	pthread_t holder;
	unsigned seed = 1;

	calibrate();
	pi_hold_iterations = (uint64_t)(hold_us * loop_per_us);
	wait_us = calloc((size_t)(secs * 200) + 1, sizeof(uint64_t));
	pthread_create(&holder, NULL, pi_holder, new_group());
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
	work_interval_t mine = new_group();
	if (mine != NULL) {
		work_interval_join(mine);
	}
	uint64_t end = mach_absolute_time() + mach_of_ns((uint64_t)(secs * 1e9));
	while (mach_absolute_time() < end && n < (uint64_t)(secs * 200)) {
		usleep(5000 + rand_r(&seed) % 5000);
		atomic_store(&pi_waiting, 1);
		uint64_t t0 = mach_absolute_time();
		os_unfair_lock_lock(&pi_lock);
		uint64_t t1 = mach_absolute_time();
		atomic_store(&pi_waiting, 0);
		os_unfair_lock_unlock(&pi_lock);
		wait_us[n++] = ns_of(t1 - t0) / 1000;
	}
	atomic_store(&pi_stop, 1);
	pthread_join(holder, NULL);
	qsort(wait_us, n, sizeof(uint64_t), cmp_u64);
	printf("t-sched: pi hold_us=%llu waits=%llu wait_us_p50=%llu p90=%llu p99=%llu max=%llu holds=%llu holds_waited=%llu "
	    "holder_E%%_while_waited=%.0f\n", hold_us, n, pct(wait_us, n, 50), pct(wait_us, n, 90), pct(wait_us, n, 99),
	    n ? wait_us[n - 1] : 0, pi_holds, pi_holds_waited, pi_holds_waited ? 100.0 * pi_holds_small / pi_holds_waited : 0);
	free(wait_us);
	return 0;
}

int
main(int argc, char **argv)
{
	mach_timebase_info(&tb);
	setvbuf(stdout, NULL, _IOLBF, 0);
	read_cores();
	if (argc >= 2 && strcmp(argv[1], "amp") == 0) {
		char *text = amp_text(), *line, *next;
		const char *label = argc >= 3 ? argv[2] : "now";

		if (text == NULL) {
			printf("t-amp %s: no kern.hackios.amp\n", label);
			return 1;
		}
		for (line = text; line && *line; line = next) {
			next = strchr(line, '\n');
			if (next) {
				*next++ = 0;
			}
			printf("t-amp %s: %s\n", label, line);
		}
		free(text);
		return 0;
	}
	if (argc >= 2 && strcmp(argv[1], "check") == 0) {
		int one = 1, result = 0;
		size_t size = sizeof(result);
		if (sysctlbyname("kern.hackios.sched_check", &result, &size, &one, sizeof(one)) != 0) {
			printf("t-sched: check: kern.hackios.sched_check: %s\n", strerror(errno));
			return 1;
		}
		printf("t-sched: check passed=%d failed=%d\n", result >> 16, result & 0xffff);
		return 0;
	}
	if (argc >= 3 && strcmp(argv[1], "spin") == 0) {
		return cmd_spin(argc, argv);
	}
	if (argc >= 2 && strcmp(argv[1], "fair") == 0) {
		return cmd_fair(argc, argv);
	}
	if (argc >= 2 && strcmp(argv[1], "frames") == 0) {
		return cmd_frames(argc, argv);
	}
	if (argc >= 3 && strcmp(argv[1], "lat") == 0) {
		return cmd_lat(argc, argv);
	}
	if (argc >= 2 && strcmp(argv[1], "mix") == 0) {
		return cmd_mix(argc, argv);
	}
	if (argc >= 2 && strcmp(argv[1], "pi") == 0) {
		return cmd_pi(argc, argv);
	}
	fprintf(stderr, "usage: sched_test amp LABEL | check | spin QOS N SECS | fair SECS NA NB | frames PERIOD_MS WORK_US SECS [BGN [BGQOS]] |\n"
	    "       lat QOS N SECS [BGN [BGQOS]] | mix SECS | pi SECS HOLD_US\n");
	return 2;
}
