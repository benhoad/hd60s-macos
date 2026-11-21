// SPDX-License-Identifier: GPL-2.0
/*
 * OBS Studio source for the Elgato Game Capture HD60 S on macOS.
 *
 * One capture thread per source owns the device: it runs the cold start, waits
 * for an HDMI signal, starts the stream, and restarts it when the source
 * changes mode, the device is replugged or the stream stalls. Frames and audio
 * are handed to OBS straight from the USB event thread.
 */
#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>

#include <stdatomic.h>

#include "hd60s-mac.h"

OBS_DECLARE_MODULE()

MODULE_EXPORT const char *obs_module_name(void)
{
	return "Elgato HD60 S capture";
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Captures video and audio from the Elgato Game Capture HD60 S "
	       "over USB, without Elgato's software.";
}

#define S_BULK		"bulk"
#define S_AUDIO_INPUT	"audio_input"

#define AUDIO_RATE	48000
#define AUDIO_BATCH	(480 * 4)	/* 10 ms of s16 stereo */

struct hd60s_source {
	obs_source_t *source;
	pthread_t thread;
	bool thread_started;
	os_event_t *wake;

	atomic_bool stop;
	atomic_bool changed;		/* the device reported a source change */
	atomic_bool restart;		/* a setting that needs a new stream */
	atomic_bool bulk;
	atomic_int audio_input;
	atomic_uint frames;

	/* USB event thread only */
	uint8_t abuf[AUDIO_BATCH + 4];
	uint32_t alen;
};

/* There is one device, so one source may drive it. */
static atomic_flag device_claimed = ATOMIC_FLAG_INIT;

static void hd60s_log(int level, const char *msg)
{
	int n = (int)strlen(msg);

	while (n && msg[n - 1] == '\n')
		n--;
	blog(level <= 3 ? LOG_ERROR : level == 4 ? LOG_WARNING :
	     level == 7 ? LOG_DEBUG : LOG_INFO, "[hd60s] %.*s", n, msg);
}

static void on_frame(void *ctx, const struct hd60s_mac_frame *f)
{
	struct hd60s_source *s = ctx;
	struct obs_source_frame frame = {
		.width = f->width,
		.height = f->height,
		.format = VIDEO_FORMAT_YUY2,
		.timestamp = f->timestamp_ns,
		.full_range = f->full_range,
	};

	frame.data[0] = (uint8_t *)f->data;
	frame.linesize[0] = f->stride;
	video_format_get_parameters_for_format(
		f->bt709 ? VIDEO_CS_709 : VIDEO_CS_601,
		f->full_range ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL,
		VIDEO_FORMAT_YUY2, frame.color_matrix, frame.color_range_min,
		frame.color_range_max);

	obs_source_output_video(s->source, &frame);
	atomic_fetch_add(&s->frames, 1);
}

static void flush_audio(struct hd60s_source *s)
{
	uint32_t frames = s->alen / 4, rest = s->alen % 4;
	struct obs_source_audio audio = {
		.frames = frames,
		.speakers = SPEAKERS_STEREO,
		.format = AUDIO_FORMAT_16BIT,
		.samples_per_sec = AUDIO_RATE,
		/* the batch began this long ago */
		.timestamp = os_gettime_ns() -
			     (uint64_t)frames * 1000000000ULL / AUDIO_RATE,
	};

	if (!frames)
		return;
	audio.data[0] = s->abuf;
	obs_source_output_audio(s->source, &audio);
	/* a sample split across two USB packets waits for its other half */
	memmove(s->abuf, s->abuf + frames * 4, rest);
	s->alen = rest;
}

/* The device interleaves audio a few samples at a time; batch it. */
static void on_audio(void *ctx, const uint8_t *pcm, uint32_t len)
{
	struct hd60s_source *s = ctx;

	while (len) {
		uint32_t n = AUDIO_BATCH - s->alen;

		if (n > len)
			n = len;
		memcpy(s->abuf + s->alen, pcm, n);
		s->alen += n;
		pcm += n;
		len -= n;
		if (s->alen >= AUDIO_BATCH)
			flush_audio(s);
	}
}

static void on_source_change(void *ctx)
{
	struct hd60s_source *s = ctx;

	atomic_store(&s->changed, true);
	os_event_signal(s->wake);
}

/* Sleep, but wake at once for a stop, a setting or a source change. */
static void nap(struct hd60s_source *s, unsigned long ms)
{
	if (!atomic_load(&s->stop))
		os_event_timedwait(s->wake, ms);
}

static void run_device(struct hd60s_source *s, struct hd60s_dev *d)
{
	const struct hd60s_mac_callbacks cb = {
		.ctx = s,
		.frame = on_frame,
		.audio = on_audio,
		.source_change = on_source_change,
	};
	struct hd60s_mac_mode mode;
	bool capturing = false;
	int audio_input = 0;		/* what the cold start leaves selected */
	unsigned int seen = 0, quiet = 0;

	while (!atomic_load(&s->stop) && hd60s_mac_present(d)) {
		bool restart = atomic_exchange(&s->restart, false);
		int want_audio = atomic_load(&s->audio_input);

		atomic_store(&s->changed, false);
		hd60s_mac_get_mode(d, &mode);

		if (want_audio != audio_input &&
		    hd60s_mac_set_audio_input(d, (unsigned int)want_audio) == 0)
			audio_input = want_audio;

		/* frames stopped although the source is still there */
		if (capturing) {
			unsigned int now = atomic_load(&s->frames);

			quiet = now == seen ? quiet + 1 : 0;
			seen = now;
			if (quiet >= 15) {
				blog(LOG_WARNING, "[hd60s] stream stalled, restarting it");
				restart = true;
			}
		}

		/*
		 * The stream is tied to the mode it was started in. A source
		 * that changed mode or went away needs it torn down; stopping
		 * also lets the driver adopt the new mode.
		 */
		if (capturing && (restart || !mode.signal || !mode.adopted)) {
			hd60s_mac_stop(d);
			capturing = false;
			s->alen = 0;
			obs_source_output_video(s->source, NULL);
			continue;
		}

		if (!capturing && mode.signal && mode.adopted) {
			int ret = hd60s_mac_start(d, &cb, atomic_load(&s->bulk));

			if (ret == 0) {
				capturing = true;
				quiet = 0;
				seen = atomic_load(&s->frames);
				blog(LOG_INFO, "[hd60s] capturing %ux%u%s %u%s Hz%s",
				     mode.width, mode.height,
				     mode.interlaced ? "i" : "p", mode.fps,
				     mode.fractional ? "/1.001" : "",
				     mode.hdcp ? " (source reports HDCP)" : "");
			} else {
				blog(LOG_WARNING, "[hd60s] cannot start the stream (%d)", ret);
				nap(s, 2000);
			}
		}

		nap(s, 200);
	}

	if (capturing)
		hd60s_mac_stop(d);
	obs_source_output_video(s->source, NULL);
}

static void *capture_thread(void *arg)
{
	struct hd60s_source *s = arg;
	bool told = false;

	os_set_thread_name("hd60s-capture");
	while (!atomic_load(&s->stop)) {
		struct hd60s_dev *d;

		/* quiet while polling for a device that is not plugged in */
		hd60s_mac_set_log(hd60s_log, told ? 2 : 6);
		d = hd60s_mac_open();
		hd60s_mac_set_log(hd60s_log, 6);
		if (!d) {
			told = true;
			nap(s, 3000);
			continue;
		}
		told = false;

		run_device(s, d);
		hd60s_mac_close(d);
		nap(s, 2000);
	}
	return NULL;
}

static void hd60s_update(void *data, obs_data_t *settings)
{
	struct hd60s_source *s = data;
	bool bulk = obs_data_get_bool(settings, S_BULK);

	atomic_store(&s->audio_input, (int)obs_data_get_int(settings, S_AUDIO_INPUT));
	if (atomic_exchange(&s->bulk, bulk) != bulk)
		atomic_store(&s->restart, true);
	os_event_signal(s->wake);
}

static void *hd60s_create(obs_data_t *settings, obs_source_t *source)
{
	struct hd60s_source *s = bzalloc(sizeof(*s));

	s->source = source;
	if (os_event_init(&s->wake, OS_EVENT_TYPE_AUTO) != 0) {
		bfree(s);
		return NULL;
	}
	atomic_store(&s->bulk, obs_data_get_bool(settings, S_BULK));
	atomic_store(&s->audio_input, (int)obs_data_get_int(settings, S_AUDIO_INPUT));

	if (atomic_flag_test_and_set(&device_claimed)) {
		blog(LOG_WARNING,
		     "[hd60s] another HD60 S source already owns the device; '%s' will stay blank",
		     obs_source_get_name(source));
		return s;
	}
	if (pthread_create(&s->thread, NULL, capture_thread, s) != 0) {
		atomic_flag_clear(&device_claimed);
		blog(LOG_ERROR, "[hd60s] cannot start the capture thread");
		return s;
	}
	s->thread_started = true;
	return s;
}

static void hd60s_destroy(void *data)
{
	struct hd60s_source *s = data;

	if (s->thread_started) {
		atomic_store(&s->stop, true);
		os_event_signal(s->wake);
		pthread_join(s->thread, NULL);
		atomic_flag_clear(&device_claimed);
	}
	os_event_destroy(s->wake);
	bfree(s);
}

static const char *hd60s_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Elgato HD60 S";
}

static void hd60s_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, S_BULK, false);
	obs_data_set_default_int(settings, S_AUDIO_INPUT, 0);
}

static obs_properties_t *hd60s_get_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	UNUSED_PARAMETER(data);
	p = obs_properties_add_list(props, S_AUDIO_INPUT, "Audio input",
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, "HDMI", 0);
	obs_property_list_add_int(p, "Analog (3.5 mm jack)", 1);

	p = obs_properties_add_bool(props, S_BULK, "Use bulk USB transfers");
	obs_property_set_long_description(
		p, "Leave off unless the picture stutters or never appears. "
		   "Isochronous transfers, the default, reserve USB bandwidth.");
	return props;
}

static struct obs_source_info hd60s_source_info = {
	.id = "hd60s_capture",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO |
			OBS_SOURCE_DO_NOT_DUPLICATE,
	.icon_type = OBS_ICON_TYPE_CAMERA,
	.get_name = hd60s_get_name,
	.create = hd60s_create,
	.destroy = hd60s_destroy,
	.update = hd60s_update,
	.get_defaults = hd60s_get_defaults,
	.get_properties = hd60s_get_properties,
};

bool obs_module_load(void)
{
	hd60s_mac_set_log(hd60s_log, 6);
	obs_register_source(&hd60s_source_info);
	return true;
}
