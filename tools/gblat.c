// SPDX-License-Identifier: GPL-2.0
#define _POSIX_C_SOURCE 200809L
/* gblat -- how old is a frame by the time userspace can see it?
 *
 * The original point of this hardware is playing a console on a monitor, and a
 * controller is unusable if the picture lags the stick. So the number that
 * matters is not frames per second, it is AGE: the delay between the driver
 * finishing a frame and an application getting its hands on it.
 *
 * The driver stamps every buffer with ktime_get_ns() at the moment the frame
 * assembler completes it, and V4L2 reports that stamp on CLOCK_MONOTONIC. So the
 * age of a frame at dequeue is exactly
 *
 *     clock_gettime(CLOCK_MONOTONIC) - buf.timestamp
 *
 * and that is the driver-and-transport contribution with no player in the way.
 * Anything a player adds on top of this is the player's own buffering, which is
 * where whole tenths of a second usually hide.
 *
 * REQBUFS is 2 by default on purpose -- the smallest depth the driver allows,
 * and the one the low-latency presenter is meant to use. Two buffers is tight:
 * any stall in the consumer drops a frame rather than delaying it, which for
 * this application is the correct trade.
 *
 * Usage: gblat [/dev/videoN] [frames] [nbufs]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

static double ms_since(const struct timeval *tv, const struct timespec *now)
{
	double t = tv->tv_sec + tv->tv_usec / 1e6;
	double n = now->tv_sec + now->tv_nsec / 1e9;
	return (n - t) * 1000.0;
}

static int cmp(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/video1";
	int want = argc > 2 ? atoi(argv[2]) : 300;
	int nbufs = argc > 3 ? atoi(argv[3]) : 2;

	if (want < 5 || want > 1000000 || nbufs < 1) {
		fprintf(stderr, "usage: gblat [/dev/videoN] [frames 5..1000000] [nbufs >= 1]\n");
		return 1;
	}
	struct v4l2_requestbuffers req = { 0 };
	struct v4l2_format fmt = { 0 };
	void *maps[8]; unsigned lens[8];
	double *age, *gap;
	struct timespec prev = { 0 };
	int fd, i, n = 0;

	fd = open(dev, O_RDWR);
	if (fd < 0) { perror(dev); return 1; }

	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(fd, VIDIOC_G_FMT, &fmt) < 0) { perror("G_FMT"); return 1; }
	printf("%s: %ux%u, %u bytes/frame, %d buffers\n", dev,
	       fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.sizeimage, nbufs);

	req.count = nbufs;
	req.type  = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;
	if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) { perror("REQBUFS"); return 1; }
	printf("driver granted %u buffers\n", req.count);
	if (req.count > sizeof maps / sizeof maps[0]) {
		fprintf(stderr, "granted %u buffers; this tool handles at most %zu\n",
		        req.count, sizeof maps / sizeof maps[0]);
		return 1;
	}
	nbufs = req.count;

	for (i = 0; i < nbufs; i++) {
		struct v4l2_buffer b = { 0 };
		b.type = req.type; b.memory = req.memory; b.index = i;
		if (ioctl(fd, VIDIOC_QUERYBUF, &b) < 0) { perror("QUERYBUF"); return 1; }
		lens[i] = b.length;
		maps[i] = mmap(NULL, b.length, PROT_READ, MAP_SHARED, fd, b.m.offset);
		if (maps[i] == MAP_FAILED) { perror("mmap"); return 1; }
		if (ioctl(fd, VIDIOC_QBUF, &b) < 0) { perror("QBUF"); return 1; }
	}

	int type = req.type;
	if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) { perror("STREAMON"); return 1; }

	age = calloc(want, sizeof *age);
	gap = calloc(want, sizeof *gap);

	while (n < want) {
		struct v4l2_buffer b = { 0 };
		struct timespec now;

		b.type = req.type; b.memory = req.memory;
		if (ioctl(fd, VIDIOC_DQBUF, &b) < 0) {
			if (errno == EAGAIN) continue;
			perror("DQBUF"); break;
		}
		clock_gettime(CLOCK_MONOTONIC, &now);

		age[n] = ms_since(&b.timestamp, &now);
		if (prev.tv_sec)
			gap[n] = (now.tv_sec - prev.tv_sec) * 1000.0 +
				 (now.tv_nsec - prev.tv_nsec) / 1e6;
		prev = now;
		n++;

		if (ioctl(fd, VIDIOC_QBUF, &b) < 0) { perror("QBUF"); break; }
	}

	ioctl(fd, VIDIOC_STREAMOFF, &type);
	for (i = 0; i < nbufs; i++) munmap(maps[i], lens[i]);
	close(fd);

	/* Skip the first few: the capture starts wherever the device is in the
	 * raster, so the opening frames are not representative of steady state.
	 * Two must remain, or there is no gap to measure. */
	int skip = 3, m = n - skip;
	if (m < 2) { fprintf(stderr, "too few frames\n"); return 1; }
	qsort(age + skip, m, sizeof *age, cmp);
	qsort(gap + skip + 1, m - 1, sizeof *gap, cmp);

	double sum = 0;
	for (i = skip; i < n; i++) sum += age[i];

	printf("\n%d frames (first %d skipped)\n", m, skip);
	printf("frame AGE at dequeue -- driver + transport, no player:\n");
	printf("   min %.2f ms   median %.2f ms   mean %.2f ms   p95 %.2f ms   max %.2f ms\n",
	       age[skip], age[skip + m / 2], sum / m,
	       age[skip + (int)(m * 0.95)], age[n - 1]);
	printf("inter-frame gap at dequeue:\n");
	printf("   min %.2f ms   median %.2f ms   max %.2f ms   (33.37 ms = 29.97 fps)\n",
	       gap[skip + 1], gap[skip + 1 + (m - 1) / 2], gap[n - 1]);
	free(age); free(gap);
	return 0;
}
