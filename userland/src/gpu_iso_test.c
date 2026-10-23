/*
 * gpu_iso_test: whether one program's GPU commands reach another program's GPU memory
 * (iokit/Kernel/HKGPU.cpp, boot-arg gpu_pt). Two connections to HKGPU are two clients, each
 * with page tables of its own, exactly as two programs are. Every check is made with the
 * 3D core's command processor itself (CP_MEM_WRITE, CP_MEM_TO_MEM from an indirect buffer, as a
 * program's commands are run), and every one has a control that must work.
 *
 *   own        B writes its own buffer                                  must land
 *   write      B writes A's buffer at A's address                       must not land; B's faults + 1
 *   read       B copies A's buffer into its own                         must not copy
 *   ring       B copies the kernel's ring (0x01000000) into its own     must not copy (privileged memory)
 *   fence      B writes the kernel's fence page (0x01200040)            must not land; later waits still work
 *   back       A writes its own buffer after B ran                      must land (the switch back)
 *   gone       B closes; C opens, gets addresses, writes its own        must land
 *   protect    (with the argument "protect") B writes a register the kernel keeps for itself
 *              (RBBM_SECVID_TSB_CNTL): the core stops on it, the kernel resets it
 *              (gpu_recover), and A's next write lands
 *
 * Prints one line per check, "PASS" or "FAIL", and exits 1 if any failed.
 */
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <IOKit/IOKitLib.h>

/* Linux's msm_drm.h, as iokit/Kernel/HKGPU.cpp takes them; the request is the selector */
#define GET_PARAM       0x40
#define GEM_NEW         0x42
#define GEM_INFO        0x43
#define GEM_SUBMIT      0x46
#define WAIT_FENCE      0x47
#define PARAM_FAULTS    0x09
#define INFO_IOVA       1
#define SUBMIT_CMD_BUF  1

struct param { uint32_t pipe, param; uint64_t value; uint32_t len, pad; };
struct gem_new { uint64_t size; uint32_t flags, handle; };
struct gem_info { uint32_t handle, info; uint64_t value; uint32_t len, pad; };
struct submit_cmd { uint32_t type, submit_idx, submit_offset, size, pad, nr_relocs; uint64_t relocs; };
struct submit_bo { uint32_t flags, handle; uint64_t presumed; };
struct submit {
	uint32_t flags, fence, nr_bos, nr_cmds;
	uint64_t bos, cmds;
	int32_t fence_fd;
	uint32_t queueid;
	uint64_t in_syncobjs, out_syncobjs;
	uint32_t nr_in_syncobjs, nr_out_syncobjs, syncobj_stride, pad;
};
struct wait_fence { uint32_t fence, flags; int64_t seconds, nanoseconds; uint32_t queueid; };

#define RING_ADDRESS    0x01000000ull   /* bsd/dev/hk_adreno.c: CORE_RING_ADDRESS */
#define FENCE_ADDRESS   0x01200040ull   /* ... CORE_SCRATCH_ADDRESS + GPU_FENCE_AT */
#define CP_MEM_WRITE    0x3d
#define CP_MEM_TO_MEM   0x73
#define CP_WAIT_MEM_WRITES 0x12
#define RBBM_SECVID_TSB_CNTL 0xf803

struct client {
	io_connect_t connection;
	uint32_t data, commands;                /* handles: a buffer to write, one of commands */
	uint64_t data_address, commands_address;
	volatile uint32_t *words, *program;     /* both as this processor sees them */
};

static int failures;

static uint32_t
odd_parity(uint32_t v)
{
	return 0x9669 >> (0xf & (v ^ v >> 4 ^ v >> 8 ^ v >> 12 ^ v >> 16 ^ v >> 20 ^ v >> 24 ^ v >> 28)) & 1;
}

static uint32_t
packet(uint32_t command, uint32_t words)
{
	return 7u << 28 | words | odd_parity(words) << 15 | command << 16 | odd_parity(command) << 23;
}

static uint32_t
register_write(uint32_t number)
{
	return 4u << 28 | 1 | odd_parity(1) << 7 | number << 8 | odd_parity(number) << 27;
}

static kern_return_t
call(struct client *c, uint32_t request, void *data, size_t size)
{
	size_t back = size;

	return IOConnectCallStructMethod(c->connection, request, data, size, data, &back);
}

static uint32_t
buffer(struct client *c, uint64_t size, uint64_t *address, volatile uint32_t **memory)
{
	struct gem_new made = { .size = size };
	struct gem_info info;
	mach_vm_address_t at = 0;
	mach_vm_size_t length = 0;

	if (call(c, GEM_NEW, &made, sizeof(made)) != KERN_SUCCESS) {
		return 0;
	}
	info = (struct gem_info){ .handle = made.handle, .info = INFO_IOVA };
	if (call(c, GEM_INFO, &info, sizeof(info)) != KERN_SUCCESS ||
	    IOConnectMapMemory64(c->connection, made.handle, mach_task_self(), &at, &length, kIOMapAnywhere) != KERN_SUCCESS) {
		return 0;
	}
	*address = info.value;
	*memory = (volatile uint32_t *)at;
	return made.handle;
}

static int
open_client(struct client *c, const char *name)
{
	io_service_t service = IOServiceGetMatchingService(0, IOServiceMatching("HKGPU"));

	memset(c, 0, sizeof(*c));
	if (service == 0 || IOServiceOpen(service, mach_task_self(), 0, &c->connection) != KERN_SUCCESS) {
		printf("gpu_iso_test: %s: HKGPU cannot be opened\n", name);
		return 0;
	}
	IOObjectRelease(service);
	c->data = buffer(c, 65536, &c->data_address, &c->words);
	c->commands = buffer(c, 4096, &c->commands_address, &c->program);
	if (c->data == 0 || c->commands == 0) {
		printf("gpu_iso_test: %s: no buffers\n", name);
		return 0;
	}
	printf("gpu_iso_test: client %s: its buffer at %#llx, its commands at %#llx\n", name, c->data_address,
	    c->commands_address);
	return 1;
}

static uint64_t
faults(struct client *c)
{
	struct param p = { .param = PARAM_FAULTS };

	return call(c, GET_PARAM, &p, sizeof(p)) == KERN_SUCCESS ? p.value : ~0ull;
}

/* The commands in c->program, `words` of them, run; whether they were done within two seconds. */
static int
run(struct client *c, uint32_t words)
{
	struct submit_bo bos[2] = { { .handle = c->commands }, { .handle = c->data } };
	struct submit_cmd cmd = { .type = SUBMIT_CMD_BUF, .submit_idx = 0, .size = 4 * words };
	struct submit s = { .nr_bos = 2, .nr_cmds = 1, .bos = (uintptr_t)bos, .cmds = (uintptr_t)&cmd };
	struct wait_fence w;
	mach_timebase_info_data_t base;
	uint64_t ns;

	if (call(c, GEM_SUBMIT, &s, sizeof(s)) != KERN_SUCCESS) {
		return 0;
	}
	mach_timebase_info(&base);
	ns = mach_continuous_time() * base.numer / base.denom + 2000000000ull;
	w = (struct wait_fence){ .fence = s.fence, .seconds = ns / 1000000000ull, .nanoseconds = ns % 1000000000ull };
	return call(c, WAIT_FENCE, &w, sizeof(w)) == KERN_SUCCESS;
}

static uint32_t
mem_write(struct client *c, uint32_t at, uint64_t address, uint32_t value)
{
	c->program[at++] = packet(CP_MEM_WRITE, 3);
	c->program[at++] = (uint32_t)address;
	c->program[at++] = (uint32_t)(address >> 32);
	c->program[at++] = value;
	return at;
}

static uint32_t
mem_copy(struct client *c, uint32_t at, uint64_t to, uint64_t from)
{
	c->program[at++] = packet(CP_MEM_TO_MEM, 5);
	c->program[at++] = 1u << 30;            /* (after the writes before it) */
	c->program[at++] = (uint32_t)to;
	c->program[at++] = (uint32_t)(to >> 32);
	c->program[at++] = (uint32_t)from;
	c->program[at++] = (uint32_t)(from >> 32);
	return at;
}

static uint32_t
settle(struct client *c, uint32_t at)
{
	c->program[at++] = packet(CP_WAIT_MEM_WRITES, 0);
	return at;
}

static void
check(const char *name, int pass, const char *what)
{
	printf("gpu_iso_test: %-8s %s: %s\n", name, pass ? "PASS" : "FAIL", what);
	failures += !pass;
}

int
main(int argc, char **argv)
{
	struct client a, b, c;
	uint64_t before;
	int ran;

	if (!open_client(&a, "A") || !open_client(&b, "B")) {
		return 1;
	}
	a.words[0] = 0x11111111;
	a.words[1] = 0x22222222;
	b.words[0] = b.words[1] = b.words[2] = b.words[3] = 0;
	before = faults(&b);
	printf("gpu_iso_test: B's faults so far: %llu\n", before);

	ran = run(&b, settle(&b, mem_write(&b, 0, b.data_address, 0xb0b0b0b0)));
	check("own", ran && b.words[0] == 0xb0b0b0b0, "B's write to its own buffer landed");

	ran = run(&b, settle(&b, mem_write(&b, 0, a.data_address, 0xdeadbeef)));
	check("write", ran && a.words[0] == 0x11111111, "B's write to A's buffer did not land");
	check("faults", faults(&b) > before, "the SMMU's refusal is counted against B");
	printf("gpu_iso_test: (B's faults now %llu; A's %llu)\n", faults(&b), faults(&a));

	ran = run(&b, settle(&b, mem_copy(&b, 0, b.data_address + 4, a.data_address + 4)));
	check("read", ran && b.words[1] != 0x22222222, "B could not copy A's buffer into its own");

	b.words[2] = 0x5a5a5a5a;
	ran = run(&b, settle(&b, mem_copy(&b, 0, b.data_address + 8, RING_ADDRESS)));
	printf("gpu_iso_test: (B read %#x from the kernel's ring)\n", b.words[2]);
	check("ring", ran && (b.words[2] == 0x5a5a5a5a || b.words[2] == 0), "B could not read the kernel's ring");

	ran = run(&b, settle(&b, mem_write(&b, 0, FENCE_ADDRESS, 0x7fffffff)));
	check("fence", ran, "B's write to the kernel's fence page did not end the kernel's waits");
	ran = run(&b, settle(&b, mem_write(&b, 0, b.data_address + 12, 0xf00df00d)));
	check("fence", ran && b.words[3] == 0xf00df00d, "and B's next submission is waited for, and lands");

	ran = run(&a, settle(&a, mem_write(&a, 0, a.data_address + 8, 0xa0a0a0a0)));
	check("back", ran && a.words[2] == 0xa0a0a0a0 && a.words[0] == 0x11111111, "A's own write landed after B's");

	IOServiceClose(b.connection);
	if (!open_client(&c, "C")) {
		return 1;
	}
	ran = run(&c, settle(&c, mem_write(&c, 0, c.data_address, 0xc0c0c0c0)));
	check("gone", ran && c.words[0] == 0xc0c0c0c0, "after B is gone, C's write to its own buffer landed");

	if (argc > 1 && strcmp(argv[1], "protect") == 0) {
		c.program[0] = register_write(RBBM_SECVID_TSB_CNTL);
		c.program[1] = 0;
		ran = run(&c, 2);
		check("protect", !ran, "C's write to a register the kernel keeps stopped the core (it was not done)");
		ran = run(&a, settle(&a, mem_write(&a, 0, a.data_address + 16, 0xa1a1a1a1)));
		check("protect", ran && a.words[4] == 0xa1a1a1a1, "after the reset, A's next write landed");
	}
	IOServiceClose(c.connection);
	IOServiceClose(a.connection);
	printf("gpu_iso_test: %s\n", failures ? "FAILED" : "all passed");
	return failures != 0;
}
