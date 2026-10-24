/*
 * /dev/hk-touch: touch samples on their way from whoever produces them to the
 * program that turns them into HID events for the window server.
 *
 * iOS expects touches from Apple's multitouch driver through IOHIDFamily; neither exists
 * here. Instead a producer -- a kernel driver for a real touch controller or for an emulated
 * pointer, or a user program replaying a script -- posts samples, and code of ours running
 * inside backboardd reads them and synthesizes the events. Both sides use the same record.
 *
 *   write(): one or more records (time is ignored; the kernel stamps them)
 *   read():  blocks until there is at least one record, returns as many whole ones as fit
 */
#ifndef _SYS_HK_INPUT_H_
#define _SYS_HK_INPUT_H_

#include <stdint.h>

#define HK_TOUCH_DEVICE    "/dev/hk-touch"

#define HK_TOUCH_DOWN      1       /* a finger has landed */
#define HK_TOUCH_MOVE      2
#define HK_TOUCH_UP        3       /* it has lifted (x, y: where it was last) */
#define HK_BUTTON_DOWN     4       /* a hardware button; `finger` says which (HK_BUTTON_*), x and y are 0 */
#define HK_BUTTON_UP       5

#define HK_BUTTON_LOCK     1       /* the side button */
#define HK_BUTTON_VOLUME_UP 2
#define HK_BUTTON_VOLUME_DOWN 3
#define HK_BUTTON_HOME     4
#define HK_SLIDER          6       /* a three-position switch (the OnePlus 8's alert slider) is now at `finger` (HK_SLIDER_*) */

#define HK_SLIDER_DOWN     1       /* ring */
#define HK_SLIDER_MIDDLE   2       /* silent */
#define HK_SLIDER_UP       3       /* Do Not Disturb */

struct hk_touch {
	uint64_t time;                  /* mach_absolute_time() when the sample was posted */
	uint32_t type;                  /* HK_TOUCH_* or HK_BUTTON_DOWN/UP */
	uint32_t finger;                /* which finger, from 0 (or which button) */
	uint32_t x, y;                  /* position on the display, 0 = left/top edge, 65535 = right/bottom edge */
};

#ifdef KERNEL
/* For drivers; not from interrupt context. */
void hk_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y);
void hk_input_init(void);
void hk_virtio_input_init(void);   /* boot-arg virtio_input=1: QEMU's virtio pointer and keyboard */
void hk_goodix_init(void);         /* boot-arg touchscreen=1: the phone's touch screen */
void hk_cpufreq_init(void);        /* boot-arg cpufreq=1: the phone's processor at full speed */
#endif

#endif /* _SYS_HK_INPUT_H_ */
