/*
 * A sampling profiler for a process of ours (included by inject.c and inject_sb.c): a thread of the process reads the program
 * counters of the others that are runnable, every 5 ms; on the notification
 * com.hackios.prof the samples are grouped by function (dladdr: the symbol, or the image and a 256-byte bucket where the image
 * has none) and the first thirty are written down as "hkprof:" lines. com.hackios.prof.start clears the samples and starts the
 * clock, so that a gesture can be bracketed: `rwtouch notify com.hackios.prof.start ... notify com.hackios.prof`. Offsets of an
 * image without symbols are looked up offline in the unstripped library (xnu-deps/build/mesa/.../libOSMesa.8.dylib: nm, or
 * llvm-symbolizer with the offset). Boot-arg hkprof=1 turns the notifications on in the programs that include this.
 */
#include <dlfcn.h>
#include <mach/mach.h>
#include <notify.h>
#include <pthread.h>
#include <unistd.h>

#define HKPROF_MAX 40000
#define HKPROF_THREADS 256

static uintptr_t hkprof_pcs[HKPROF_MAX];
static volatile unsigned hkprof_count;
static volatile int hkprof_running;
static void (*hkprof_say)(const char *format, ...);

/*
 * Every 5 ms, each thread of the process that is runnable (TH_STATE_RUNNING: on a core or waiting for one) is stopped and
 * its program counter read (thread_get_state): the place it is at, not where a signal would find it -- a timer signal is
 * delivered where the thread leaves the kernel, and put 78% of a window server's time "in mach_msg" that was its own code.
 */
static void *
hkprof_sampler(__unused void *argument)
{
	mach_port_t self = mach_thread_self();

	while (hkprof_running) {
		thread_act_array_t threads;
		mach_msg_type_number_t count = 0;

		usleep(5000);
		if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) {
			continue;
		}
		for (mach_msg_type_number_t i = 0; i < count; i++) {
			thread_basic_info_data_t info;
			mach_msg_type_number_t info_count = THREAD_BASIC_INFO_COUNT;

			if (threads[i] != self && hkprof_count < HKPROF_MAX &&
			    thread_info(threads[i], THREAD_BASIC_INFO, (thread_info_t)&info, &info_count) == KERN_SUCCESS &&
			    info.run_state == TH_STATE_RUNNING && !(info.flags & TH_FLAGS_IDLE)) {
				arm_thread_state64_t state;
				mach_msg_type_number_t state_count = ARM_THREAD_STATE64_COUNT;

				if (thread_get_state(threads[i], ARM_THREAD_STATE64, (thread_state_t)&state, &state_count) == KERN_SUCCESS) {
					hkprof_pcs[hkprof_count++] = (uintptr_t)__darwin_arm_thread_state64_get_pc(state);
				}
			}
			mach_port_deallocate(mach_task_self(), threads[i]);
		}
		vm_deallocate(mach_task_self(), (vm_address_t)threads, count * sizeof(threads[0]));
	}
	return NULL;
}

static void
hkprof_start(void)
{
	pthread_t thread;

	if (hkprof_running) {
		return;
	}
	hkprof_count = 0;
	hkprof_running = 1;
	pthread_create(&thread, NULL, hkprof_sampler, NULL);
	pthread_detach(thread);
}

struct hkprof_key {
	const char *image, *symbol;
	uintptr_t bucket;
	unsigned count;
};

static void
hkprof_dump(const char *who)
{
	static struct hkprof_key keys[4096];
	unsigned used = 0, total = hkprof_count;

	hkprof_running = 0;
	usleep(20000);
	for (unsigned i = 0; i < total; i++) {
		Dl_info info;
		struct hkprof_key key = { "?", NULL, hkprof_pcs[i] >> 8, 1 };

		if (dladdr((void *)hkprof_pcs[i], &info) != 0) {
			const char *slash = info.dli_fname ? strrchr(info.dli_fname, '/') : NULL;

			key.image = slash ? slash + 1 : info.dli_fname ? info.dli_fname : "?";
			/* a symbol only where the pc is near it: an image stripped of local symbols puts every function between two
			   exported ones under the first (Mesa: 19% "in glFramebufferTextureMultisampleMultiviewOVR") */
			key.symbol = info.dli_sname != NULL && hkprof_pcs[i] - (uintptr_t)info.dli_saddr < 0x200 ? info.dli_sname : NULL;
			key.bucket = key.symbol ? (uintptr_t)info.dli_saddr : (hkprof_pcs[i] - (uintptr_t)info.dli_fbase) >> 8;
		}
		unsigned k = 0;

		while (k < used && !(keys[k].bucket == key.bucket && keys[k].symbol == key.symbol && strcmp(keys[k].image, key.image) == 0)) {
			k++;
		}
		if (k < used) {
			keys[k].count++;
		} else if (used < sizeof(keys) / sizeof(keys[0])) {
			keys[used++] = key;
		}
	}
	hkprof_say("hkprof: %s: %u samples of a runnable thread, 5 ms apart (%u thread-ms), %u places\n", who, total, total * 5, used);
	for (unsigned n = 0; n < 30 && n < used; n++) {
		unsigned best = n;

		for (unsigned k = n + 1; k < used; k++) {
			best = keys[k].count > keys[best].count ? k : best;
		}
		struct hkprof_key swap = keys[n];

		keys[n] = keys[best];
		keys[best] = swap;
		if (keys[n].symbol != NULL) {
			hkprof_say("hkprof: %5u %4.1f%% %s %s\n", keys[n].count, 100.0 * keys[n].count / (total ? total : 1), keys[n].image,
			    keys[n].symbol);
		} else {
			hkprof_say("hkprof: %5u %4.1f%% %s +%#lx\n", keys[n].count, 100.0 * keys[n].count / (total ? total : 1), keys[n].image,
			    (unsigned long)(keys[n].bucket << 8));
		}
	}
}

/* the two notifications, where the boot-args ask for them */
static void
hkprof_register(const char *who, void (*say_function)(const char *, ...))
{
	int token;

	hkprof_say = say_function;
	notify_register_dispatch("com.hackios.prof.start", &token, dispatch_get_main_queue(), ^(__unused int t) {
		hkprof_start();
	});
	notify_register_dispatch("com.hackios.prof", &token, dispatch_get_main_queue(), ^(__unused int t) {
		hkprof_dump(who);
	});
}
