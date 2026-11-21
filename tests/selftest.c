// SPDX-License-Identifier: GPL-2.0
/*
 * Offline test of everything downstream of the USB endpoint: a synthetic
 * EP 0x83 stream (color bars, with blanking lines and audio escapes) is fed
 * through the upstream parser in awkward chunk sizes, and the frame that comes
 * out of the macOS back end is checked and written as a PNG.
 *
 * Needs no device. Both translation units are included whole so the test can
 * reach their statics.
 */
#define main grab_main
#include "../mac/grab.c"
#undef main
#include "../mac/video.c"

#define W	1280
#define H	720
#define VTOTAL	750

/* 75% bars in limited-range BT.709 Y, Cb, Cr */
static const u8 bars[8][3] = {
	{ 180, 128, 128 }, { 168, 44, 136 }, { 145, 147, 44 }, { 133, 63, 52 },
	{ 63, 193, 204 }, { 51, 109, 212 }, { 28, 212, 120 }, { 16, 128, 128 },
};

static size_t put_line(u8 *out, u8 xy, int y)
{
	int x;

	out[0] = 0xff;
	out[1] = 0x00;
	out[2] = 0x00;
	out[3] = xy;
	for (x = 0; x < W; x++) {
		const u8 *b = bars[x * 8 / W];

		out[4 + x * 2] = (xy & 0x20) ? 0x10 : (u8)(b[0] - (y == 0 ? 0 : 0));
		out[4 + x * 2 + 1] = (xy & 0x20) ? 0x80 : b[(x & 1) ? 2 : 1];
	}
	return 4 + W * 2;
}

static size_t put_audio(u8 *out, u8 dwords)
{
	out[0] = 0xff;
	out[1] = 0x00;
	out[2] = 0xff;
	out[3] = dwords;
	memset(out + 4, 0x11, dwords * 4);
	return 4 + dwords * 4;
}

int main(void)
{
	static const u32 chunks[] = { 16384, 1, 3, 32768, 1023, 4, 39936, 7 };
	struct grab g = { .want = 1, .lock = PTHREAD_MUTEX_INITIALIZER };
	struct hd60s_mac *m = calloc(1, sizeof(*m));
	struct hd60s_dev *d = calloc(1, sizeof(*d));
	size_t cap = 4 * (size_t)VTOTAL * (4 + W * 2 + 4 + 16), len = 0, off, i;
	u8 *stream = malloc(cap);
	int frame, line, fails = 0;
	u32 audio = 0;

	/* garbage first: the parser has to find its own way in */
	memset(stream, 0x5a, 4096);
	len = 4096;
	for (frame = 0; frame < 4; frame++) {
		for (line = 0; line < VTOTAL; line++) {
			bool blank = line < 30;

			len += put_line(stream + len, blank ? 0xab : 0x80, line - 30);
			if (line % 5 == 0) {
				len += put_audio(stream + len, 4);
				audio += 16;
			}
		}
	}

	d->udev = &m->udev;
	m->udev.priv = m;
	d->intf = &m->intf[0];
	d->ops = &hd60s_ops_mstar;
	d->quantization = V4L2_QUANTIZATION_LIM_RANGE;
	d->cfg = (struct hd60s_timing){ .htotal = 1650, .vtotal = VTOTAL,
					.hactive = W, .vactive = H, .fps = 60,
					.modeflag = 0x10 /* BT.709 */ };
	hd60s_update_format(d);
	m->frame = malloc(d->fmt.sizeimage);
	m->cb = (struct hd60s_mac_callbacks){ .ctx = &g, .frame = on_frame,
					      .audio = on_audio };
	hd60s_parser_reset(d);

	for (off = 0, i = 0; off < len; i++) {
		u32 n = chunks[i % ARRAY_SIZE(chunks)];

		if (n > len - off)
			n = (u32)(len - off);
		hd60s_parse(&d->p, stream + off, n);
		off += n;
	}

#define CHECK(cond, ...)	\
	do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

	/* frames 0-2 end at the next frame's blanking; the 4th never closes */
	CHECK(d->p.frames == 3, "frames %u, expected 3", d->p.frames);
	CHECK(d->p.partial == 0, "partial %u", d->p.partial);
	CHECK(d->p.parity_err == 0, "parity errors %u", d->p.parity_err);
	CHECK(d->p.resyncs == d->p.resyncs_at_lock, "resyncs after lock: %u",
	      d->p.resyncs - d->p.resyncs_at_lock);
	CHECK(atomic_load(&g.audio_bytes) == audio, "audio %llu, expected %u",
	      atomic_load(&g.audio_bytes), audio);
	CHECK(g.have, "frame 1 was not delivered");
	if (g.have) {
		const u8 *px = g.info.data + (size_t)(H - 1) * g.info.stride + (W - 2) * 2;

		CHECK(g.info.width == W && g.info.height == H && g.info.bt709 &&
		      !g.info.full_range, "frame geometry %ux%u", g.info.width,
		      g.info.height);
		CHECK(g.info.data[0] == 180 && g.info.data[1] == 128,
		      "first pixel %u %u", g.info.data[0], g.info.data[1]);
		CHECK(px[0] == 16 && px[1] == 128, "last pixel pair %u %u", px[0], px[1]);
		CHECK(write_png("build/selftest.png", &g.info), "PNG not written");
	}
	printf(fails ? "selftest: %d failure(s)\n" : "selftest: ok (build/selftest.png)\n",
	       fails);
	return fails != 0;
}
