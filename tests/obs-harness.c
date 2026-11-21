// SPDX-License-Identifier: GPL-2.0
/*
 * Drives the OBS plugin's source without OBS: the plugin is compiled in whole,
 * linked against the real libobs for its utility functions, and the three calls
 * that hand data to OBS are replaced here with counters. Needs the device and a
 * live HDMI source.
 */
#include "../obs/hd60s-obs.c"

static struct obs_source_info info;
static atomic_uint v_frames, v_clears, v_bad;
static atomic_ullong a_frames;
static atomic_uint v_w, v_h;
static uint64_t last_ts;

void obs_register_source_s(const struct obs_source_info *i, size_t size)
{
	memcpy(&info, i, size < sizeof(info) ? size : sizeof(info));
}

void obs_source_output_video(obs_source_t *source, const struct obs_source_frame *f)
{
	UNUSED_PARAMETER(source);
	if (!f) {
		atomic_fetch_add(&v_clears, 1);
		return;
	}
	if (f->format != VIDEO_FORMAT_YUY2 || !f->data[0] ||
	    f->linesize[0] != f->width * 2 || f->timestamp <= last_ts ||
	    f->color_matrix[0] == 0.0f)
		atomic_fetch_add(&v_bad, 1);
	last_ts = f->timestamp;
	atomic_store(&v_w, f->width);
	atomic_store(&v_h, f->height);
	atomic_fetch_add(&v_frames, 1);
}

void obs_source_output_audio(obs_source_t *source, const struct obs_source_audio *a)
{
	UNUSED_PARAMETER(source);
	atomic_fetch_add(&a_frames, a->frames);
}

static unsigned int wait_frames(unsigned int more, int timeout_s)
{
	unsigned int start = atomic_load(&v_frames);
	int i;

	for (i = 0; i < timeout_s * 10 && atomic_load(&v_frames) < start + more; i++)
		os_sleep_ms(100);
	return atomic_load(&v_frames) - start;
}

static int fails;

static void expect(bool ok, const char *what)
{
	printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		fails++;
}

int main(void)
{
	obs_data_t *settings = obs_data_create();
	unsigned int n, before;
	unsigned long long a0;
	uint64_t t;
	void *s;

	obs_module_load();
	info.get_defaults(settings);
	s = info.create(settings, (obs_source_t *)&info);

	expect(wait_frames(1, 25) >= 1, "first frame arrives after create");
	before = atomic_load(&v_frames);
	a0 = atomic_load(&a_frames);
	os_sleep_ms(5000);
	n = atomic_load(&v_frames) - before;
	printf("      %u frames and %llu audio frames in 5 s at %ux%u\n", n,
	       atomic_load(&a_frames) - a0, atomic_load(&v_w), atomic_load(&v_h));
	expect(n > 100, "frames flow steadily");
	expect(atomic_load(&a_frames) - a0 > 200000, "audio flows at about 48 kHz");

	obs_data_set_bool(settings, S_BULK, true);
	info.update(s, settings);
	os_sleep_ms(1500);
	expect(wait_frames(60, 15) >= 60, "stream restarts on bulk after the setting changes");

	obs_data_set_int(settings, S_AUDIO_INPUT, 1);
	info.update(s, settings);
	os_sleep_ms(1500);
	obs_data_set_int(settings, S_AUDIO_INPUT, 0);
	obs_data_set_bool(settings, S_BULK, false);
	info.update(s, settings);
	os_sleep_ms(1500);
	expect(wait_frames(60, 15) >= 60, "back on isochronous with HDMI audio");

	expect(atomic_load(&v_bad) == 0, "every frame was well formed, timestamps rising");

	t = os_gettime_ns();
	info.destroy(s);
	printf("      destroy took %.2f s\n", (os_gettime_ns() - t) / 1e9);
	n = atomic_load(&v_frames);
	os_sleep_ms(500);
	expect(atomic_load(&v_frames) == n, "nothing arrives after destroy");
	expect(atomic_load(&v_clears) >= 1, "the picture was cleared on stop");

	obs_data_release(settings);
	printf(fails ? "obs harness: %d failure(s)\n" : "obs harness: ok\n", fails);
	return fails != 0;
}
