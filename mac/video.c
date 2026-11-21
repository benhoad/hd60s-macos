// SPDX-License-Identifier: GPL-2.0
/*
 * Elgato Game Capture HD60 S - macOS capture transport
 *
 * The macOS counterpart of upstream/hd60s-video.c and hd60s-audio.c: the
 * transfer ring on libusb instead of URBs, and a callback instead of V4L2 and
 * ALSA. The transport sequences (alt selection, start and stop order, the
 * delays between steps) are ported from hd60s-video.c,
 * Copyright (c) 2026 Doug Brown <doug@schmorgal.com>, and deliberately kept
 * in the same shape so the two can be compared side by side.
 */
#include <libusb.h>
#include <pthread/qos.h>

#include "hd60s.h"
#include "hd60s-mac.h"

struct hd60s_mac {
	libusb_context *ctx;
	libusb_device_handle *h;
	struct libusb_config_descriptor *cfg;
	struct usb_device udev;
	struct usb_interface intf[2];

	pthread_t ev_thread;
	atomic_bool ev_run;

	struct libusb_transfer *xfer[HD60S_NUM_URBS];
	unsigned int bulk_size;
	unsigned int err_run;		/* consecutive failed transfers */

	/* the capture session */
	bool open;
	bool force_bulk;
	struct list_head fh;		/* on d->vdev.fh_list while open */
	struct hd60s_buffer token;	/* what p->cur points at */
	u8 *frame;
	struct hd60s_mac_callbacks cb;
};

static struct hd60s_mac *mac_of(struct hd60s_dev *d)
{
	return d->udev->priv;
}

static struct hd60s_dev *parser_dev(struct hd60s_parser *p)
{
	return container_of(p, struct hd60s_dev, p);
}

/*
 * The parser's back end, on the USB event thread. There is one frame buffer:
 * the frame callback runs synchronously and the buffer is refilled after it
 * returns.
 */

void hd60s_p_get_buffer(struct hd60s_parser *p)
{
	struct hd60s_mac *m = mac_of(parser_dev(p));

	if (m->frame) {
		p->cur = &m->token;
		p->fb = m->frame;
	} else {
		p->dropped++;
	}
}

void hd60s_p_finish_buffer(struct hd60s_parser *p)
{
	struct hd60s_dev *d = parser_dev(p);
	struct hd60s_mac *m = mac_of(d);
	struct hd60s_buffer *b = p->cur;

	p->cur = NULL;
	p->fb = NULL;
	p->started = false;
	p->restart_f0 = 0;	/* a whole frame proves the fields alternate */
	if (!b)
		return;
	p->frames++;

	if (m->cb.frame) {
		struct hd60s_mac_frame f = {
			.data = m->frame,
			.width = p->line_bytes / 2,
			.height = p->frame_h,
			.stride = p->line_bytes,
			.size = p->frame_size,
			.interlaced = p->interlaced,
			.bt709 = d->fmt.colorspace == V4L2_COLORSPACE_REC709,
			.full_range = d->fmt.quantization == V4L2_QUANTIZATION_FULL_RANGE,
			.sequence = p->sequence,
			.timestamp_ns = ktime_get_ns(),
		};

		m->cb.frame(m->cb.ctx, &f);
	}
	p->sequence++;
}

void hd60s_p_abort_frame(struct hd60s_parser *p)
{
	p->partial++;
	p->lines = 0;
	p->started = false;
	p->needs_clear = true;		/* lines are missing; clear before reuse */

	if (p->interlaced && p->field == 0 && ++p->restart_f0 >= 60 &&
	    !p->f0_warned) {
		p->f0_warned = true;
		dev_warn(&parser_dev(p)->intf->dev,
			 "every frame aborts at a field-0 restart; the source may be genuine 240p/288p, which this device reports as interlaced and cannot weave\n");
	}
}

void hd60s_p_audio(struct hd60s_parser *p, const u8 *data, u32 len)
{
	hd60s_audio_write(parser_dev(p), data, len);
}

void hd60s_audio_write(struct hd60s_dev *d, const u8 *data, u32 len)
{
	struct hd60s_mac *m = mac_of(d);

	if (m->cb.audio)
		m->cb.audio(m->cb.ctx, data, len);
}

int hd60s_audio_register(struct hd60s_dev *d)
{
	(void)d;
	return 0;
}

void hd60s_audio_unregister(struct hd60s_dev *d)
{
	(void)d;
}

void hd60s_audio_suspend(struct hd60s_dev *d)
{
	(void)d;
}

/* The status poll's V4L2_EVENT_SOURCE_CHANGE. */
void v4l2_event_queue(struct video_device *vdev, const struct v4l2_event *ev)
{
	struct hd60s_dev *d = container_of(vdev, struct hd60s_dev, vdev);
	struct hd60s_mac *m = mac_of(d);

	(void)ev;
	if (m->cb.source_change)
		m->cb.source_change(m->cb.ctx);
}

/* ---- the transfer ring ------------------------------------------------ */

static void *hd60s_event_thread(void *arg)
{
	struct hd60s_mac *m = arg;

	pthread_setname_np("hd60s-usb");
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
	while (atomic_load(&m->ev_run)) {
		struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };

		libusb_handle_events_timeout_completed(m->ctx, &tv, NULL);
	}
	return NULL;
}

static void hd60s_resubmit(struct hd60s_dev *d, struct libusb_transfer *x)
{
	struct hd60s_mac *m = mac_of(d);
	int ret, n;

	if (!READ_ONCE(d->streaming)) {
		atomic_dec(&d->inflight);
		return;
	}
	/*
	 * A halted endpoint fails every transfer at once; resubmitting would
	 * spin. Let the ring run down and the watchdog report the silence.
	 */
	if (m->err_run > 4 * HD60S_NUM_URBS) {
		n = atomic_dec_return(&d->inflight);
		if (!n)
			dev_err(&d->intf->dev,
				"every transfer is failing; the ring has stopped\n");
		return;
	}
	ret = libusb_submit_transfer(x);
	if (ret < 0) {
		n = atomic_dec_return(&d->inflight);
		d->resubmit_err++;
		if (d->resubmit_err <= 3)
			dev_err(&d->intf->dev,
				"transfer resubmit failed: %s (%d still in flight)\n",
				libusb_error_name(ret), n);
	}
}

static void LIBUSB_CALL hd60s_xfer_complete(struct libusb_transfer *x)
{
	struct hd60s_dev *d = x->user_data;
	struct hd60s_mac *m = mac_of(d);
	bool iso = x->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS;
	int i;

	switch (x->status) {
	case LIBUSB_TRANSFER_COMPLETED:
		m->err_run = 0;
		break;
	case LIBUSB_TRANSFER_NO_DEVICE:
		d->udev->state = USB_STATE_NOTATTACHED;
		atomic_dec(&d->inflight);
		return;
	case LIBUSB_TRANSFER_CANCELLED:
		atomic_dec(&d->inflight);	/* canceled: off the ring */
		return;
	default:
		m->err_run++;
		if (!iso) {
			/* Counted, not parsed, as upstream. */
			d->bulk_urbs++;
			d->bulk_err++;
			d->bulk_lost += x->actual_length;
			d->bulk_status = x->status;
		}
		if (m->err_run <= 3)
			dev_warn(&d->intf->dev, "%s transfer status %d\n",
				 iso ? "iso" : "bulk", x->status);
		if (!iso) {
			hd60s_resubmit(d, x);
			return;
		}
		break;
	}

	if (iso) {
		/*
		 * A packet the host controller could not deliver is a hole in a
		 * byte stream whose records straddle packet boundaries, so it
		 * costs the frame it lands in. Counting it is what separates
		 * USB-layer loss from a parser fault.
		 */
		for (i = 0; i < x->num_iso_packets; i++) {
			struct libusb_iso_packet_descriptor *pk = &x->iso_packet_desc[i];

			d->iso_total++;
			if (pk->status != LIBUSB_TRANSFER_COMPLETED) {
				d->iso_err++;
				continue;
			}
			if (!pk->actual_length)
				continue;
			hd60s_parse(&d->p, libusb_get_iso_packet_buffer_simple(x, i),
				    pk->actual_length);
		}
	} else {
		d->bulk_urbs++;
		hd60s_parse(&d->p, x->buffer, x->actual_length);
	}

	hd60s_resubmit(d, x);
}

/* Cancel the ring and wait for the event thread to see it empty. */
static void hd60s_kill_urbs(struct hd60s_dev *d)
{
	struct hd60s_mac *m = mac_of(d);
	int i;

	for (i = 0; i < HD60S_NUM_URBS; i++)
		if (m->xfer[i])
			libusb_cancel_transfer(m->xfer[i]);

	for (i = 0; i < 400 && atomic_read(&d->inflight) > 0; i++)
		usleep_range(5000, 6000);
	if (atomic_read(&d->inflight) > 0)
		dev_warn(&d->intf->dev, "%d transfers did not cancel\n",
			 atomic_read(&d->inflight));
}

static void hd60s_free_urbs(struct hd60s_dev *d)
{
	struct hd60s_mac *m = mac_of(d);
	int i;

	/* A transfer libusb still owns cannot be freed; leak it instead. */
	if (atomic_read(&d->inflight) > 0) {
		memset(m->xfer, 0, sizeof(m->xfer));
		return;
	}
	for (i = 0; i < HD60S_NUM_URBS; i++) {
		struct libusb_transfer *x = m->xfer[i];

		if (!x)
			continue;
		free(x->buffer);
		libusb_free_transfer(x);
		m->xfer[i] = NULL;
	}
}

static int hd60s_alloc_urbs(struct hd60s_dev *d)
{
	struct hd60s_mac *m = mac_of(d);
	int i;

	d->urb_size = d->use_bulk ? m->bulk_size
				  : d->iso_pkt_size * d->iso_packets;

	for (i = 0; i < HD60S_NUM_URBS; i++) {
		struct libusb_transfer *x;

		x = libusb_alloc_transfer(d->use_bulk ? 0 : d->iso_packets);
		if (!x)
			goto fail;
		m->xfer[i] = x;
		x->buffer = malloc(d->urb_size);
		if (!x->buffer)
			goto fail;
	}
	return 0;

fail:
	hd60s_free_urbs(d);
	return -ENOMEM;
}

/*
 * Lay the transfers out for the alt setting actually in force. SET_INTERFACE
 * can land on a lower alt than was asked for, meaning smaller packets; the
 * buffers are big enough, because the retry only walks downwards.
 */
static void hd60s_relayout_urbs(struct hd60s_dev *d)
{
	struct hd60s_mac *m = mac_of(d);
	int i;

	for (i = 0; i < HD60S_NUM_URBS; i++) {
		struct libusb_transfer *x = m->xfer[i];

		if (d->use_bulk) {
			libusb_fill_bulk_transfer(x, m->h, HD60S_EP_VIDEO, x->buffer,
						  (int)d->urb_size,
						  hd60s_xfer_complete, d, 0);
		} else {
			libusb_fill_iso_transfer(x, m->h, HD60S_EP_VIDEO, x->buffer,
						 (int)(d->iso_pkt_size * d->iso_packets),
						 d->iso_packets,
						 hd60s_xfer_complete, d, 0);
			libusb_set_iso_packet_lengths(x, d->iso_pkt_size);
		}
	}
}

/* Bytes/s, or 0 if the alt carries no isochronous EP 0x83. */
static u64 hd60s_alt_capacity(struct hd60s_dev *d, int alt,
			      unsigned int *psize, unsigned int *interval)
{
	struct hd60s_mac *m = mac_of(d);
	const struct libusb_interface *itf = &m->cfg->interface[HD60S_IF_VIDEO];
	unsigned int size, interval_us;
	int a, i;
	u8 bi;

	for (a = 0; a < itf->num_altsetting; a++) {
		const struct libusb_interface_descriptor *id = &itf->altsetting[a];

		if (id->bAlternateSetting != alt)
			continue;
		for (i = 0; i < id->bNumEndpoints; i++) {
			const struct libusb_endpoint_descriptor *ep = &id->endpoint[i];
			struct libusb_ss_endpoint_companion_descriptor *ss = NULL;

			if (ep->bEndpointAddress != HD60S_EP_VIDEO)
				continue;
			if ((ep->bmAttributes & 3) != LIBUSB_TRANSFER_TYPE_ISOCHRONOUS)
				return 0;

			size = ep->wMaxPacketSize & 0x7ff;
			if (d->udev->speed >= USB_SPEED_SUPER &&
			    !libusb_get_ss_endpoint_companion_descriptor(m->ctx, ep, &ss)) {
				d->alt_burst = ss->bMaxBurst + 1;
				d->alt_mult = (ss->bmAttributes & 3) + 1;
				size *= d->alt_burst * d->alt_mult;
				libusb_free_ss_endpoint_companion_descriptor(ss);
			} else {
				d->alt_burst = 1;
				d->alt_mult = ((ep->wMaxPacketSize >> 11) & 3) + 1;
				size *= d->alt_mult;
			}
			if (!size)
				return 0;

			/* high/super speed isochronous: 125 us << (bInterval - 1) */
			bi = clamp_t(u8, ep->bInterval, 1, 16);
			interval_us = 125U << (bi - 1);
			*psize = size;
			if (interval)
				*interval = 1U << (bi - 1);
			return div_u64((u64)size * 1000000ULL, interval_us);
		}
	}
	return 0;
}

/*
 * Alt selection, using the Windows driver's formula: alt 1 / alt 1 / alt 2 for
 * 480i / 720p60 / 1080p60.
 */
static int hd60s_pick_alt(struct hd60s_dev *d)
{
	bool ss = d->udev->speed >= USB_SPEED_SUPER;
	u64 bw = (u64)d->tm.hactive * d->tm.vactive * d->tm.fps;
	u64 thr = ss ? 65536000ULL : 4096000ULL;
	unsigned int psize = 0, itv = 1;
	int alt;

	bw += (bw * 0xb926fabULL) >> 32;		/* x1.0451803 */

	if (bw <= thr)
		alt = 1;
	else if (!ss || bw <= 2 * thr)
		alt = 2;
	else
		alt = 3;

	/*
	 * Never alt 3: the firmware brings that endpoint up as bulk regardless
	 * of the descriptor.
	 */
	if (alt == 3 || !hd60s_alt_capacity(d, alt, &psize, &itv) || !psize) {
		d->use_bulk = true;
		dev_info(&d->intf->dev,
			 "%llu Mpx/s: no usable isochronous alt, using bulk (alt 4)\n",
			 div_u64(bw, 1000000));
		return 4;
	}

	d->iso_pkt_size = psize;
	d->iso_interval = itv;
	d->use_bulk = false;
	dev_dbg(&d->intf->dev,
		"%llu Mpx/s -> alt %d (%u B/interval, burst %u mult %u)\n",
		div_u64(bw, 1000000), alt, psize, d->alt_burst, d->alt_mult);
	return alt;
}

static int usb_set_interface(struct usb_device *udev, int ifnum, int alt)
{
	int r = libusb_set_interface_alt_setting(udev->h, ifnum, alt);

	if (r == LIBUSB_ERROR_NO_DEVICE)
		udev->state = USB_STATE_NOTATTACHED;
	return r < 0 ? kshim_usb_errno(r) : 0;
}

/*
 * SET_INTERFACE with the Windows driver's downward retry: on failure try
 * alt-1, alt-2, and so on.
 */
static int hd60s_set_alt(struct hd60s_dev *d, int alt)
{
	int ret = -EINVAL;

	lockdep_assert_held(&d->ctrl_lock);
	for (; alt > 0; alt--) {
		unsigned int psize = 0, itv = 1;

		ret = usb_set_interface(d->udev, HD60S_IF_VIDEO, alt);
		if (ret == 0) {
			d->alt = alt;
			if (!d->use_bulk &&
			    hd60s_alt_capacity(d, alt, &psize, &itv) && psize) {
				d->iso_pkt_size = psize;
				d->iso_interval = itv;
			}
			return 0;
		}
		dev_warn(&d->intf->dev, "SET_INTERFACE alt %d: %d\n", alt, ret);
		if (d->use_bulk || ret == -ETIMEDOUT || ret == -ENODEV)
			break;
	}
	return ret;
}

/* Through the same wedge latch as every other control transfer. */
static int hd60s_set_alt0(struct hd60s_dev *d)
{
	int ret = usb_set_interface(d->udev, HD60S_IF_VIDEO, 0);

	if (ret == -ETIMEDOUT && !d->wedged) {
		d->wedged = true;
		dev_err(&d->intf->dev,
			"SET_INTERFACE alt 0 timed out, EP0 is wedged\n");
	} else if (ret < 0) {
		dev_err(&d->intf->dev, "SET_INTERFACE alt 0: %d\n", ret);
	}
	return ret;
}

/*
 * Transport teardown, in the Windows driver's order: SET_INTERFACE alt 0 goes
 * out before the ring is canceled, so the FX3 drains its DMA channel into a
 * pipe the host was servicing until that moment. Canceling first fills those
 * buffers and blocks the thread that also services vendor requests, leaving the
 * device enumerated but unresponsive until VBUS is cycled.
 *
 * ctrl_lock is held throughout: the status poll must not fire an I2C
 * transaction into the FX3's SET_INTERFACE handler.
 */
static void hd60s_stop_transport(struct hd60s_dev *d)
{
	lockdep_assert_held(&d->ctrl_lock);
	WRITE_ONCE(d->streaming, false);

	if (d->wedged)
		dev_warn(&d->intf->dev,
			 "EP0 wedged: skipping the control-plane teardown\n");
	else
		hd60s_set_alt0(d);

	hd60s_kill_urbs(d);

	if (d->wedged)
		return;
	msleep(35);
	hd60s_disarm_events_nolock(d);
	hd60s_stream_enable_nolock(d, false);
}

/*
 * Transport bring-up: stream enable, the alt setting 105 ms later, then the
 * ring. On failure the transport is left torn down.
 */
static int hd60s_start_transport(struct hd60s_dev *d, int alt)
{
	struct hd60s_mac *m = mac_of(d);
	int i, ret;

	mutex_lock(&d->ctrl_lock);
	hd60s_disarm_events_nolock(d);

	ret = hd60s_stream_enable_nolock(d, true);
	if (ret < 0) {
		dev_err(&d->intf->dev, "stream enable: %d\n", ret);
		goto unlock;		/* nothing started, nothing to tear down */
	}
	msleep(105);

	ret = hd60s_set_alt(d, alt);
	if (ret < 0)
		goto stop;
	hd60s_relayout_urbs(d);

	m->err_run = 0;
	WRITE_ONCE(d->streaming, true);
	atomic_set(&d->inflight, 0);
	for (i = 0; i < HD60S_NUM_URBS; i++) {
		atomic_inc(&d->inflight);
		ret = libusb_submit_transfer(m->xfer[i]);
		if (ret < 0) {
			atomic_dec(&d->inflight);
			dev_err(&d->intf->dev, "submit transfer %d: %s\n", i,
				libusb_error_name(ret));
			ret = kshim_usb_errno(ret);
			goto stop;
		}
	}
	mutex_unlock(&d->ctrl_lock);
	return 0;

stop:
	hd60s_stop_transport(d);
unlock:
	mutex_unlock(&d->ctrl_lock);
	return ret;
}

/* A once-per-second check that the endpoint is delivering. */
#define HD60S_WATCHDOG_SILENT_S	8

void hd60s_watchdog(struct work_struct *work)
{
	struct hd60s_dev *d = container_of(work, struct hd60s_dev, watchdog.work);
	struct hd60s_parser *p = &d->p;

	if (!READ_ONCE(d->streaming))
		return;

	d->wd_tick++;
	if (!p->bytes && !d->use_bulk && !d->wd_warned &&
	    d->wd_tick >= HD60S_WATCHDOG_SILENT_S) {
		d->wd_warned = true;
		dev_err(&d->intf->dev,
			"no isochronous data after %u s on alt %d (burst %u mult %u); try bulk\n",
			d->wd_tick, d->alt, d->alt_burst, d->alt_mult);
	} else if (d->wd_warned && p->bytes) {
		d->wd_warned = false;
		dev_info(&d->intf->dev,
			 "isochronous data started after %u s; the error above was premature\n",
			 d->wd_tick);
	} else if (p->bytes == d->wd_bytes && d->wd_tick > HD60S_WATCHDOG_SILENT_S) {
		dev_warn(&d->intf->dev,
			 "endpoint silent for a second (%u frames so far)\n",
			 p->frames);
	}

	d->wd_bytes = p->bytes;
	queue_delayed_work(d->wq, &d->watchdog, msecs_to_jiffies(1000));
}

/*
 * Snapshot the geometry the parser will use, all of it from the configured
 * mode, which is what the frame buffer was sized from.
 */
static void hd60s_parser_reset(struct hd60s_dev *d)
{
	struct hd60s_parser *p = &d->p;

	memset(p, 0, sizeof(*p));
	p->interlaced = d->fmt.field == V4L2_FIELD_INTERLACED;
	p->line_bytes = d->fmt.bytesperline;
	p->stride     = p->line_bytes * (p->interlaced ? 2 : 1);
	p->frame_h    = d->fmt.height;
	p->frame_size = d->fmt.sizeimage;
	p->state      = HD60S_PS_HDR;
	p->in_blank   = true;		/* wait for a real field start */
	p->needs_clear = true;

	d->iso_total = 0;
	d->iso_err = 0;
	d->bulk_urbs = 0;
	d->bulk_err = 0;
	d->bulk_lost = 0;
	d->bulk_status = 0;
	d->resubmit_err = 0;
}

/* Bring up the transport. Needs a signal: the alt is sized from the mode. */
static int hd60s_start_capture(struct hd60s_dev *d)
{
	struct hd60s_mac *m = mac_of(d);
	struct hd60s_parser *p = &d->p;
	int alt, ret;

	lockdep_assert_held(&d->vlock);

	{
		bool il = hd60s_interlaced(&d->tm);
		u32 dw = d->tm.hactive, dh = d->tm.vactive * (il ? 2 : 1);

		if (dw * 2 != p->line_bytes || dh != p->frame_h) {
			dev_warn(&d->intf->dev,
				 "source is %ux%u but the capture is %ux%u, frames stop until the capture is restarted\n",
				 dw, dh, p->line_bytes / 2, p->frame_h);
			p->stale = true;
		}
	}

	if (m->force_bulk) {
		d->use_bulk = true;
		alt = 4;
	} else {
		alt = hd60s_pick_alt(d);
	}

	/* 32 packets per transfer and 16 transfers, the Windows geometry. */
	d->iso_packets = HD60S_ISO_PACKETS;

	ret = hd60s_alloc_urbs(d);
	if (ret < 0)
		return ret;

	ret = hd60s_start_transport(d, alt);
	if (ret < 0) {
		hd60s_free_urbs(d);
		return ret;
	}

	d->wd_bytes = 0;
	d->wd_tick = 0;
	d->wd_warned = false;
	queue_delayed_work(d->wq, &d->watchdog, msecs_to_jiffies(1000));

	dev_info(&d->intf->dev, "streaming %ux%u%s on alt %d (%s)\n",
		 d->fmt.width, d->fmt.height, p->interlaced ? "i" : "p",
		 d->alt, d->use_bulk ? "bulk" : "isochronous");
	return 0;
}

static int hd60s_start_streaming(struct hd60s_dev *d)
{
	if (d->gone)
		return -ENODEV;

	hd60s_parser_reset(d);

	if (!d->signal) {
		dev_info(&d->intf->dev,
			 "no signal: the transport starts when one arrives\n");
		d->await_signal = true;
		return 0;
	}
	return hd60s_start_capture(d);
}

/* Called by the status poll, under vlock, when a signal arrives. */
void hd60s_video_signal(struct hd60s_dev *d)
{
	int ret;

	if (!d->await_signal)
		return;
	d->await_signal = false;

	ret = hd60s_start_capture(d);
	if (ret < 0)
		dev_err(&d->intf->dev,
			"could not start the transport on signal (%d)\n", ret);
}

static void hd60s_stop_streaming(struct hd60s_dev *d)
{
	struct hd60s_parser *p = &d->p;

	cancel_delayed_work_sync(&d->watchdog);

	/* Still waiting for a signal: nothing went out on the wire. */
	if (d->await_signal) {
		d->await_signal = false;
	} else {
		mutex_lock(&d->ctrl_lock);
		hd60s_stop_transport(d);
		mutex_unlock(&d->ctrl_lock);
	}

	hd60s_free_urbs(d);
	p->cur = NULL;
	p->fb = NULL;

	dev_dbg(&d->intf->dev,
		"stopped: %u frames, %u partial, %u dropped, %u resyncs (%u at lock-on), %u parity errors, %llu MB, %u audio bytes, %u/%u iso packets bad\n",
		p->frames, p->partial, p->dropped, p->resyncs, p->resyncs_at_lock,
		p->parity_err, div_u64(p->bytes, 1000000), p->audio_bytes,
		d->iso_err, d->iso_total);
	if (d->use_bulk)
		dev_dbg(&d->intf->dev,
			"bulk: %u transfers, %u errors (last %d), %llu bytes discarded\n",
			d->bulk_urbs, d->bulk_err, d->bulk_status, d->bulk_lost);
	if (p->nbadhdr)
		dev_dbg(&d->intf->dev,
			"first bad headers after lock-on: %4ph %4ph %4ph %4ph\n",
			p->badhdr[0], p->badhdr[1], p->badhdr[2], p->badhdr[3]);
	if (d->resubmit_err)
		dev_warn(&d->intf->dev, "%u transfers were lost from the ring\n",
			 d->resubmit_err);
}

/* System sleep: only the transport goes down and comes back. */
void hd60s_video_suspend(struct hd60s_dev *d)
{
	cancel_delayed_work_sync(&d->watchdog);

	mutex_lock(&d->ctrl_lock);
	d->pm_streaming = READ_ONCE(d->streaming);
	if (d->pm_streaming)
		hd60s_stop_transport(d);
	mutex_unlock(&d->ctrl_lock);
}

void hd60s_video_resume(struct hd60s_dev *d)
{
	int ret;

	if (!d->pm_streaming)
		return;
	d->pm_streaming = false;

	ret = hd60s_start_transport(d, d->alt);
	if (ret < 0) {
		dev_err(&d->intf->dev,
			"resume: could not restart the transport (%d)\n", ret);
		return;
	}

	d->wd_bytes = 0;
	d->wd_tick = 0;
	d->wd_warned = false;
	queue_delayed_work(d->wq, &d->watchdog, msecs_to_jiffies(1000));
}

/* ---- the capture session ---------------------------------------------- */

static void hd60s_session_close(struct hd60s_dev *d)
{
	struct hd60s_mac *m = mac_of(d);
	unsigned long flags;

	lockdep_assert_held(&d->vlock);
	spin_lock_irqsave(&d->vdev.fh_lock, flags);
	list_del(&m->fh);
	spin_unlock_irqrestore(&d->vdev.fh_lock, flags);
	d->queue.busy = false;
	m->open = false;
	free(m->frame);
	m->frame = NULL;
	memset(&m->cb, 0, sizeof(m->cb));
}

int hd60s_mac_start(struct hd60s_dev *d, const struct hd60s_mac_callbacks *cb,
		    bool force_bulk)
{
	struct hd60s_mac *m = mac_of(d);
	unsigned long flags;
	int ret;

	mutex_lock(&d->vlock);
	if (m->open || !d->registered) {
		mutex_unlock(&d->vlock);
		return m->open ? -EBUSY : -ENODEV;
	}

	m->frame = malloc(d->fmt.sizeimage);
	if (!m->frame) {
		mutex_unlock(&d->vlock);
		return -ENOMEM;
	}
	m->cb = *cb;
	m->force_bulk = force_bulk;
	m->open = true;
	d->queue.busy = true;
	spin_lock_irqsave(&d->vdev.fh_lock, flags);
	list_add_tail(&m->fh, &d->vdev.fh_list);
	spin_unlock_irqrestore(&d->vdev.fh_lock, flags);

	ret = hd60s_start_streaming(d);
	if (ret < 0)
		hd60s_session_close(d);
	mutex_unlock(&d->vlock);
	return ret;
}

void hd60s_mac_stop(struct hd60s_dev *d)
{
	struct hd60s_mac *m = mac_of(d);

	mutex_lock(&d->vlock);
	if (m->open) {
		hd60s_stop_streaming(d);
		hd60s_session_close(d);
		/* adopt a mode that changed while the session held the old one */
		if (!d->gone)
			hd60s_follow_detected(d);
	}
	mutex_unlock(&d->vlock);
}

int hd60s_video_register(struct hd60s_dev *d)
{
	struct hd60s_mac *m = mac_of(d);

	spin_lock_init(&d->vdev.fh_lock);
	INIT_LIST_HEAD(&d->vdev.fh_list);
	INIT_LIST_HEAD(&m->fh);
	d->registered = true;
	return 0;
}

void hd60s_video_unregister(struct hd60s_dev *d)
{
	struct hd60s_mac *m = mac_of(d);

	if (!d->registered)
		return;
	mutex_lock(&d->vlock);
	if (m->open) {
		hd60s_stop_streaming(d);
		hd60s_session_close(d);
	}
	d->registered = false;
	mutex_unlock(&d->vlock);
}

void hd60s_mac_get_mode(struct hd60s_dev *d, struct hd60s_mac_mode *mode)
{
	bool il;

	memset(mode, 0, sizeof(*mode));
	mutex_lock(&d->vlock);
	il = hd60s_interlaced(&d->tm);
	mode->signal = d->signal;
	if (d->signal) {
		mode->width = d->tm.hactive;
		mode->height = d->tm.vactive * (il ? 2 : 1);
		mode->interlaced = il;
		mode->fps = d->tm.fps;
		mode->fractional = d->tm.modeflag & HD60S_MF_FRACTIONAL;
		mode->hdcp = d->tm.modeflag & HD60S_MF_HDCP;
		mode->audio_khz = d->tm.audio_khz;
		mode->adopted = d->cfg_locked && d->fmt.width == mode->width &&
				d->fmt.height == mode->height;
	}
	mutex_unlock(&d->vlock);
}

void hd60s_mac_get_stats(struct hd60s_dev *d, struct hd60s_mac_stats *st)
{
	const struct hd60s_parser *p = &d->p;

	/* Unlocked reads of counters the event thread owns: a snapshot. */
	st->bytes = p->bytes;
	st->frames = p->frames;
	st->partial = p->partial;
	st->resyncs = p->resyncs;
	st->resyncs_at_lock = p->resyncs_at_lock;
	st->parity_err = p->parity_err;
	st->audio_bytes = p->audio_bytes;
	st->iso_total = d->iso_total;
	st->iso_err = d->iso_err;
	st->bulk_transfers = d->bulk_urbs;
	st->bulk_err = d->bulk_err;
	st->resubmit_err = d->resubmit_err;
	st->alt = d->alt;
	st->bulk = d->use_bulk;
}

bool hd60s_mac_present(struct hd60s_dev *d)
{
	return d->udev->state != USB_STATE_NOTATTACHED && !d->wedged;
}

int hd60s_mac_set_audio_input(struct hd60s_dev *d, unsigned int input)
{
	int ret = 0;

	mutex_lock(&d->vlock);
	if (!d->ops->set_audio || input > 1) {
		ret = -EINVAL;
	} else if (input != d->audio_src) {
		ret = d->ops->set_audio(d, input);
		if (ret < 0)
			dev_warn(&d->intf->dev, "audio input %u: %d\n", input, ret);
		else
			d->audio_src = input;
	}
	mutex_unlock(&d->vlock);
	return ret;
}

/* ---- open and close ---------------------------------------------------- */

static const struct usb_device_id *hd60s_match(u16 vid, u16 pid)
{
	const struct usb_device_id *id;

	for (id = kshim_usb_driver->id_table; id->idVendor; id++)
		if (id->idVendor == vid && id->idProduct == pid)
			return id;
	return NULL;
}

struct hd60s_dev *hd60s_mac_open(void)
{
	const struct usb_device_id *id = NULL;
	struct libusb_device_descriptor dd;
	libusb_device **list = NULL;
	libusb_device *dev = NULL;
	struct hd60s_mac *m;
	struct hd60s_dev *d;
	const char *env;
	ssize_t i, n;
	int ret;

	m = calloc(1, sizeof(*m));
	if (!m)
		return NULL;

	ret = libusb_init(&m->ctx);
	if (ret < 0) {
		kshim_log(KSHIM_ERR, "libusb_init: %s\n", libusb_error_name(ret));
		free(m);
		return NULL;
	}

	n = libusb_get_device_list(m->ctx, &list);
	for (i = 0; i < n; i++) {
		if (libusb_get_device_descriptor(list[i], &dd) < 0)
			continue;
		id = hd60s_match(dd.idVendor, dd.idProduct);
		if (id) {
			dev = list[i];
			break;
		}
	}
	if (!dev) {
		kshim_log(KSHIM_ERR, "no Elgato HD60 S found\n");
		goto err_list;
	}

	ret = libusb_open(dev, &m->h);
	if (ret < 0) {
		kshim_log(KSHIM_ERR, "cannot open the device: %s\n",
			  libusb_error_name(ret));
		goto err_list;
	}
	ret = libusb_get_config_descriptor(dev, 0, &m->cfg);
	if (ret < 0 || m->cfg->bNumInterfaces < 1) {
		kshim_log(KSHIM_ERR, "cannot read the configuration descriptor\n");
		goto err_close;
	}

	m->udev.h = m->h;
	m->udev.state = USB_STATE_CONFIGURED;
	m->udev.speed = libusb_get_device_speed(dev) >= LIBUSB_SPEED_SUPER ?
			USB_SPEED_SUPER : USB_SPEED_HIGH;
	m->udev.descriptor.idProduct = dd.idProduct;
	m->udev.priv = m;
	for (i = 0; i < 2; i++) {
		m->intf[i].udev = &m->udev;
		m->intf[i].ifnum = (int)i;
		m->udev.intfs[i] = &m->intf[i];
	}
	libusb_free_device_list(list, 1);
	list = NULL;

	/* Tunable while the macOS transport is being characterised. */
	m->bulk_size = HD60S_BULK_SIZE;
	env = getenv("HD60S_BULK_SIZE");
	if (env && atoi(env) >= 1024)
		m->bulk_size = (unsigned int)atoi(env) & ~1023U;

	if (m->udev.speed < USB_SPEED_SUPER)
		kshim_log(KSHIM_WARN,
			  "connected at USB 2 speed; only low resolutions will work\n");

	atomic_store(&m->ev_run, true);
	if (pthread_create(&m->ev_thread, NULL, hd60s_event_thread, m)) {
		kshim_log(KSHIM_ERR, "cannot start the USB event thread\n");
		goto err_close;
	}

	ret = kshim_usb_driver->probe(&m->intf[0], id);
	if (ret < 0) {
		kshim_log(KSHIM_ERR, "probe failed: %d\n", ret);
		goto err_thread;
	}
	d = usb_get_intfdata(&m->intf[0]);

	/* The cold start runs on the work queue and takes a few seconds. */
	flush_work(&d->init_work);
	if (!d->registered) {
		hd60s_mac_close(d);
		return NULL;
	}
	return d;

err_thread:
	atomic_store(&m->ev_run, false);
	pthread_join(m->ev_thread, NULL);
err_close:
	if (m->cfg)
		libusb_free_config_descriptor(m->cfg);
	libusb_close(m->h);
err_list:
	if (list)
		libusb_free_device_list(list, 1);
	libusb_exit(m->ctx);
	free(m);
	return NULL;
}

void hd60s_mac_close(struct hd60s_dev *d)
{
	struct hd60s_mac *m = mac_of(d);
	int i;

	/* Stops any capture, cancels the poll and frees `d`. */
	kshim_usb_driver->disconnect(&m->intf[0]);

	atomic_store(&m->ev_run, false);
	pthread_join(m->ev_thread, NULL);
	for (i = 0; i < 2; i++)
		libusb_release_interface(m->h, i);
	libusb_free_config_descriptor(m->cfg);
	libusb_close(m->h);
	libusb_exit(m->ctx);
	free(m);
}
