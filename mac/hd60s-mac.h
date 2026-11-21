/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Elgato Game Capture HD60 S on macOS: the capture library's public surface.
 *
 * The hardware logic underneath is the unmodified Linux driver in upstream/;
 * this is what replaces its V4L2 and ALSA front ends.
 */
#ifndef _HD60S_MAC_H_
#define _HD60S_MAC_H_

#include <stdbool.h>
#include <stdint.h>

struct hd60s_dev;

struct hd60s_mac_mode {
	bool signal;
	bool adopted;		/* the capture format matches the detected mode */
	uint32_t width, height;	/* full frame, both fields when interlaced */
	bool interlaced;
	uint32_t fps;		/* field rate when interlaced */
	bool fractional;	/* rate is fps * 1000/1001 */
	bool hdcp;
	uint32_t audio_khz;
};

/* One complete YUY2 frame. `data` is only valid during the callback. */
struct hd60s_mac_frame {
	const uint8_t *data;
	uint32_t width, height;
	uint32_t stride;	/* bytes per line, width * 2 */
	uint32_t size;
	bool interlaced;	/* the two fields are already woven */
	bool bt709;		/* else BT.601 */
	bool full_range;	/* else 16-235 */
	uint32_t sequence;
	uint64_t timestamp_ns;	/* CLOCK_UPTIME_RAW */
};

/* All three run on the USB event thread and must not block. */
struct hd60s_mac_callbacks {
	void *ctx;
	void (*frame)(void *ctx, const struct hd60s_mac_frame *f);
	/* s16 little-endian stereo at 48 kHz */
	void (*audio)(void *ctx, const uint8_t *pcm, uint32_t len);
	/* signal appeared, vanished or changed mode */
	void (*source_change)(void *ctx);
};

struct hd60s_mac_stats {
	uint64_t bytes;
	uint32_t frames, partial, resyncs, resyncs_at_lock, parity_err;
	uint32_t audio_bytes;
	uint32_t iso_total, iso_err;
	uint32_t bulk_transfers, bulk_err;
	uint32_t resubmit_err;
	int alt;
	bool bulk;
};

/*
 * Find the device, run the cold start and wait for it to finish. Takes a few
 * seconds. NULL if there is no device or it did not come up.
 */
struct hd60s_dev *hd60s_mac_open(void);
/* Stops any capture and releases the device. `d` is invalid afterwards. */
void hd60s_mac_close(struct hd60s_dev *d);

void hd60s_mac_get_mode(struct hd60s_dev *d, struct hd60s_mac_mode *mode);

/*
 * Start capturing in the currently detected mode. Isochronous by default, as
 * the Linux and Windows drivers; `force_bulk` selects the bulk alt setting.
 */
int hd60s_mac_start(struct hd60s_dev *d, const struct hd60s_mac_callbacks *cb,
		    bool force_bulk);
void hd60s_mac_stop(struct hd60s_dev *d);
void hd60s_mac_get_stats(struct hd60s_dev *d, struct hd60s_mac_stats *st);

/* False once the device has been unplugged or has stopped answering. */
bool hd60s_mac_present(struct hd60s_dev *d);

/* 0 is the audio embedded in HDMI (the default), 1 the 3.5 mm analog jack. */
int hd60s_mac_set_audio_input(struct hd60s_dev *d, unsigned int input);

/*
 * Log levels are syslog's: 3 error, 4 warning, 6 info, 7 debug. Messages above
 * `max_level` are dropped. With no function set they go to stderr.
 */
void hd60s_mac_set_log(void (*fn)(int level, const char *msg), int max_level);

#endif /* _HD60S_MAC_H_ */
