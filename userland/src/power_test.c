/*
 * The system's power, tried with no hands on the phone (rc.jobs.power). Root only.
 *
 *   power_test status            the kernel's numbers: the panel, the sleeps, the last one, the clocks
 *   power_test display off|on    the display's power as backboardd asks for it (IOMobileFramebufferRequestPowerChange,
 *                                selector 12 of the framebuffer: xnu iokit/Kernel/MiniIOMobileFramebuffer.cpp)
 *   power_test sleep S [N] [powerd]
 *                                N times (default 1): a wake in S seconds from the kernel's own timer (or, with
 *                                `powerd`, asked of powerd: IOPMrootDomain's "wake"), the system put to sleep
 *                                (IOPMSleepSystem) and, the moment it is back, one "t-sleep: PASS|FAIL" line with how
 *                                long it was gone by mach_continuous_time() (which counts the sleep) and by
 *                                mach_absolute_time() (which does not), what woke it and how many cores were off
 *
 * The side button as iOS gets it: rwtouch button lock (/dev/hk-touch). The power key's own interrupt, which wakes
 * the phone from a real press, is only tried by a hand on the key.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

/* IOKit's IOPMLib.h, which the SDK leaves out */
extern io_connect_t IOPMFindPowerManagement(mach_port_t master);
extern IOReturn IOPMSleepSystem(io_connect_t fb);
extern IOReturn IOPMSchedulePowerEvent(CFDateRef time_to_wake, CFStringRef my_id, CFStringRef type);
/* (marked unavailable on iOS in the SDK, there all the same) */
extern kern_return_t registry_set_property(io_registry_entry_t entry, CFStringRef name, CFTypeRef value)
    __asm__("_IORegistryEntrySetCFProperty");

static long long
sysctl_number(const char *name)
{
	long long value = -1;
	size_t size = sizeof(value);
	int i = 0;

	if (sysctlbyname(name, &value, &size, NULL, 0) != 0) {
		return -1;
	}
	if (size == sizeof(int)) {
		memcpy(&i, &value, sizeof(i));
		return i;
	}
	return value;
}

static double
seconds_of(uint64_t mach)
{
	mach_timebase_info_data_t tb;

	mach_timebase_info(&tb);
	return (double)mach * tb.numer / tb.denom / 1e9;
}

static void
status(void)
{
	char last[128] = "";
	size_t size = sizeof(last);
	mach_timebase_info_data_t tb;

	uint64_t absolute = mach_absolute_time(), continuous = mach_continuous_time();   /* (in this order: never negative) */

	mach_timebase_info(&tb);
	sysctlbyname("kern.hackios.sleep_last", last, &size, NULL, 0);
	printf("power_test: panel %s (put to sleep %lld times); the system slept %lld times, %lld s in all; the last: %s; "
	    "awake %llu s, asleep %llu s of mach time\n", sysctl_number("kern.hackios.display_on") == 1 ? "lit" : "asleep",
	    sysctl_number("kern.hackios.display_offs"), sysctl_number("kern.hackios.sleeps"),
	    sysctl_number("kern.hackios.sleep_seconds"), last,
	    (unsigned long long)(seconds_of(absolute)), (unsigned long long)(seconds_of(continuous - absolute)));
}

static int
display(int on)
{
	io_service_t fb = IOServiceGetMatchingService(0, IOServiceMatching("IOMobileFramebuffer"));
	io_connect_t conn = MACH_PORT_NULL;
	uint64_t in = (uint64_t)on;
	kern_return_t kr;

	if (fb == MACH_PORT_NULL || IOServiceOpen(fb, mach_task_self(), 0, &conn) != KERN_SUCCESS) {
		printf("power_test: no framebuffer to ask\n");
		return 1;
	}
	kr = IOConnectCallScalarMethod(conn, 12, &in, 1, NULL, NULL);
	IOServiceClose(conn);
	IOObjectRelease(fb);
	printf("power_test: display power %d: %#x; the panel is %s now\n", on, kr,
	    sysctl_number("kern.hackios.display_on") == 1 ? "lit" : "asleep");
	return kr == KERN_SUCCESS ? 0 : 1;
}


/*
 * One sleep, event by event: the wake is asked for (by default from the kernel's own timer, kern.hackios.sleep_timer; with
 * `powerd` through IOPMSchedulePowerEvent, which powerd does not take for less than about ten seconds, and a sleep
 * that asked for five then ran to the kernel's sleep_max), the system put to sleep, and the moment the kernel counts the
 * sleep it is over. What it says is checked against what a sleep of this kind is: the wake came from what was asked
 * for, the continuous clock ran the time asked (within a second and a half) while mach_absolute_time() stood still
 * (under two seconds), and all but two cores were off when it began (hk_power.c). One "t-sleep:" line says which.
 */
static int
sleep_once(int seconds, int number, int powerd, int dark)
{
	io_service_t root = IOServiceGetMatchingService(0, IOServiceMatching("IOPMrootDomain"));
	io_connect_t pm = IOPMFindPowerManagement(MACH_PORT_NULL);
	long long before = sysctl_number("kern.hackios.sleeps");
	uint64_t a0, a1, c0, c1;
	char last[160] = "";
	size_t size = sizeof(last);
	kern_return_t kr = KERN_SUCCESS;
	unsigned off = 0, cpus = 0;
	double cont, mach;
	const char *why = NULL, *by = powerd ? "rtc" : dark ? "timer" : "pwrkey";

	if (root == MACH_PORT_NULL || pm == MACH_PORT_NULL) {
		printf("t-sleep: FAIL n=%d: no root domain\n", number);
		return 1;
	}
	if (powerd) {
		/* through powerd, which owns the root domain's "wake" and writes its own schedule there before a sleep */
		int value = seconds;
		CFNumberRef number_ref = CFNumberCreate(NULL, kCFNumberIntType, &value);
		CFDateRef when = CFDateCreate(NULL, CFAbsoluteTimeGetCurrent() + seconds);

		kr = IOPMSchedulePowerEvent(when, CFSTR("com.hackios.power_test"), CFSTR("wake"));
		CFRelease(when);
		if (kr != kIOReturnSuccess) {
			kr = registry_set_property(root, CFSTR("wake"), number_ref);    /* (without powerd: the root domain itself) */
		}
		CFRelease(number_ref);
	} else {
		long long timer = seconds;
		int key = !dark;

		sysctlbyname("kern.hackios.sleep_timer_key", NULL, NULL, &key, sizeof(key));
		if (sysctlbyname("kern.hackios.sleep_timer", NULL, NULL, &timer, sizeof(timer)) != 0) {
			printf("t-sleep: FAIL n=%d: no kern.hackios.sleep_timer (a kernel without it: use `powerd`)\n", number);
			return 1;
		}
	}
	a0 = mach_absolute_time();
	c0 = mach_continuous_time();
	IOPMSleepSystem(pm);
	/* entering takes a second when iOS allows it; an assertion that forbids it does not go away by waiting for it */
	for (int i = 0; i < 50 * (seconds + 12) && sysctl_number("kern.hackios.sleeps") == before; i++) {
		usleep(20000);          /* (the kernel counts the sleep when it is over: this ends the moment it is) */
	}
	a1 = mach_absolute_time();
	c1 = mach_continuous_time();
	sysctlbyname("kern.hackios.sleep_last", last, &size, NULL, 0);
	mach = seconds_of(a1 - a0);
	cont = seconds_of(c1 - c0);
	if (sysctl_number("kern.hackios.sleeps") == before) {
		why = "NOT ASLEEP: no sleep counted (the system refused it, or never got there)";
	} else if (strstr(last, by) == NULL) {
		why = "woken by something else than what was asked for";
	} else if (cont < seconds - 0.3 || cont > seconds + 1.5) {
		why = "the continuous clock did not run the time asked for";
	} else if (mach > 2.0) {
		why = "mach_absolute_time() did not stand still";
	}
	for (const char *at = strstr(last, ", "); at != NULL && cpus == 0; at = strstr(at + 1, ", ")) {
		sscanf(at, ", %u of %u cores off", &off, &cpus);
	}
	if (why == NULL && (cpus == 0 || off + 2 < cpus)) {
		why = "the cores were not switched off";
	}
	{
		char woken[16] = "?";

		if (strstr(last, "woken by ") != NULL) {
			sscanf(strstr(last, "woken by ") + 9, "%15[a-z_]", woken);
		}
		printf("t-sleep: %s n=%d want=%d cont=%.2f mach=%.2f by=%s cores_off=%u/%u%s%s\n", why ? "FAIL" : "PASS", number,
		    seconds, cont, mach, woken, off, cpus, why ? ": " : "", why ? why : "");
	}
	IOServiceClose(pm);
	IOObjectRelease(root);
	(void)kr;
	return why != NULL;
}

int
main(int argc, char **argv)
{
	if (argc >= 2 && strcmp(argv[1], "status") == 0) {
		status();
		return 0;
	}
	if (argc >= 3 && strcmp(argv[1], "display") == 0) {
		return display(strcmp(argv[2], "on") == 0);
	}
	if (argc >= 2 && strcmp(argv[1], "backlight") == 0) {
		/* what the panel has been told: its value, and how many writes its DSI link took (hk_power.c) */
		printf("t-backlight: %s value %lld writes %lld\n", argc >= 3 ? argv[2] : "now", sysctl_number("kern.hackios.backlight"),
		    sysctl_number("kern.hackios.backlight_sent"));
		fflush(stdout);
		return 0;
	}
	if (argc >= 3 && strcmp(argv[1], "kprof") == 0) {
		/* kprof start | kprof LABEL: the kernel's sampler (kern.hackios.kprof_ctl, kern.hackios.kprof): start, or stop and say */
		if (strcmp(argv[2], "start") == 0) {
			int one = 1;

			sysctlbyname("kern.hackios.kprof_ctl", NULL, NULL, &one, sizeof(one));
		} else {
			char text[2048] = "";
			size_t size = sizeof(text);

			sysctlbyname("kern.hackios.kprof", text, &size, NULL, 0);
			printf("t-kprof: %s %s\n", argv[2], text);
		}
		fflush(stdout);
		return 0;
	}
	if (argc >= 2 && strcmp(argv[1], "top") == 0) {
		/* top [LABEL]: processor time by process since the last "top" (the first one only starts the count) */
		char text[480] = "";
		size_t size = sizeof(text);

		sysctlbyname("kern.hackios.top", text, &size, NULL, 0);
		printf("t-top: %s %s\n", argc >= 3 ? argv[2] : "now", text);
		fflush(stdout);
		return 0;
	}
	if (argc >= 2 && strcmp(argv[1], "lat") == 0) {
		char text[96] = "";
		size_t size = sizeof(text);

		sysctlbyname("kern.hackios.memlat", text, &size, NULL, 0);
		printf("t-lat: %s %s\n", argc >= 3 ? argv[2] : "now", text);
		fflush(stdout);
		return 0;
	}
	if (argc >= 2 && strcmp(argv[1], "cpu") == 0) {
		char text[256] = "";
		size_t size = sizeof(text);

		sysctlbyname("kern.hackios.cpuspeed", text, &size, NULL, 0);
		printf("t-cpu: %s %s\n", argc >= 3 ? argv[2] : "now", text);
		fflush(stdout);
		return 0;
	}
	if (argc >= 2 && strcmp(argv[1], "mem") == 0) {
		/* what all the cores together get writing memory, now (kern.hackios.membench runs the test: about a second) */
		printf("t-mem: %s %lld MB/s\n", argc >= 3 ? argv[2] : "now", sysctl_number("kern.hackios.membench"));
		fflush(stdout);
		return 0;
	}
	if (argc >= 4 && strcmp(argv[1], "waitdisplay") == 0) {
		/* waitdisplay on|off SECS: until the panel is lit (or dark), no longer than SECS (a fixed wait after a wake is
		   either too long or too short) */
		int want = strcmp(argv[2], "on") == 0;
		double max = atof(argv[3]);
		uint64_t start = mach_absolute_time();
		double took = 0;

		while ((sysctl_number("kern.hackios.display_on") == 1) != want && (took = seconds_of(mach_absolute_time() - start)) < max) {
			usleep(20000);
		}
		took = seconds_of(mach_absolute_time() - start);
		printf("t-display: %s %s after %.2f s\n", argv[2], (sysctl_number("kern.hackios.display_on") == 1) == want ? "reached" : "NOT reached", took);
		fflush(stdout);
		return (sysctl_number("kern.hackios.display_on") == 1) != want;
	}
	if (argc >= 3 && strcmp(argv[1], "sleep") == 0) {
		int seconds = atoi(argv[2]), times = argc >= 4 && argv[3][0] >= '0' && argv[3][0] <= '9' ? atoi(argv[3]) : 1, failed = 0;
		int powerd = argc >= 4 && strcmp(argv[argc - 1], "powerd") == 0, dark = argc >= 4 && strcmp(argv[argc - 1], "dark") == 0;

		for (int i = 0; i < times; i++) {
			failed += sleep_once(seconds, i + 1, powerd, dark);
			fflush(stdout);
		}
		printf("t-sleeps: %s %d of %d came back as asked\n", failed ? "FAIL" : "PASS", times - failed, times);
		fflush(stdout);
		return failed != 0;
	}
	fprintf(stderr, "usage: power_test status | backlight [LABEL] | mem [LABEL] | display off|on | waitdisplay on|off SECONDS | sleep SECONDS [TIMES] [powerd|dark]\n");
	return 2;
}
