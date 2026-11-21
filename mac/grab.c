// SPDX-License-Identifier: GPL-2.0
/*
 * hd60s-grab: bring the HD60 S up, report what it sees on HDMI, capture for a
 * moment and save one frame as a PNG. The first end-to-end test of the macOS
 * port.
 */
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "hd60s-mac.h"

static volatile sig_atomic_t interrupted;

static void on_sigint(int sig)
{
	(void)sig;
	interrupted = 1;
}

static uint64_t now_ns(void)
{
	return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

static void sleep_ms(unsigned int ms)
{
	usleep(ms * 1000);
}

struct grab {
	uint32_t want;			/* sequence number of the frame to keep */
	pthread_mutex_t lock;
	struct hd60s_mac_frame info;
	uint8_t *copy;
	bool have;
	atomic_uint frames;
	atomic_ullong audio_bytes;
	atomic_ullong first_ns, last_ns;
	atomic_bool changed;
};

static void on_frame(void *ctx, const struct hd60s_mac_frame *f)
{
	struct grab *g = ctx;

	if (!atomic_load(&g->frames))
		atomic_store(&g->first_ns, f->timestamp_ns);
	atomic_store(&g->last_ns, f->timestamp_ns);
	atomic_fetch_add(&g->frames, 1);

	if (f->sequence == g->want) {
		pthread_mutex_lock(&g->lock);
		if (!g->have) {
			g->copy = malloc(f->size);
			if (g->copy) {
				memcpy(g->copy, f->data, f->size);
				g->info = *f;
				g->info.data = g->copy;
				g->have = true;
			}
		}
		pthread_mutex_unlock(&g->lock);
	}
}

static void on_audio(void *ctx, const uint8_t *pcm, uint32_t len)
{
	struct grab *g = ctx;

	(void)pcm;
	atomic_fetch_add(&g->audio_bytes, len);
}

static void on_source_change(void *ctx)
{
	struct grab *g = ctx;

	atomic_store(&g->changed, true);
}

static uint8_t clamp8(float v)
{
	return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)(v + 0.5f);
}

/* YUY2 to 8-bit RGBX, honouring the frame's matrix and range. */
static uint8_t *yuy2_to_rgbx(const struct hd60s_mac_frame *f)
{
	float kr = f->bt709 ? 0.2126f : 0.299f;
	float kb = f->bt709 ? 0.0722f : 0.114f;
	float kg = 1.0f - kr - kb;
	float ys = f->full_range ? 1.0f : 255.0f / 219.0f;
	float cs = f->full_range ? 1.0f : 255.0f / 224.0f;
	float yo = f->full_range ? 0.0f : 16.0f;
	uint8_t *rgb = malloc((size_t)f->width * f->height * 4);
	uint32_t x, y;

	if (!rgb)
		return NULL;
	for (y = 0; y < f->height; y++) {
		const uint8_t *s = f->data + (size_t)y * f->stride;
		uint8_t *o = rgb + (size_t)y * f->width * 4;

		for (x = 0; x < f->width; x++) {
			float Y = (s[x * 2] - yo) * ys;
			float cb = (s[(x & ~1U) * 2 + 1] - 128.0f) * cs;
			float cr = (s[(x & ~1U) * 2 + 3] - 128.0f) * cs;
			float r = Y + 2.0f * (1.0f - kr) * cr;
			float b = Y + 2.0f * (1.0f - kb) * cb;
			float g = (Y - kr * r - kb * b) / kg;

			o[x * 4 + 0] = clamp8(r);
			o[x * 4 + 1] = clamp8(g);
			o[x * 4 + 2] = clamp8(b);
			o[x * 4 + 3] = 255;
		}
	}
	return rgb;
}

static bool write_png(const char *path, const struct hd60s_mac_frame *f)
{
	uint8_t *rgb = yuy2_to_rgbx(f);
	CGColorSpaceRef cs;
	CGContextRef bmp;
	CGImageRef img;
	CFStringRef cfpath;
	CFURLRef url;
	CGImageDestinationRef dst;
	bool ok = false;

	if (!rgb)
		return false;
	cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
	bmp = CGBitmapContextCreate(rgb, f->width, f->height, 8, (size_t)f->width * 4,
				    cs, (CGBitmapInfo)kCGImageAlphaNoneSkipLast);
	img = bmp ? CGBitmapContextCreateImage(bmp) : NULL;
	cfpath = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
	url = CFURLCreateWithFileSystemPath(NULL, cfpath, kCFURLPOSIXPathStyle, false);
	dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
	if (img && dst) {
		CGImageDestinationAddImage(dst, img, NULL);
		ok = CGImageDestinationFinalize(dst);
	}
	if (dst)
		CFRelease(dst);
	CFRelease(url);
	CFRelease(cfpath);
	if (img)
		CGImageRelease(img);
	if (bmp)
		CGContextRelease(bmp);
	CGColorSpaceRelease(cs);
	free(rgb);
	return ok;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: hd60s-grab [-o frame.png] [-n frame] [-t seconds] [-w seconds] [-b] [-i] [-v]\n"
		"  -o FILE  PNG to write (default frame.png)\n"
		"  -n N     save the Nth captured frame (default 30)\n"
		"  -t SEC   capture for at least SEC seconds and report the rate\n"
		"  -w SEC   how long to wait for an HDMI signal (default 10)\n"
		"  -b       use the bulk alt setting instead of isochronous\n"
		"  -i       initialise and report the signal only; do not capture\n"
		"  -v       debug logging\n");
}

int main(int argc, char **argv)
{
	const char *out = "frame.png";
	struct grab g = { .want = 30, .lock = PTHREAD_MUTEX_INITIALIZER };
	struct hd60s_mac_callbacks cb = {
		.ctx = &g, .frame = on_frame, .audio = on_audio,
		.source_change = on_source_change,
	};
	struct hd60s_mac_mode mode;
	struct hd60s_mac_stats st;
	struct hd60s_dev *d;
	bool bulk = false, init_only = false, told = false;
	double seconds = 0, wait = 10, elapsed, fps;
	uint64_t start, deadline;
	int c, ret, rc = 1;

	while ((c = getopt(argc, argv, "o:n:t:w:bivh")) != -1) {
		switch (c) {
		case 'o':
			out = optarg;
			break;
		case 'n':
			g.want = (uint32_t)strtoul(optarg, NULL, 0);
			break;
		case 't':
			seconds = atof(optarg);
			break;
		case 'w':
			wait = atof(optarg);
			break;
		case 'b':
			bulk = true;
			break;
		case 'i':
			init_only = true;
			break;
		case 'v':
			hd60s_mac_set_log(NULL, 7);
			break;
		default:
			usage();
			return c == 'h' ? 0 : 2;
		}
	}

	signal(SIGINT, on_sigint);
	signal(SIGTERM, on_sigint);

	d = hd60s_mac_open();
	if (!d)
		return 1;

	/* The status poll runs every 500 ms; give a source time to lock. */
	start = now_ns();
	for (;;) {
		hd60s_mac_get_mode(d, &mode);
		if ((mode.signal && mode.adopted) || interrupted ||
		    now_ns() - start >= (uint64_t)(wait * 1e9))
			break;
		if (!mode.signal && wait > 10 && !told) {
			fprintf(stderr, "waiting up to %.0f s for an HDMI signal...\n", wait);
			told = true;
		}
		sleep_ms(250);
	}
	hd60s_mac_get_mode(d, &mode);
	if (!mode.signal) {
		fprintf(stderr, "no HDMI signal detected\n");
		goto out;
	}
	printf("signal: %ux%u%s %u%s Hz, audio %u kHz%s\n", mode.width, mode.height,
	       mode.interlaced ? "i" : "p", mode.fps, mode.fractional ? "/1.001" : "",
	       mode.audio_khz, mode.hdcp ? ", HDCP protected" : "");
	if (init_only || interrupted) {
		rc = 0;
		goto out;
	}

	ret = hd60s_mac_start(d, &cb, bulk);
	if (ret < 0) {
		fprintf(stderr, "cannot start the capture: %s\n", strerror(-ret));
		goto out;
	}

	start = now_ns();
	deadline = start + (uint64_t)((seconds > 12 ? seconds : 12) * 1e9) + 3000000000ULL;
	while (!interrupted && now_ns() < deadline) {
		bool have;

		pthread_mutex_lock(&g.lock);
		have = g.have;
		pthread_mutex_unlock(&g.lock);
		if (have && (now_ns() - start) >= (uint64_t)(seconds * 1e9))
			break;
		sleep_ms(20);
	}

	hd60s_mac_get_stats(d, &st);
	hd60s_mac_stop(d);

	elapsed = (double)(atomic_load(&g.last_ns) - atomic_load(&g.first_ns)) / 1e9;
	fps = elapsed > 0 ? (atomic_load(&g.frames) - 1) / elapsed : 0;
	printf("captured %u frames in %.2f s (%.2f fps), %.1f MB/s on alt %d (%s)\n",
	       atomic_load(&g.frames), (now_ns() - start) / 1e9, fps,
	       (double)st.bytes / ((now_ns() - start) / 1e9) / 1e6, st.alt,
	       st.bulk ? "bulk" : "isochronous");
	printf("incomplete frames %u, resyncs %u (%u before lock), parity errors %u, audio %llu bytes\n",
	       st.partial, st.resyncs, st.resyncs_at_lock, st.parity_err,
	       atomic_load(&g.audio_bytes));
	if (st.bulk)
		printf("bulk transfers %u, failed %u, resubmit failures %u\n",
		       st.bulk_transfers, st.bulk_err, st.resubmit_err);
	else
		printf("iso packets %u, bad %u, resubmit failures %u\n",
		       st.iso_total, st.iso_err, st.resubmit_err);
	if (atomic_load(&g.changed))
		printf("note: the source changed mode during the capture\n");

	if (!g.have) {
		fprintf(stderr, "frame %u never arrived\n", g.want);
		goto out;
	}
	if (!write_png(out, &g.info)) {
		fprintf(stderr, "cannot write %s\n", out);
		goto out;
	}
	printf("wrote %s (%ux%u, %s, %s range)\n", out, g.info.width, g.info.height,
	       g.info.bt709 ? "BT.709" : "BT.601", g.info.full_range ? "full" : "limited");
	rc = 0;
out:
	hd60s_mac_close(d);
	free(g.copy);
	return rc;
}
