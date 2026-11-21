/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Just enough of the Linux kernel API, in userspace, for the upstream HD60 S
 * driver's hardware files to compile unmodified on macOS.
 *
 * Every <linux/...> and <media/...> header the driver includes is a one-line
 * stub that includes this file. USB goes to libusb, work queues to a pthread,
 * and V4L2 is reduced to the handful of structs and constants the control
 * plane touches. The implementation is in mac/kshim.c.
 */
#ifndef _KSHIM_H_
#define _KSHIM_H_

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- types ---------------------------------------------------------- */

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef u16 __le16;
typedef unsigned int gfp_t;
typedef uintptr_t dma_addr_t;

#define GFP_KERNEL	0
#define GFP_ATOMIC	0

#define KERNEL_VERSION(a, b, c)	(((a) << 16) + ((b) << 8) + (c))
#define LINUX_VERSION_CODE	KERNEL_VERSION(7, 0, 0)

/* ---- bits and arithmetic -------------------------------------------- */

#define BIT(n)			(1UL << (n))
#define GENMASK(h, l)		(((~0UL) << (l)) & (~0UL >> (63 - (h))))

/* lowest set bit of a mask, as a constant expression */
#define __kshim_lsb(mask)	((mask) & ~((mask) << 1))
#define FIELD_PREP(mask, val)	\
	(((__typeof__(mask))(val) * __kshim_lsb(mask)) & (mask))
#define FIELD_PREP_CONST(mask, val)	FIELD_PREP(mask, val)
#define FIELD_GET(mask, reg)	\
	((__typeof__(mask))(((reg) & (mask)) / __kshim_lsb(mask)))

#define BUILD_BUG_ON(e)		((void)sizeof(char[1 - 2 * !!(e)]))
#define BUILD_BUG_ON_ZERO(e)	0
#define static_assert		_Static_assert

#define ARRAY_SIZE(a)		(sizeof(a) / sizeof((a)[0]))

#define min(a, b)	({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a < _b ? _a : _b; })
#define max(a, b)	({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a > _b ? _a : _b; })
#define min_t(t, a, b)	({ t _a = (a); t _b = (b); _a < _b ? _a : _b; })
#define max_t(t, a, b)	({ t _a = (a); t _b = (b); _a > _b ? _a : _b; })
#define clamp(v, lo, hi)	min(max(v, lo), hi)
#define clamp_t(t, v, lo, hi)	min_t(t, max_t(t, v, lo), hi)
#undef abs
#define abs(x)		({ __typeof__(x) _x = (x); _x < 0 ? -_x : _x; })

#define div_u64(a, b)	((u64)(a) / (u32)(b))

static inline u16 get_unaligned_le16(const void *p)
{
	const u8 *b = p;

	return (u16)(b[0] | b[1] << 8);
}

static inline void *memset32(u32 *s, u32 v, size_t n)
{
	u32 *p = s;

	while (n--)
		*p++ = v;
	return s;
}

#define le16_to_cpu(x)		(x)
#define cpu_to_le16(x)		(x)

#define READ_ONCE(x)		(x)
#define WRITE_ONCE(x, v)	((x) = (v))

#define container_of(ptr, type, member)	\
	((type *)((char *)(ptr) - offsetof(type, member)))

/* ---- memory, time --------------------------------------------------- */

#define kzalloc(size, gfp)	calloc(1, (size))
#define kfree(p)		free(p)

void kshim_sleep_us(unsigned long us);
u64 kshim_now_ns(void);

#define msleep(ms)		kshim_sleep_us((unsigned long)(ms) * 1000UL)
#define usleep_range(lo, hi)	kshim_sleep_us(lo)
#define msecs_to_jiffies(ms)	((unsigned long)(ms))	/* a jiffy is 1 ms here */
#define ktime_get_ns()		kshim_now_ns()

/* ---- logging -------------------------------------------------------- */

struct device {
	int unused;
};

enum { KSHIM_ERR = 3, KSHIM_WARN = 4, KSHIM_INFO = 6, KSHIM_DBG = 7 };
extern int kshim_loglevel;	/* messages above this level are dropped */

/* Understands the kernel's %*ph extension; otherwise printf. */
void kshim_log(int level, const char *fmt, ...);

#define dev_err(dev, ...)	((void)(dev), kshim_log(KSHIM_ERR, __VA_ARGS__))
#define dev_warn(dev, ...)	((void)(dev), kshim_log(KSHIM_WARN, __VA_ARGS__))
#define dev_info(dev, ...)	((void)(dev), kshim_log(KSHIM_INFO, __VA_ARGS__))
#define dev_dbg(dev, ...)	((void)(dev), kshim_log(KSHIM_DBG, __VA_ARGS__))
#define dev_warn_ratelimited	dev_warn

/* ---- locks, atomics, lists ------------------------------------------ */

struct mutex {
	pthread_mutex_t m;
};

#define mutex_init(l)		pthread_mutex_init(&(l)->m, NULL)
#define mutex_lock(l)		pthread_mutex_lock(&(l)->m)
#define mutex_unlock(l)		pthread_mutex_unlock(&(l)->m)
#define lockdep_assert_held(l)	((void)(l))

typedef struct {
	pthread_mutex_t m;
} spinlock_t;

#define spin_lock_init(l)	pthread_mutex_init(&(l)->m, NULL)
#define spin_lock_irqsave(l, flags)	\
	do { (flags) = 0; pthread_mutex_lock(&(l)->m); } while (0)
#define spin_unlock_irqrestore(l, flags)	\
	do { (void)(flags); pthread_mutex_unlock(&(l)->m); } while (0)

typedef struct {
	atomic_int v;
} atomic_t;

#define atomic_set(a, i)	atomic_store(&(a)->v, (i))
#define atomic_read(a)		atomic_load(&(a)->v)
#define atomic_inc(a)		((void)atomic_fetch_add(&(a)->v, 1))
#define atomic_dec(a)		((void)atomic_fetch_sub(&(a)->v, 1))
#define atomic_dec_return(a)	(atomic_fetch_sub(&(a)->v, 1) - 1)

struct list_head {
	struct list_head *next, *prev;
};

static inline void INIT_LIST_HEAD(struct list_head *h)
{
	h->next = h;
	h->prev = h;
}

static inline bool list_empty(const struct list_head *h)
{
	return h->next == h;
}

static inline void list_add_tail(struct list_head *n, struct list_head *h)
{
	n->prev = h->prev;
	n->next = h;
	h->prev->next = n;
	h->prev = n;
}

static inline void list_del(struct list_head *n)
{
	n->prev->next = n->next;
	n->next->prev = n->prev;
	n->next = n;
	n->prev = n;
}

/* ---- work queues: one pthread per queue ------------------------------ */

struct workqueue_struct;

struct work_struct {
	void (*fn)(struct work_struct *work);
	struct workqueue_struct *wq;
	bool pending, running, canceling;
	u64 due_ns;
};

struct delayed_work {
	struct work_struct work;
};

struct workqueue_struct *alloc_ordered_workqueue(const char *name, unsigned int flags);
void destroy_workqueue(struct workqueue_struct *wq);
void kshim_init_work(struct work_struct *w, void (*fn)(struct work_struct *));
bool kshim_queue_work(struct workqueue_struct *wq, struct work_struct *w,
		      unsigned long delay_ms);
bool cancel_work_sync(struct work_struct *w);
void flush_work(struct work_struct *w);

#define INIT_WORK(w, f)			kshim_init_work((w), (f))
#define INIT_DELAYED_WORK(dw, f)	kshim_init_work(&(dw)->work, (f))
#define queue_work(wq, w)		kshim_queue_work((wq), (w), 0)
#define queue_delayed_work(wq, dw, j)	kshim_queue_work((wq), &(dw)->work, (j))
#define cancel_delayed_work_sync(dw)	cancel_work_sync(&(dw)->work)

/* ---- USB, on libusb -------------------------------------------------- */

struct libusb_device_handle;
struct usb_interface;
struct urb;

enum { USB_STATE_NOTATTACHED = 0, USB_STATE_CONFIGURED = 7 };
enum { USB_SPEED_FULL = 2, USB_SPEED_HIGH = 3, USB_SPEED_SUPER = 5 };

#define USB_DIR_OUT		0x00
#define USB_DIR_IN		0x80
#define USB_TYPE_VENDOR		0x40

struct usb_device {
	struct libusb_device_handle *h;
	int state;
	int speed;
	struct {
		__le16 idProduct;
	} descriptor;
	struct usb_interface *intfs[2];
	void *priv;			/* the macOS layer's own state */
};

struct usb_interface {
	struct device dev;
	struct usb_device *udev;
	void *intfdata;
	int ifnum;
};

struct usb_device_id {
	u16 idVendor, idProduct;
	u8 bInterfaceNumber;
};

#define USB_DEVICE_INTERFACE_NUMBER(vid, pid, num)	\
	.idVendor = (vid), .idProduct = (pid), .bInterfaceNumber = (num)

typedef struct {
	int event;
} pm_message_t;

struct usb_driver {
	const char *name;
	int (*probe)(struct usb_interface *intf, const struct usb_device_id *id);
	void (*disconnect)(struct usb_interface *intf);
	int (*suspend)(struct usb_interface *intf, pm_message_t message);
	int (*resume)(struct usb_interface *intf);
	int (*reset_resume)(struct usb_interface *intf);
	const struct usb_device_id *id_table;
};

/* The driver's one usb_driver, exported for the macOS layer to call into. */
extern struct usb_driver *const kshim_usb_driver;
#define module_usb_driver(drv)	struct usb_driver *const kshim_usb_driver = &(drv)

#define MODULE_DEVICE_TABLE(type, name)	_Static_assert(1, "")
#define MODULE_DESCRIPTION(s)		_Static_assert(1, "")
#define MODULE_AUTHOR(s)		_Static_assert(1, "")
#define MODULE_LICENSE(s)		_Static_assert(1, "")

#define interface_to_usbdev(intf)	((intf)->udev)
#define usb_get_dev(udev)		(udev)
#define usb_put_dev(udev)		((void)(udev))
#define usb_set_intfdata(intf, data)	((intf)->intfdata = (data))
#define usb_get_intfdata(intf)		((intf)->intfdata)
#define usb_ifnum_to_if(udev, n)	((udev)->intfs[(n)])
/* Both interfaces are claimed through libusb when the device is configured. */
#define usb_driver_claim_interface(drv, intf, data)	\
	((void)(drv), (intf)->intfdata = (data), 0)
#define usb_driver_release_interface(drv, intf)	((void)(drv), (void)(intf))

int kshim_usb_errno(int libusb_err);
int usb_control_msg_send(struct usb_device *udev, u8 ep, u8 req, u8 reqtype,
			 u16 val, u16 idx, const void *buf, u16 len,
			 int timeout_ms, gfp_t gfp);
int usb_control_msg_recv(struct usb_device *udev, u8 ep, u8 req, u8 reqtype,
			 u16 val, u16 idx, void *buf, u16 len,
			 int timeout_ms, gfp_t gfp);
/* SET_CONFIGURATION, then claim both interfaces */
int usb_reset_configuration(struct usb_device *udev);

/* ---- V4L2: only what the control plane reads and writes -------------- */

struct v4l2_device {
	void (*release)(struct v4l2_device *v4l2_dev);
	void *ctrl_handler;
};

#define v4l2_device_register(dev, v)	((void)(dev), (void)(v), 0)
#define v4l2_device_unregister(v)	((void)(v))
#define v4l2_device_disconnect(v)	((void)(v))
/* the only reference there is */
#define v4l2_device_put(v)		((v)->release(v))

struct v4l2_ctrl_handler {
	int error;
};

#define v4l2_ctrl_handler_free(h)	((void)(h))
#define v4l2_ctrl_handler_setup(h)	((void)(h), 0)

struct video_device {
	spinlock_t fh_lock;
	struct list_head fh_list;	/* non-empty while a capture session is open */
	int num;
};

struct vb2_queue {
	bool busy;			/* a capture session holds buffers */
};

#define vb2_is_busy(q)		((q)->busy)

struct vb2_v4l2_buffer {
	int unused;
};

struct v4l2_pix_format {
	u32 width, height;
	u32 pixelformat;
	u32 field;
	u32 bytesperline;
	u32 sizeimage;
	u32 colorspace;
	u32 flags;
	u32 ycbcr_enc;
	u32 quantization;
	u32 xfer_func;
};

struct v4l2_bt_timings {
	u32 width, height;
	u32 interlaced;
	u32 polarities;
	u64 pixelclock;
	u32 hfrontporch, hsync, hbackporch;
	u32 vfrontporch, vsync, vbackporch;
	u32 il_vfrontporch, il_vsync, il_vbackporch;
	u32 standards;
	u32 flags;
};

struct v4l2_dv_timings {
	u32 type;
	struct v4l2_bt_timings bt;
};

struct v4l2_event {
	u32 type;
	union {
		struct {
			u32 changes;
		} src_change;
	} u;
};

/* implemented by mac/video.c */
void v4l2_event_queue(struct video_device *vdev, const struct v4l2_event *ev);

#define V4L2_PIX_FMT_YUYV		0x56595559	/* 'YUYV' */
#define V4L2_FIELD_NONE			1
#define V4L2_FIELD_INTERLACED		4
#define V4L2_COLORSPACE_SMPTE170M	1
#define V4L2_COLORSPACE_REC709		3
#define V4L2_XFER_FUNC_709		1
#define V4L2_YCBCR_ENC_DEFAULT		0
#define V4L2_QUANTIZATION_DEFAULT	0
#define V4L2_QUANTIZATION_FULL_RANGE	1
#define V4L2_QUANTIZATION_LIM_RANGE	2
#define V4L2_DV_RGB_RANGE_AUTO		0
#define V4L2_DV_RGB_RANGE_LIMITED	1
#define V4L2_DV_RGB_RANGE_FULL		2
#define V4L2_DV_BT_656_1120		0
#define V4L2_DV_PROGRESSIVE		0
#define V4L2_DV_INTERLACED		1
#define V4L2_DV_VSYNC_POS_POL		0x1
#define V4L2_DV_HSYNC_POS_POL		0x2
#define V4L2_DV_BT_STD_CEA861		0x1
#define V4L2_DV_FL_HALF_LINE		0x8
#define V4L2_EVENT_SOURCE_CHANGE	5
#define V4L2_EVENT_SRC_CH_RESOLUTION	0x1

#endif /* _KSHIM_H_ */
