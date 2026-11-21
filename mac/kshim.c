// SPDX-License-Identifier: GPL-2.0
/*
 * Userspace implementation of the kernel services declared in shim/kshim.h:
 * logging, sleeping, work queues, and control transfers over libusb.
 */
#include <libusb.h>
#include <stdarg.h>
#include <time.h>

#include "kshim.h"
#include "hd60s-mac.h"

/* ---- time ------------------------------------------------------------- */

u64 kshim_now_ns(void)
{
	return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

void kshim_sleep_us(unsigned long us)
{
	struct timespec ts = { .tv_sec = us / 1000000UL,
			       .tv_nsec = (long)(us % 1000000UL) * 1000L };

	while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
		;
}

/* ---- logging ---------------------------------------------------------- */

int kshim_loglevel = KSHIM_INFO;

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static void (*log_hook)(int level, const char *msg);

void hd60s_mac_set_log(void (*fn)(int level, const char *msg), int max_level)
{
	log_hook = fn;
	kshim_loglevel = max_level;
}

static void log_append(char *out, size_t cap, size_t *len, const char *s)
{
	size_t n = strlen(s);

	if (*len + n >= cap)
		n = cap - 1 - *len;
	memcpy(out + *len, s, n);
	*len += n;
	out[*len] = '\0';
}

/*
 * printf, walked one conversion at a time so that the kernel's "%*ph" (hex
 * dump of N bytes) can be handled; the C library has no such thing.
 */
static void kshim_vformat(char *out, size_t cap, const char *fmt, va_list ap)
{
	size_t len = 0;
	char spec[32], piece[256];

	out[0] = '\0';
	while (*fmt && len + 1 < cap) {
		const char *start = fmt;
		int lmod = 0;		/* 0, 1 = l, 2 = ll, 3 = z */
		int width = -1;
		size_t n;

		if (*fmt != '%') {
			out[len++] = *fmt++;
			out[len] = '\0';
			continue;
		}
		fmt++;
		if (*fmt == '%') {
			out[len++] = *fmt++;
			out[len] = '\0';
			continue;
		}
		while (*fmt && strchr("-+ #0", *fmt))
			fmt++;
		if (*fmt >= '0' && *fmt <= '9') {
			width = 0;
			while (*fmt >= '0' && *fmt <= '9')
				width = width * 10 + (*fmt++ - '0');
		}
		if (*fmt == '.') {
			fmt++;
			while (*fmt >= '0' && *fmt <= '9')
				fmt++;
		}
		for (;; fmt++) {
			if (*fmt == 'l')
				lmod = lmod == 1 ? 2 : 1;
			else if (*fmt == 'z')
				lmod = 3;
			else if (*fmt != 'h')
				break;
		}
		if (!*fmt)
			break;

		if (fmt[0] == 'p' && fmt[1] == 'h') {
			const u8 *b = va_arg(ap, const u8 *);
			int i, cnt = width > 0 ? width : 1;

			for (i = 0; i < cnt; i++) {
				snprintf(piece, sizeof(piece), i ? " %02x" : "%02x", b[i]);
				log_append(out, cap, &len, piece);
			}
			fmt += 2;
			continue;
		}

		n = (size_t)(fmt + 1 - start);
		if (n >= sizeof(spec))
			n = sizeof(spec) - 1;
		memcpy(spec, start, n);
		spec[n] = '\0';

		switch (*fmt++) {
		case 'd':
		case 'i':
			if (lmod == 2)
				snprintf(piece, sizeof(piece), spec, va_arg(ap, long long));
			else if (lmod == 1)
				snprintf(piece, sizeof(piece), spec, va_arg(ap, long));
			else if (lmod == 3)
				snprintf(piece, sizeof(piece), spec, va_arg(ap, ssize_t));
			else
				snprintf(piece, sizeof(piece), spec, va_arg(ap, int));
			break;
		case 'u':
		case 'x':
		case 'X':
		case 'o':
			if (lmod == 2)
				snprintf(piece, sizeof(piece), spec, va_arg(ap, unsigned long long));
			else if (lmod == 1)
				snprintf(piece, sizeof(piece), spec, va_arg(ap, unsigned long));
			else if (lmod == 3)
				snprintf(piece, sizeof(piece), spec, va_arg(ap, size_t));
			else
				snprintf(piece, sizeof(piece), spec, va_arg(ap, unsigned int));
			break;
		case 'c':
			snprintf(piece, sizeof(piece), spec, va_arg(ap, int));
			break;
		case 's':
			snprintf(piece, sizeof(piece), spec, va_arg(ap, const char *));
			break;
		case 'p':
			snprintf(piece, sizeof(piece), spec, va_arg(ap, void *));
			break;
		default:
			snprintf(piece, sizeof(piece), "%s", spec);
			break;
		}
		log_append(out, cap, &len, piece);
	}
}

void kshim_log(int level, const char *fmt, ...)
{
	static const char * const tag[] = {
		[KSHIM_ERR] = "error: ", [KSHIM_WARN] = "warning: ",
		[KSHIM_INFO] = "", [KSHIM_DBG] = "debug: ",
	};
	char buf[1024];
	va_list ap;

	if (level > kshim_loglevel)
		return;
	va_start(ap, fmt);
	kshim_vformat(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	if (log_hook) {
		log_hook(level, buf);
		return;
	}
	pthread_mutex_lock(&log_lock);
	fprintf(stderr, "hd60s: %s%s", tag[level], buf);
	pthread_mutex_unlock(&log_lock);
}

/* ---- work queues ------------------------------------------------------ */

#define KSHIM_MAX_WORKS	8

struct workqueue_struct {
	pthread_t thread;
	bool stop;
	struct work_struct *works[KSHIM_MAX_WORKS];
	int nworks;
};

/* One lock for all work state: there is one device and a handful of works. */
static pthread_mutex_t wq_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wq_cond = PTHREAD_COND_INITIALIZER;

static void *wq_thread(void *arg)
{
	struct workqueue_struct *wq = arg;

	pthread_setname_np("hd60s-work");
	pthread_mutex_lock(&wq_lock);
	while (!wq->stop) {
		struct work_struct *best = NULL;
		u64 now = kshim_now_ns();
		int i;

		for (i = 0; i < wq->nworks; i++) {
			struct work_struct *w = wq->works[i];

			if (w->pending && (!best || w->due_ns < best->due_ns))
				best = w;
		}
		if (best && best->due_ns <= now) {
			best->pending = false;
			best->running = true;
			pthread_mutex_unlock(&wq_lock);
			best->fn(best);
			pthread_mutex_lock(&wq_lock);
			best->running = false;
			pthread_cond_broadcast(&wq_cond);
		} else if (best) {
			u64 wait = best->due_ns - now;
			struct timespec ts = { .tv_sec = (time_t)(wait / 1000000000ULL),
					       .tv_nsec = (long)(wait % 1000000000ULL) };

			pthread_cond_timedwait_relative_np(&wq_cond, &wq_lock, &ts);
		} else {
			pthread_cond_wait(&wq_cond, &wq_lock);
		}
	}
	pthread_mutex_unlock(&wq_lock);
	return NULL;
}

struct workqueue_struct *alloc_ordered_workqueue(const char *name, unsigned int flags)
{
	struct workqueue_struct *wq = calloc(1, sizeof(*wq));

	(void)name;
	(void)flags;
	if (!wq)
		return NULL;
	if (pthread_create(&wq->thread, NULL, wq_thread, wq)) {
		free(wq);
		return NULL;
	}
	return wq;
}

void destroy_workqueue(struct workqueue_struct *wq)
{
	pthread_mutex_lock(&wq_lock);
	wq->stop = true;
	pthread_cond_broadcast(&wq_cond);
	pthread_mutex_unlock(&wq_lock);
	pthread_join(wq->thread, NULL);
	free(wq);
}

void kshim_init_work(struct work_struct *w, void (*fn)(struct work_struct *))
{
	memset(w, 0, sizeof(*w));
	w->fn = fn;
}

bool kshim_queue_work(struct workqueue_struct *wq, struct work_struct *w,
		      unsigned long delay_ms)
{
	bool queued = false;
	int i;

	pthread_mutex_lock(&wq_lock);
	if (!w->canceling && !w->pending) {
		for (i = 0; i < wq->nworks && wq->works[i] != w; i++)
			;
		if (i == wq->nworks && wq->nworks < KSHIM_MAX_WORKS)
			wq->works[wq->nworks++] = w;
		if (i < wq->nworks) {
			w->wq = wq;
			w->due_ns = kshim_now_ns() + (u64)delay_ms * 1000000ULL;
			w->pending = true;
			queued = true;
			pthread_cond_broadcast(&wq_cond);
		}
	}
	pthread_mutex_unlock(&wq_lock);
	return queued;
}

/* True when called from the work itself, which must not wait for itself. */
static bool wq_is_self(struct work_struct *w)
{
	return w->wq && pthread_equal(pthread_self(), w->wq->thread);
}

/* Not pending and not running on return, even if the work requeues itself. */
bool cancel_work_sync(struct work_struct *w)
{
	bool was;

	pthread_mutex_lock(&wq_lock);
	was = w->pending;
	w->canceling = true;
	w->pending = false;
	while (w->running && !wq_is_self(w))
		pthread_cond_wait(&wq_cond, &wq_lock);
	w->canceling = false;
	pthread_mutex_unlock(&wq_lock);
	return was;
}

void flush_work(struct work_struct *w)
{
	pthread_mutex_lock(&wq_lock);
	while ((w->pending || w->running) && !wq_is_self(w))
		pthread_cond_wait(&wq_cond, &wq_lock);
	pthread_mutex_unlock(&wq_lock);
}

/* ---- USB -------------------------------------------------------------- */

/* Set HD60S_TRACE=1 to log every control transfer. */
static int usb_trace = -1;

static bool usb_tracing(void)
{
	if (usb_trace < 0)
		usb_trace = getenv("HD60S_TRACE") ? 1 : 0;
	return usb_trace;
}

int kshim_usb_errno(int err)
{
	switch (err) {
	case LIBUSB_ERROR_TIMEOUT:
		return -ETIMEDOUT;
	case LIBUSB_ERROR_PIPE:
		return -EPIPE;
	case LIBUSB_ERROR_NO_DEVICE:
		return -ENODEV;
	case LIBUSB_ERROR_NO_MEM:
		return -ENOMEM;
	case LIBUSB_ERROR_BUSY:
		return -EBUSY;
	case LIBUSB_ERROR_INVALID_PARAM:
		return -EINVAL;
	default:
		return err < 0 ? -EIO : err;
	}
}

static int usb_control(struct usb_device *udev, u8 req, u8 reqtype, u16 val,
		       u16 idx, void *buf, u16 len, int timeout_ms)
{
	int r = libusb_control_transfer(udev->h, reqtype, req, val, idx, buf,
					len, (unsigned int)timeout_ms);

	if (usb_tracing()) {
		char hex[3 * 16 + 1] = "";
		int i, n = r > 0 ? (r > 16 ? 16 : r) : 0;

		for (i = 0; i < n; i++)
			snprintf(hex + 3 * i, 4, " %02x", ((u8 *)buf)[i]);
		kshim_log(KSHIM_INFO, "ctrl %s req %02x val %04x idx %04x len %u -> %d%s%s\n",
			  (reqtype & USB_DIR_IN) ? "IN " : "OUT", req, val, idx,
			  len, r, n ? " :" : "", hex);
	}
	if (r == LIBUSB_ERROR_NO_DEVICE)
		udev->state = USB_STATE_NOTATTACHED;
	if (r < 0)
		return kshim_usb_errno(r);
	/* the kernel helpers treat a short transfer as a failure */
	return r == len ? 0 : -EIO;
}

int usb_control_msg_send(struct usb_device *udev, u8 ep, u8 req, u8 reqtype,
			 u16 val, u16 idx, const void *buf, u16 len,
			 int timeout_ms, gfp_t gfp)
{
	(void)ep;
	(void)gfp;
	return usb_control(udev, req, reqtype, val, idx, (void *)buf, len, timeout_ms);
}

int usb_control_msg_recv(struct usb_device *udev, u8 ep, u8 req, u8 reqtype,
			 u16 val, u16 idx, void *buf, u16 len,
			 int timeout_ms, gfp_t gfp)
{
	(void)ep;
	(void)gfp;
	return usb_control(udev, req, reqtype, val, idx, buf, len, timeout_ms);
}

int usb_reset_configuration(struct usb_device *udev)
{
	int i, r;

	r = libusb_set_configuration(udev->h, 1);
	if (r < 0)
		return kshim_usb_errno(r);
	for (i = 0; i < 2; i++) {
		r = libusb_claim_interface(udev->h, i);
		if (r < 0) {
			kshim_log(KSHIM_ERR, "cannot claim interface %d: %s\n", i,
				  libusb_error_name(r));
			return kshim_usb_errno(r);
		}
	}
	return 0;
}
