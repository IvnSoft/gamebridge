// SPDX-License-Identifier: GPL-2.0
/* The GameBridge's BT.656 frame assembler. See gb-bt656.h for why this is a
 * separate, kernel-agnostic translation unit.
 *
 * THE WIRE FORMAT, because everything below follows from it.
 *
 * The CX25840 emits raw ITU-R BT.656 and the FX2 forwards it verbatim. There is
 * no vendor framing and no per-packet header -- which is why none was ever
 * found. Each line is bracketed by 4-byte timing reference codes
 *
 *     FF 00 00 XY        XY = 1 F V H PPPP
 *
 *     F = field   (0 = field 1, 1 = field 2)
 *     V = 1 during vertical blanking
 *     H = 0 for SAV (start of active video), 1 for EAV (end)
 *
 * This device zeroes the four protection bits, so a valid code has a zero low
 * nibble. Between a SAV and the next EAV sit 1440 bytes: 720 pixels of 4:2:2.
 *
 * MOST EAVs are followed by an ancillary-data packet occupying the horizontal
 * blanking interval:
 *
 *     00 FF FF  DID SDID DC  <4 * (DC & 0x0f) payload bytes>  <checksum>
 *
 * DID is always 0xbf; DC is 0x42 (15-byte block) or 0x44 (23-byte). Where a
 * block is present it lands exactly on the next SAV, so the length rule is
 * exact -- but a line may carry NO block at all (24% of them in one capture),
 * so the header must be checked and never assumed.
 *
 * That payload is 48 kHz 16-bit big-endian stereo PCM. It is arbitrary bytes,
 * and silence sits at 0x0000/0xFFFF, so it SPELLS FF 00 00 xy -- 177 times in a
 * 4 MB capture, 6% of all lines. Scanning it as video invents timing codes
 * carrying whatever F and V bits the audio happened to hold, and that is the
 * entire reason both bits were once believed unreliable. With the block skipped
 * by its declared length instead of scanned, every vertical-blanking run in
 * every capture is exactly 19 lines and the F bit flips within one line of each
 * true field boundary.
 */
#include "gb-bt656.h"

enum { ANC_NONE = 0, ANC_H0, ANC_H1, ANC_H2, ANC_DID, ANC_SDID, ANC_DC, ANC_DATA };

/* All-zero UYVY is NOT black. U=0,Y=0,V=0 converts to RGB(0,135,0), a strong
 * green, so every line the decoder fails to fill shows up as a bright green
 * scanline rather than disappearing into a dark picture. That was a useful
 * accident while debugging and is unacceptable in output. Real black is neutral
 * chroma with luma at the 0x10 black level. */
void gb_bt656_blank(struct gb_bt656 *s)
{
	int frame_bytes = s->height * GB_LINE_BYTES;
	int i;

	memset(s->rowmap, 0, sizeof s->rowmap);
	for (i = 0; i < frame_bytes; i += 4) {
		s->frame[i + 0] = 0x80;   /* Cb */
		s->frame[i + 1] = 0x10;   /* Y  */
		s->frame[i + 2] = 0x80;   /* Cr */
		s->frame[i + 3] = 0x10;   /* Y  */
	}
}

void gb_bt656_init(struct gb_bt656 *s, unsigned char *frame, int field_lines,
                   int progressive, int pal,
                   const struct gb_bt656_ops *ops, void *ctx)
{
	memset(s, 0, sizeof *s);
	s->field_lines  = field_lines;
	s->progressive  = progressive;
	s->pal          = pal;
	s->height       = field_lines * (progressive ? 1 : 2);
	s->frame        = frame;
	s->ops          = ops;
	s->ctx          = ctx;
	gb_bt656_blank(s);
}

/* Emit one ancillary block's payload as interleaved stereo.
 *
 * The block is NOT stored interleaved. A 16-byte payload is four consecutive
 * samples of one channel followed by four of the other -- AAAA BBBB, not
 * ABABABAB -- so writing it out verbatim hands a player the channels shuffled.
 *
 * Read as ABABABAB the two "channels" came out with RMS equal to four
 * significant figures and correlation +0.99, and that was twice reported as
 * "the device captures mono". It is not. Split by block half on the same music
 * the halves measure RMS 301 against 265 and correlate at +0.88, and with one
 * input physically disconnected they decorrelate completely.
 *
 * The first half is LEFT and the second is RIGHT, established by disconnecting
 * the right input: first half RMS 808 with a musical spectrum, second half RMS
 * 1.22 with a peak of 3 -- LSB dither and nothing else, a ratio of 663 to 1.
 * That is not a judgement call, so the order below is correct as written. */
static void anc_emit_audio(struct gb_bt656 *s, const unsigned char *p, int len)
{
	int half = len / 2;
	int i, n = 0;

	if (half & 1)                    /* not a whole number of 16-bit samples */
		return;
	if (!s->ops->audio)
		return;

	for (i = 0; i < half; i += 2) {
		s->pcm_out[n++] = p[i];             /* LEFT, sample i/2 */
		s->pcm_out[n++] = p[i + 1];
		s->pcm_out[n++] = p[half + i];      /* RIGHT, same instant */
		s->pcm_out[n++] = p[half + i + 1];
	}
	s->ops->audio(s->ctx, s->pcm_out, n);
}

/* A frame is only as good as the two fields that built it, and a field that is
 * not field-length is not one. cur_run is the field just finished and runs[3]
 * the one before it -- the pair this frame is made of, since the shift into
 * runs[] happens after this call.
 *
 * TOO SHORT catches the start of a capture, which begins wherever the device
 * happens to be in the raster: measured over 210 NTSC frames, the first accepted
 * frame was built from runs of 120 and 202 lines, sat 10 rows high and had 43
 * unwritten rows, and was the only frame out of position among 210.
 *
 * TOO LONG means the split rule fired, which only happens when a field boundary
 * was missed entirely -- so the frame is a guess. Measured over 173 PAL frames,
 * four were displaced by 128 to 172 rows and all four were built from runs of
 * 296 while the decoder re-locked to the new standard. The other 169 held
 * position to 0.11 rows.
 *
 * A real field is 243/244 lines on 525/60 and 288 on 625/50, and the window is
 * field_lines +/- 8 either way, so both have room. */
static void bt656_flush(struct gb_bt656 *s)
{
	int lo = s->field_lines - 8, hi = s->field_lines + 8;
	int accepted = !(s->cur_run < lo || s->runs[3] < lo ||
	                 s->cur_run >= hi || s->runs[3] >= hi)
	               && s->lines >= s->field_lines;

	s->attempts++;
	if (!accepted)
		s->short_frames++;
	if (s->ops->frame_done)
		s->ops->frame_done(s->ctx, s, accepted);
}

void gb_bt656_flush(struct gb_bt656 *s)
{
	bt656_flush(s);
}

void gb_bt656_feed(struct gb_bt656 *s, const unsigned char *p, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		unsigned char c = p[i];

		/* Ancillary data: consumed, never scanned. A mismatch anywhere in the
		 * header means this was not an ancillary packet after all, so fall
		 * through and treat the byte as ordinary stream content. */
		if (s->anc) {
			switch (s->anc) {
			case ANC_H0:   if (c == 0x00) { s->anc = ANC_H1;   continue; } break;
			case ANC_H1:   if (c == 0xff) { s->anc = ANC_H2;   continue; } break;
			case ANC_H2:   if (c == 0xff) { s->anc = ANC_DID;  continue; } break;
			case ANC_DID:  s->anc_did = c;   s->anc = ANC_SDID; continue;
			case ANC_SDID:                   s->anc = ANC_DC;   continue;
			case ANC_DC:   s->anc_skip = 4 * (c & 0x0f) + 1;
			               /* everything but the trailing checksum is audio,
			                * and only DID 0xbf is a real block */
			               s->anc_keep = (s->anc_did == 0xbf)
			                             ? s->anc_skip - 1 : 0;
			               if (s->anc_keep > (int)sizeof s->anc_pcm)
			                       s->anc_keep = 0;
			               s->anc_pcm_len = 0;
			               s->anc = ANC_DATA;                   continue;
			case ANC_DATA: if (s->anc_keep > 0) {
			                       s->anc_pcm[s->anc_pcm_len++] = c;
			                       s->anc_keep--;
			               }
			               if (--s->anc_skip <= 0) {
			                       /* whole block in hand: de-interleave it */
			                       if (s->anc_pcm_len)
			                               anc_emit_audio(s, s->anc_pcm,
			                                              s->anc_pcm_len);
			                       s->anc_pcm_len = 0;
			                       s->anc = ANC_NONE;
			               }
			                                                    continue;
			}
			s->anc = ANC_NONE;
		}

		/* Always run the preamble matcher, even mid-line. BT.656 reserves 0x00
		 * and 0xFF -- active video is 0x01..0xFE -- so FF 00 00 inside what we
		 * think is a line is never data: it means we lost sync, almost always
		 * because an isochronous packet was dropped and we kept copying through
		 * the gap, swallowing the next line's preamble. Resyncing here rather
		 * than trusting the byte count is the difference between a third of the
		 * frames and nearly all of them. */
		if (s->match == 3) {
			s->match = 0;
			if ((c & 0x80) && (c & 0x0f) == 0) {
				int field  = (c >> 6) & 1;
				int vblank = (c >> 5) & 1;
				int eav    = (c >> 4) & 1;

				s->copying = 0;          /* abandon any half-copied line */

				/* An ancillary packet follows every EAV; skip it whole
				 * rather than scan it. This one line is what made the F
				 * and V bits trustworthy. */
				if (eav) {
					s->anc = ANC_H0;
					continue;
				}

				/* Vertical blanking consumes no row index: at 23 or 31
				 * bytes per line the entire interval is under 600 bytes,
				 * less than one isochronous packet -- so a single lost
				 * packet can still swallow a whole field boundary. That
				 * is what the F-bit fallback below is for. */
				if (vblank) {
					s->in_vblank = 1;
					continue;
				}

				/* ONE anchor per field, and it is the F transition.
				 *
				 * The obvious anchor is the V 1->0 edge, and using it
				 * where it exists with F only as a fallback is what an
				 * earlier version did. It is wrong, because the two
				 * anchors do not sit at the same place -- so fields
				 * alternated between two origins two lines apart.
				 * Measured: 35% of frames had their two fields displaced
				 * from EACH OTHER by 2 field rows, and the displaced
				 * field ran off the bottom leaving unfilled rows there.
				 *
				 * The F bit is on every line, so it survives a lost
				 * blanking interval intact. Anchoring on it alone makes
				 * the two cases identical, because blanking lines
				 * consume no row index. The V bit is left as a backstop
				 * for a field whose F transition was lost with a packet.
				 *
				 * REFUTED, with numbers -- do not retry: snapping
				 * line_of to a parity-dependent constant at the end of a
				 * surviving blanking interval made field 1 twice as
				 * unsteady (centroid stdev 0.396 -> 0.791) and the two
				 * fields' separation half again as noisy (0.626 ->
				 * 0.950). The F transition alone beat every snap tried. */
				int boundary = 0;
				if (!s->started) {
					boundary = 1;
				} else if (field != s->cur_field) {
					boundary = 1;
				} else if (s->in_vblank &&
				           s->line_of >= s->field_lines / 2) {
					/* Blanking ended but F never changed: the transition
					 * was lost. Anchor here rather than run two fields
					 * together. */
					boundary = 1;
					field = !s->cur_field;
				} else if (s->line_of >= s->field_lines + 8) {
					/* Both lost. Split anyway rather than throw the
					 * second field away. */
					boundary = 1;
					field = !s->cur_field;
				}
				s->in_vblank = 0;

				/* An implausibly short field is never real; keep filling
				 * the current one rather than toggle parity on it. */
				if (boundary && s->started &&
				    s->line_of < s->field_lines / 2)
					boundary = 0;

				if (boundary) {
					s->started = 1;
					s->cur_field = field;
					if (s->progressive || s->cur_field == 0) {
						bt656_flush(s);
						gb_bt656_blank(s);
						s->lines = 0;
					}
					s->runs[0] = s->runs[1]; s->runs[1] = s->runs[2];
					s->runs[2] = s->runs[3]; s->runs[3] = s->cur_run;
					s->cur_run = 0;
					s->line_of = 0;
				}

				/* Progressive: one field IS the frame, lines consecutive.
				 * This path was silently lost in an earlier rewrite, which
				 * left --progressive writing the first ~120 lines of a field
				 * onto the even rows of a 240-row frame -- the picture
				 * appeared as its own top half, stretched, with black lines
				 * between.
				 *
				 * Interlace, and the sign in front of the field is the whole
				 * of it. It depends on the STANDARD, because it comes from
				 * the parity of the chip's active-line count:
				 *
				 *   525/60  vactive 487, ODD  -> 243 + 244 lines, and field
				 *           2's line k pairs with field 1's k+1.
				 *           row = 2*line - F. Field 2's first line lands on
				 *           row -1 and is dropped: that is the half-line,
				 *           and dropping it is correct.
				 *   625/50  vactive 576, EVEN -> 288 + 288, fields pair up
				 *           directly. row = 2*line + F.
				 *
				 * Measured on each, by the offset at which the two fields'
				 * content agrees. NTSC: 2.03 at -1 against 4.85 at 0 with the
				 * wrong sign, and vertical high-frequency energy down 29%.
				 * PAL: 3.47 at +1 against 9.02 at 0 with NTSC's sign. Getting
				 * this wrong is the "everything looks doubled" picture, and on
				 * PAL it also left row 575 permanently unwritten. */
				if (s->progressive)
					s->row = s->line_of++;
				else if (s->pal)
					s->row = 2 * s->line_of++ + s->cur_field;
				else
					s->row = 2 * s->line_of++ - s->cur_field;
				s->cur_run++;
				if (s->row >= 0 && s->row < s->height) {
					s->rowmap[s->row >> 3] |= 1u << (s->row & 7);
					s->lines++;
				}
				s->col     = 0;
				s->copying = GB_LINE_BYTES;
				continue;
			}
			/* Not a valid code. It may itself begin the next preamble. */
			if (c == 0xFF) { s->match = 1; continue; }
			/* otherwise fall through and treat it as payload */
		}

		if (c == 0xFF && s->match == 0)      { s->match = 1; continue; }
		if (c == 0x00 && s->match == 1)      { s->match = 2; continue; }
		if (c == 0x00 && s->match == 2)      { s->match = 3; continue; }
		if (s->match) {
			/* Partial match that did not pan out: the bytes we swallowed were
			 * real payload, but at most three of them, which is not worth
			 * reconstructing for a 1440-byte line. */
			s->match = (c == 0xFF) ? 1 : 0;
			if (s->match) continue;
		}

		/* Bounds-check every single write. In the kernel this is the only thing
		 * standing between a corrupt or hostile stream and writing past the end
		 * of a videobuf2 plane. */
		if (s->copying && s->row >= 0 && s->row < s->height &&
		    s->col < GB_LINE_BYTES) {
			s->frame[s->row * GB_LINE_BYTES + s->col++] = c;
			s->copying--;
		}
	}
}
