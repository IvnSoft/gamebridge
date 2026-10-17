// SPDX-License-Identifier: GPL-2.0
/* fuzz-bt656 -- libFuzzer harness for the BT.656 frame assembler.
 *
 * driver/gb-bt656.c is the only code in the project that makes memory-write
 * decisions from bytes the DEVICE supplied: timing codes decide the row, and an
 * ancillary block's own length field decides how much is skipped or kept as
 * audio. A malfunctioning or hostile device controls all of it. This feeds the
 * same source the kernel module compiles -- unmodified -- with arbitrary
 * streams, under AddressSanitizer and UBSan.
 *
 * What it drives the way the driver does:
 *   - the configuration: 525/60 or 625/50, interlaced or progressive, from the
 *     first input byte;
 *   - resumability: the rest is fed in chunks of varying size, because real
 *     packets are 3030 bytes and lines are 1440, so the parser must stop and
 *     resume at any byte;
 *   - buffer hand-over: on every accepted frame, s->frame is swapped for a
 *     fresh, EXACTLY-sized allocation, as gb_frame_done() swaps vb2 planes.
 *     An exact size is what lets ASan see a write one byte past the end;
 *   - stop: gb_bt656_flush() at the end, as stop_streaming() does.
 *
 * What it does not cover: the URB handling around the assembler, the RCU and
 * locking that hand buffers over in the kernel, and timing. Those were tested
 * on hardware under KASAN; this is the parser alone.
 *
 * Build and run (clang with compiler-rt):
 *   make -C tools/fuzz
 *   make -C tools/fuzz run SECS=3600
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "gb-bt656.h"

static unsigned char *frames[2];
static int cur;
static size_t frame_bytes;
static volatile unsigned audio_sum;

static void on_frame(void *ctx, struct gb_bt656 *s, int accepted)
{
	(void)ctx;
	if (!accepted)
		return;
	/* The finished frame goes to the consumer and a different buffer is
	 * handed in -- read every byte of the old one first, so a stale or short
	 * buffer would be caught here. */
	unsigned sum = 0;
	for (size_t i = 0; i < frame_bytes; i++)
		sum += frames[cur][i];
	audio_sum += sum;
	cur ^= 1;
	s->frame = frames[cur];
}

static void on_audio(void *ctx, const unsigned char *pcm, int bytes)
{
	(void)ctx;
	/* One ancillary block: never negative, never more than the 64-byte
	 * buffers it came from, and always whole stereo frames. */
	if (bytes < 0 || bytes > 64 || bytes % 4)
		abort();
	for (int i = 0; i < bytes; i++)
		audio_sum += pcm[i];
}

static const struct gb_bt656_ops ops = {
	.frame_done = on_frame,
	.audio      = on_audio,
};

/* Chunk sizes the device and the tools actually produce, plus awkward ones. */
static const int chunks[] = { 3030, 1, 2, 3, 7, 1010, 2020, 1440, 4, 65536 };

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size < 2)
		return 0;

	int pal         = data[0] & 1;
	int progressive = (data[0] >> 1) & 1;
	int pattern     = data[1] % (int)(sizeof(chunks) / sizeof(chunks[0]) + 1);
	int field_lines = pal ? 288 : 240;
	int height      = progressive ? field_lines : field_lines * 2;

	data += 2;
	size -= 2;

	frame_bytes = (size_t)height * GB_LINE_BYTES;
	frames[0] = malloc(frame_bytes);
	frames[1] = malloc(frame_bytes);
	if (!frames[0] || !frames[1])
		abort();
	cur = 0;

	/* The state struct on the heap too, so an overrun of its own arrays
	 * (anc_pcm, pcm_out, rowmap) is caught as well. */
	struct gb_bt656 *s = malloc(sizeof(*s));
	if (!s)
		abort();
	gb_bt656_init(s, frames[0], field_lines, progressive, pal, &ops, NULL);

	size_t off = 0;
	unsigned k = 0;
	while (off < size) {
		size_t n = pattern == 0 ? size
			 : (size_t)chunks[(pattern - 1 + k++) % (sizeof(chunks) / sizeof(chunks[0]))];
		if (n > size - off)
			n = size - off;
		gb_bt656_feed(s, data + off, (int)n);
		off += n;
	}
	gb_bt656_flush(s);

	free(s);
	free(frames[0]);
	free(frames[1]);
	return 0;
}
