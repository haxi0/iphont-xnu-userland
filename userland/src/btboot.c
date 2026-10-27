/*
 * btboot: the boot helper of this board's Bluetooth controller, in the place iOS has BlueTool.
 *
 * bluetoothd does not power or load its controller itself: it asks the Mach service com.apple.BlueTool
 * to run one of three "scripts" -- init (power on), boot (load the chip's firmware and settings),
 * deepsleep (power off) -- and waits for the answer. Apple's BlueTool does that for the Broadcom chips of
 * iPhones (its scripts are built in, chosen by the phone's platform; scripts of one's own in /etc/bluetool
 * are taken only on internal builds). The OnePlus 8's controller is a QCA6390, whose firmware the kernel
 * loads (bsd/dev/hk_bt.c, the way Linux's btqca.c does), so for this board the three are:
 *   init       let go of /dev/btpoweroff: the kernel powers the chip and brings it up;
 *   deepsleep  hold /dev/btpoweroff open: the kernel powers the chip off;
 *   boot       wait until the kernel says the controller is up (sysctl kern.hackios.bt, 10 s at most).
 * The request: { kMsgId "RunBuiltinScript", kMsgArgs { script, btaddress } }; the answer bluetoothd reads:
 * { result (0: done), firmware }. Anything else is answered with result 1 and said on the console.
 * The job: com.hackios.bluetool (build.sh, BLUETOOTH=1), which holds the Mach service name.
 */
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

/* (the SDK here has no xpc/xpc.h: the part of libxpc's API this uses, as libSystem exports it) */
typedef void *xpc_object_t;
typedef xpc_object_t xpc_connection_t;
typedef const struct _xpc_type_s *xpc_type_t;
typedef void (^xpc_handler_t)(xpc_object_t object);
extern const struct _xpc_type_s _xpc_type_dictionary, _xpc_type_connection;
#define XPC_TYPE_DICTIONARY (&_xpc_type_dictionary)
#define XPC_TYPE_CONNECTION (&_xpc_type_connection)
#define XPC_CONNECTION_MACH_SERVICE_LISTENER (1 << 0)
extern xpc_connection_t xpc_connection_create_mach_service(const char *name, dispatch_queue_t queue, uint64_t flags);
extern void xpc_connection_set_event_handler(xpc_connection_t connection, xpc_handler_t handler);
extern void xpc_connection_resume(xpc_connection_t connection);
extern void xpc_connection_send_message(xpc_connection_t connection, xpc_object_t message);
extern xpc_type_t xpc_get_type(xpc_object_t object);
extern const char *xpc_dictionary_get_string(xpc_object_t dictionary, const char *key);
extern xpc_object_t xpc_dictionary_get_value(xpc_object_t dictionary, const char *key);
extern xpc_object_t xpc_dictionary_create_reply(xpc_object_t original);
extern void xpc_dictionary_set_int64(xpc_object_t dictionary, const char *key, int64_t value);
extern void xpc_dictionary_set_string(xpc_object_t dictionary, const char *key, const char *string);
extern void xpc_release(xpc_object_t object);

static int poweroff_fd = -1;

/* the first word of kern.hackios.bt ("state up; ..."), or "" */
static void
controller_state(char *state, size_t size)
{
	char text[1024] = "";
	size_t length = sizeof(text) - 1;

	state[0] = 0;
	if (sysctlbyname("kern.hackios.bt", text, &length, NULL, 0) == 0 && strncmp(text, "state ", 6) == 0) {
		size_t n = strcspn(text + 6, ";");

		if (n >= size) {
			n = size - 1;
		}
		memcpy(state, text + 6, n);
		state[n] = 0;
	}
}

static int
run(const char *script)
{
	char state[32];

	if (strcmp(script, "init") == 0) {
		if (poweroff_fd >= 0) {
			close(poweroff_fd);
			poweroff_fd = -1;
		}
		return 0;
	}
	if (strcmp(script, "deepsleep") == 0) {
		if (poweroff_fd < 0) {
			poweroff_fd = open("/dev/btpoweroff", O_RDONLY);
		}
		if (poweroff_fd < 0) {
			fprintf(stderr, "btboot: /dev/btpoweroff: %s\n", strerror(errno));
			return 1;
		}
		return 0;
	}
	if (strcmp(script, "boot") == 0) {
		for (int i = 0; i < 100; i++) {
			controller_state(state, sizeof(state));
			if (strcmp(state, "up") == 0) {
				return 0;
			}
			if (strcmp(state, "FAILED") == 0 || strcmp(state, "off") == 0 || state[0] == 0) {
				break;
			}
			usleep(100000);
		}
		fprintf(stderr, "btboot: boot: the controller is %s\n", state[0] ? state : "not there (no bt=1)");
		return 1;
	}
	fprintf(stderr, "btboot: script \"%s\" is not one of this board's\n", script);
	return 1;
}

static void
handle(xpc_connection_t peer, xpc_object_t message)
{
	const char *id, *script = NULL;
	xpc_object_t args, reply;
	int result = 1;

	if (xpc_get_type(message) != XPC_TYPE_DICTIONARY) {
		return;
	}
	id = xpc_dictionary_get_string(message, "kMsgId");
	args = xpc_dictionary_get_value(message, "kMsgArgs");
	if (args == NULL || xpc_get_type(args) != XPC_TYPE_DICTIONARY) {
		args = message;
	}
	if (id != NULL && strcmp(id, "RunBuiltinScript") == 0) {
		script = xpc_dictionary_get_string(args, "script");
		result = script != NULL ? run(script) : 1;
		fprintf(stderr, "btboot: %s: %s\n", script ? script : "(no script)", result == 0 ? "done" : "FAILED");
	} else {
		fprintf(stderr, "btboot: request %s not known: answered with a failure\n", id ? id : "(none)");
	}
	reply = xpc_dictionary_create_reply(message);
	if (reply != NULL) {
		xpc_dictionary_set_int64(reply, "result", result);
		xpc_dictionary_set_string(reply, "firmware", "QCA6390 (bsd/dev/hk_bt.c)");
		xpc_dictionary_set_string(reply, "output", "");
		xpc_connection_send_message(peer, reply);
		xpc_release(reply);
	}
}

int
main(void)
{
	xpc_connection_t listener = xpc_connection_create_mach_service("com.apple.BlueTool", NULL,
	    XPC_CONNECTION_MACH_SERVICE_LISTENER);

	xpc_connection_set_event_handler(listener, ^(xpc_object_t peer) {
		if (xpc_get_type(peer) != XPC_TYPE_CONNECTION) {
			return;
		}
		xpc_connection_set_event_handler(peer, ^(xpc_object_t message) {
			handle(peer, message);
		});
		xpc_connection_resume(peer);
	});
	xpc_connection_resume(listener);
	dispatch_main();
}
