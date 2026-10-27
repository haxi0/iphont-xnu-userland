/*
 * QCA6390 board integration for iOS 16.5 bluetoothd (20F66).
 *
 * The controller's capabilities and replies stay unchanged. iOS's fixed HCI init list ends with
 * optional Write Default Erroneous Data Reporting, unsupported by this controller. Replace only that
 * init step with capability setup for this board, then advance the existing init state machine.
 * The stack uses legacy LE discovery. Omit its periodic-advertiser query too: issuing that extended
 * command makes the controller reject all later legacy advertising/scanning commands until reset.
 * bluetoothd also chooses Broadcom vendor commands from the emulated iPhone model. Select its generic
 * vendor descriptor and clear its vendor capability table: none of those Broadcom commands belongs to this Qualcomm controller. Standard HCI
 * discovery, security, ACL, SCO and profile handling remain the daemon's own.
 *
 * Only a private function-pointer table changes. UUID, expected pointers, and instruction words must
 * match before any write. No release status, entitlement, controller identity or HCI result is changed.
 */
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>

#define BT_INIT_SLOT 0x1007fbde0ULL
#define BT_REPORT_STEP 0x10010b970ULL
#define BT_PERIODIC_SLOT 0x1007fbdb0ULL
#define BT_PERIODIC_STEP 0x10010b3bcULL
#define BT_VENDOR_GET 0x10029a9d4ULL
#define BT_FACTORY_SLOT 0x100805f58ULL
#define BT_FACTORY_INVOKE 0x10029aa14ULL
#define BT_VENDOR_INSTANCE 0x10085c9f0ULL
#define BT_INIT_ADVANCE 0x1001c204cULL
#define BT_INIT_CURSOR 0x10084f588ULL
#define BT_VENDOR_MULTI_ADV 0xa0
#define BT_VENDOR_KIND 0x260
#define BT_VENDOR_CAPS 0xa8
#define BT_VENDOR_COUNT 0xdc
static const uint8_t bluetoothd_uuid[16] = {
	0x96,0x39,0x30,0xf1,0xd5,0x65,0x32,0xb5,0x91,0xd6,0x43,0xaf,0x71,0xe2,0xc8,0x5a
};
static intptr_t image_slide;

static void
configure_vendor(void)
{
	uint8_t *vendor = *(uint8_t **)(BT_VENDOR_INSTANCE + image_slide);

	/* Kind zero is the generic descriptor: the later capability initializer leaves all VSCs absent. */
	*(uint32_t *)(vendor + BT_VENDOR_KIND) = 0;
	vendor[BT_VENDOR_MULTI_ADV] = 0;
	memset(vendor + BT_VENDOR_CAPS, 0, BT_VENDOR_COUNT);
	fprintf(stderr, "bt-board: QCA6390: Broadcom VSCs/multi-advertising disabled; unsupported optional 0c5b init step omitted\n");
}

/* Run at the daemon's normal lazy initialization point, after its other singletons are ready. */
static void
qca_factory(void *block)
{
	void (*original)(void *) = (void *)(BT_FACTORY_INVOKE + image_slide);
	original(block);
	/* Suppress proprietary multi-advertising before stack customization caches this flag. */
	uint8_t *vendor = *(uint8_t **)(BT_VENDOR_INSTANCE + image_slide);
	vendor[BT_VENDOR_MULTI_ADV] = 0;
	fprintf(stderr, "bt-board: QCA6390: proprietary multi-advertising absent at factory creation\n");
}

static int
replace_pointer(uintptr_t *slot, uintptr_t value)
{
	vm_address_t page = (vm_address_t)slot & ~(vm_address_t)(vm_page_size - 1);
	kern_return_t result = vm_protect(mach_task_self(), page, vm_page_size, FALSE,
	    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
	if (result != KERN_SUCCESS) {
		fprintf(stderr, "bt-board: private table: vm_protect %d\n", result);
		return 0;
	}
	*slot = value;
	result = vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
	if (result != KERN_SUCCESS) {
		fprintf(stderr, "bt-board: read-only restore failed %d\n", result);
	}
	return 1;
}

static void
qca_legacy_init(void)
{
	void (*advance)(void *) = (void *)(BT_INIT_ADVANCE + image_slide);
	fprintf(stderr, "bt-board: legacy LE command family selected; periodic-advertiser query omitted\n");
	advance(*(void **)(BT_INIT_CURSOR + image_slide));
}

static void
qca_capabilities(void)
{
	void (*advance)(void *) = (void *)(BT_INIT_ADVANCE + image_slide);
	configure_vendor();
	advance(*(void **)(BT_INIT_CURSOR + image_slide));
}

__attribute__((constructor)) static void
configure_board(void)
{
	char state[1024];
	size_t size = sizeof(state);
	const struct mach_header_64 *header = NULL;
	int matched = 0;

	/* Injected libraries can precede the executable in this dyld image list. */
	for (uint32_t image = 0; image < _dyld_image_count() && !matched; image++) {
		header = (const void *)_dyld_get_image_header(image);
		const struct load_command *lc = (const void *)(header + 1);
		for (uint32_t i = 0; i < header->ncmds; i++) {
			if (lc->cmd == LC_UUID && lc->cmdsize == sizeof(struct uuid_command)) {
				matched = memcmp(((const struct uuid_command *)lc)->uuid, bluetoothd_uuid, 16) == 0;
			}
			lc = (const void *)((const char *)lc + lc->cmdsize);
		}
		if (matched) {
			image_slide = _dyld_get_image_vmaddr_slide(image);
		}
	}

	if (sysctlbyname("kern.bootargs", state, &size, NULL, 0) != 0) {
		return;
	}
	state[sizeof(state) - 1] = 0;
	if (strstr(state, "board=instantnoodle ") == NULL) {
		fprintf(stderr, "bt-board: instantnoodle board not confirmed; left untouched\n");
		return;
	}
	if (!matched) {
		fprintf(stderr, "bt-board: bluetoothd UUID not found; left untouched\n");
		return;
	}
	uintptr_t *slot = (void *)(BT_INIT_SLOT + image_slide);
	uintptr_t *factory = (void *)(BT_FACTORY_SLOT + image_slide);
	uintptr_t *periodic = (void *)(BT_PERIODIC_SLOT + image_slide);
	if (*slot != BT_REPORT_STEP + image_slide || slot[1] != 0 ||
	    *factory != BT_FACTORY_INVOKE + image_slide ||
	    *periodic != BT_PERIODIC_STEP + image_slide ||
	    *(uint32_t *)(BT_PERIODIC_STEP + image_slide) != 0xd100c3ff ||
	    *(uint32_t *)(BT_INIT_ADVANCE + image_slide + 12) != 0xaa0003f3 ||
	    *(uint32_t *)(BT_VENDOR_GET + image_slide) != 0xa9bf7bfd ||
	    *(uint32_t *)(BT_REPORT_STEP + image_slide) != 0xd100c3ff) {
		fprintf(stderr, "bt-board: layout mismatch uuid=%d slot=%#llx expected=%#llx next=%#llx get=%08x step=%08x; left untouched\n",
		    matched, (unsigned long long)*slot, (unsigned long long)(BT_REPORT_STEP + image_slide),
		    (unsigned long long)slot[1], *(uint32_t *)(BT_VENDOR_GET + image_slide),
		    *(uint32_t *)(BT_REPORT_STEP + image_slide));
		return;
	}
	if (!replace_pointer(factory, (uintptr_t)qca_factory)) {
		return;
	}
	if (!replace_pointer(periodic, (uintptr_t)qca_legacy_init)) {
		replace_pointer(factory, BT_FACTORY_INVOKE + image_slide);
		return;
	}
	if (!replace_pointer(slot, (uintptr_t)qca_capabilities)) {
		replace_pointer(periodic, BT_PERIODIC_STEP + image_slide);
		replace_pointer(factory, BT_FACTORY_INVOKE + image_slide);
		return;
	}
	fprintf(stderr, "bt-board: QCA6390 factory/init installed\n");
}
