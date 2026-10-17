// SPDX-License-Identifier: GPL-2.0
#define _POSIX_C_SOURCE 200809L
/* gbtest -- drive the races the last review fixed, from userspace.
 *
 * Each mode here exercises one of those fixes, on a debug kernel (KASAN,
 * lockdep, PROVE_RCU) where a mistake is loud. The kernel
 * side is the verdict -- watch dmesg -- but each mode also checks what
 * userspace can see, as a frozen capture or a wrong errno is a failure
 * KASAN will never report.
 *
 *   gbtest stream DEV SECS NBUFS SLOW_MS   MMAP capture; sleep SLOW_MS per frame.
 *                                          FAILS if no frame arrives for 2 s
 *                                          (the frozen-capture bug).
 *   gbtest cycle  DEV N FRAMES             STREAMON, take FRAMES, STREAMOFF; N
 *                                          times (gb_stop_streaming's fence).
 *   gbtest sstd   DEV                      S_STD to the other 525/625 norm and
 *                                          back; prints the errno. Must be
 *                                          EBUSY while audio holds the stream.
 *   gbtest read   DEV SECS                 read() frames until SECS or an error;
 *                                          prints how it ended (unbind test).
 *   gbtest stdrace DEV SECS                a child toggles S_STD NTSC<->PAL
 *                                          nonstop while the parent loops
 *                                          REQBUFS/QBUF/STREAMON/STREAMOFF --
 *                                          the 2026-10-16 race, where buffers
 *                                          sized for 480 lines were streamed at
 *                                          576. The verdict is the kernel log.
 *   gbtest ioctlloop DEV SECS              hammer non-queue ioctls that reach
 *                                          the subdev (S_CTRL, QUERYSTD,
 *                                          G_INPUT/S_INPUT, LOG_STATUS) from
 *                                          two processes; pull the cable
 *                                          meanwhile. Ends on ENODEV.
 *   gbtest ctrlevent DEV CLOSE_MS          subscribe to a control event, wait
 *                                          for the node to go away (unbind or
 *                                          unplug), then close CLOSE_MS later:
 *                                          close() after disconnect freed the
 *                                          control handler (third review).
 *
 * Exit status: 0 = the userspace-visible behaviour was correct, 1 = it was not,
 * 2 = the test could not run.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <linux/videodev2.h>

#define MAXBUFS 16

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void sleep_ms(int ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;

	do
		r = ioctl(fd, req, arg);
	while (r < 0 && errno == EINTR);
	return r;
}

static int open_dev(const char *dev)
{
	int fd = open(dev, O_RDWR);

	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", dev, strerror(errno));
		exit(2);
	}
	return fd;
}

struct ring {
	void *map[MAXBUFS];
	unsigned len[MAXBUFS];
	unsigned n;
};

/* REQBUFS + mmap + QBUF all. Returns 0, or -1 with errno set. */
static int ring_setup(int fd, unsigned nbufs, struct ring *r)
{
	struct v4l2_requestbuffers req = { .count = nbufs,
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
	unsigned i;

	if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0)
		return -1;
	if (req.count > MAXBUFS) {
		errno = ENOMEM;
		return -1;
	}
	r->n = req.count;
	for (i = 0; i < r->n; i++) {
		struct v4l2_buffer b = { .index = i,
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };

		if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0)
			return -1;
		r->len[i] = b.length;
		r->map[i] = mmap(NULL, b.length, PROT_READ, MAP_SHARED, fd, b.m.offset);
		if (r->map[i] == MAP_FAILED)
			return -1;
		if (xioctl(fd, VIDIOC_QBUF, &b) < 0)
			return -1;
	}
	return 0;
}

static void ring_free(int fd, struct ring *r)
{
	struct v4l2_requestbuffers req = { .count = 0,
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
	unsigned i;

	for (i = 0; i < r->n; i++)
		munmap(r->map[i], r->len[i]);
	r->n = 0;
	xioctl(fd, VIDIOC_REQBUFS, &req);
}

/* Wait up to timeout_ms for a frame, dequeue it, requeue it. Returns 1 on a
 * frame, 0 on timeout, -1 on error. *seq gets the driver's sequence number. */
static int one_frame(int fd, int timeout_ms, unsigned *seq)
{
	struct pollfd p = { .fd = fd, .events = POLLIN };
	struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_MMAP };
	int r = poll(&p, 1, timeout_ms);

	if (r <= 0)
		return r;
	if (p.revents & (POLLERR | POLLHUP)) {
		errno = EIO;
		return -1;
	}
	if (xioctl(fd, VIDIOC_DQBUF, &b) < 0)
		return -1;
	*seq = b.sequence;
	if (b.flags & V4L2_BUF_FLAG_ERROR)
		fprintf(stderr, "  buffer %u flagged ERROR\n", b.index);
	if (xioctl(fd, VIDIOC_QBUF, &b) < 0)
		return -1;
	return 1;
}

static int mode_stream(const char *dev, double secs, unsigned nbufs, int slow_ms)
{
	int fd = open_dev(dev), type = V4L2_BUF_TYPE_VIDEO_CAPTURE, bad = 0;
	struct ring r = { 0 };
	unsigned seq, last = 0, frames = 0, gaps = 0;
	double t0, worst = 0, prev;

	if (ring_setup(fd, nbufs, &r) < 0) {
		fprintf(stderr, "setup: %s\n", strerror(errno));
		return 2;
	}
	printf("stream: %u buffers, %d ms per frame consumer delay\n", r.n, slow_ms);
	if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
		fprintf(stderr, "STREAMON: %s\n", strerror(errno));
		return 2;
	}
	t0 = prev = now_s();
	while (now_s() - t0 < secs) {
		int got = one_frame(fd, 2000, &seq);

		if (got < 0) {
			printf("  DQBUF error after %u frames: %s\n", frames, strerror(errno));
			bad = 1;
			break;
		}
		if (got == 0) {
			printf("  FROZEN: no frame for 2 s after %u frames\n", frames);
			bad = 1;
			break;
		}
		if (frames && seq != last + 1)
			gaps += seq - last - 1;
		last = seq;
		frames++;
		if (now_s() - prev > worst)
			worst = now_s() - prev;
		prev = now_s();
		if (slow_ms) {
			struct timespec d = { slow_ms / 1000, (slow_ms % 1000) * 1000000L };

			nanosleep(&d, NULL);
		}
	}
	xioctl(fd, VIDIOC_STREAMOFF, &type);
	printf("stream: %u frames in %.1f s, %u dropped by sequence, worst gap %.0f ms -> %s\n",
	       frames, now_s() - t0, gaps, worst * 1000, bad ? "FAIL" : "ok");
	ring_free(fd, &r);
	close(fd);
	return bad;
}

static int mode_cycle(const char *dev, int n, int want)
{
	int fd = open_dev(dev), type = V4L2_BUF_TYPE_VIDEO_CAPTURE, i, k;
	struct ring r = { 0 };
	unsigned seq;
	double t0 = now_s();

	for (i = 0; i < n; i++) {
		if (ring_setup(fd, 4, &r) < 0 || xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
			printf("cycle %d: setup/STREAMON: %s -> FAIL\n", i, strerror(errno));
			return 1;
		}
		for (k = 0; k < want; k++) {
			int got = one_frame(fd, 2000, &seq);

			if (got != 1) {
				printf("cycle %d: frame %d: %s -> FAIL\n", i, k,
				       got ? strerror(errno) : "timeout");
				return 1;
			}
		}
		if (xioctl(fd, VIDIOC_STREAMOFF, &type) < 0) {
			printf("cycle %d: STREAMOFF: %s -> FAIL\n", i, strerror(errno));
			return 1;
		}
		ring_free(fd, &r);
	}
	printf("cycle: %d STREAMON/OFF cycles of %d frames in %.1f s -> ok\n",
	       n, want, now_s() - t0);
	close(fd);
	return 0;
}

static int mode_sstd(const char *dev)
{
	int fd = open_dev(dev);
	v4l2_std_id cur, other;
	int r, e;

	if (xioctl(fd, VIDIOC_G_STD, &cur) < 0) {
		fprintf(stderr, "G_STD: %s\n", strerror(errno));
		return 2;
	}
	other = (cur & V4L2_STD_525_60) ? V4L2_STD_PAL_B : V4L2_STD_NTSC_M;
	r = xioctl(fd, VIDIOC_S_STD, &other);
	e = errno;
	printf("sstd: 0x%llx -> 0x%llx: %s\n", (unsigned long long)cur,
	       (unsigned long long)other, r < 0 ? strerror(e) : "accepted");
	if (r == 0 && xioctl(fd, VIDIOC_S_STD, &cur) < 0)
		printf("sstd: restoring 0x%llx failed: %s\n",
		       (unsigned long long)cur, strerror(errno));
	close(fd);
	/* The verdict depends on whether audio is running, which only the caller
	 * knows: print EBUSY as the token a script greps for. */
	return r < 0 && e == EBUSY ? 0 : 1;
}

/* The S_STD/REQBUFS race (2026-10-16 security review). Before the fix,
 * gb_s_std() held vdev_lock and stream_lock but not vb_lock, so REQBUFS and
 * QBUF could run while it was in the decoder's I2C sequence, sized for the old
 * height; STREAMON then blanked and filled them at the new one, writing past
 * the plane. Two processes, two file handles: exactly the split the ioctl
 * core's locking makes concurrent. With the fix the parent's REQBUFS waits
 * for the S_STD in flight, and S_STD returns EBUSY while buffers exist.
 *
 * Userspace checks that the size G_FMT reports once the buffers exist is a
 * size they can hold, and that every dequeued buffer's bytesused fits its
 * length; the overflow itself is KASAN's to see (or an oops on a plain kernel). Run it
 * with nothing holding the stream -- with audio up, S_STD is always EBUSY and
 * the race is never exercised; the counts printed say whether it was. */
static int mode_stdrace(const char *dev, double secs)
{
	int fd = open_dev(dev);
	double end = now_s() + secs;
	unsigned rounds = 0, frames = 0, bad = 0;
	int status;
	pid_t pid;

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return 2;
	}
	if (pid == 0) {
		int cfd = open_dev(dev);
		unsigned ok = 0, busy = 0;
		v4l2_std_id std[2] = { V4L2_STD_NTSC_M, V4L2_STD_PAL_B };
		int i = 0;

		/* Flip the target only after a change lands. Flipping on EBUSY
		 * too would ask for the standard already set half the time, and
		 * S_STD to the current standard is a successful no-op -- which
		 * made the first version of this count millions of "changes". */
		xioctl(cfd, VIDIOC_S_STD, &std[0]);
		i = 1;
		while (now_s() < end) {
			if (xioctl(cfd, VIDIOC_S_STD, &std[i & 1]) == 0) {
				ok++;
				i++;
			} else if (errno == EBUSY) {
				busy++;
			}
		}
		xioctl(cfd, VIDIOC_S_STD, &std[0]);
		printf("stdrace: child: %u standard changes accepted, %u EBUSY\n",
		       ok, busy);
		close(cfd);
		fflush(stdout);	/* _exit() skips stdio */
		_exit(ok ? 0 : 3);
	}

	while (now_s() < end) {
		struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
		int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		struct ring r = { 0 };
		unsigned i, mismatch;

		if (ring_setup(fd, 2, &r) < 0 || xioctl(fd, VIDIOC_G_FMT, &fmt) < 0) {
			ring_free(fd, &r);
			continue;
		}
		/* AFTER the buffers exist: from here S_STD must refuse, so the
		 * format now is the format they were sized for. A buffer smaller
		 * than it is the race, visible without KASAN. */
		mismatch = 0;
		for (i = 0; i < r.n; i++)
			if (r.len[i] < fmt.fmt.pix.sizeimage)
				mismatch++;
		bad += mismatch;
		/* Caught it: do NOT stream these. STREAMON is where the old driver
		 * wrote past them, so skipping it lets a run against an unfixed
		 * module usually prove the race without oopsing. Not always: an
		 * S_STD can still be mid-I2C when G_FMT reads the old format, and
		 * land between it and STREAMON. Run unfixed modules on a test
		 * machine only. */
		if (!mismatch && xioctl(fd, VIDIOC_STREAMON, &type) == 0) {
			struct pollfd p = { .fd = fd, .events = POLLIN };
			struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
				.memory = V4L2_MEMORY_MMAP };

			if (poll(&p, 1, 100) > 0 && xioctl(fd, VIDIOC_DQBUF, &b) == 0) {
				frames++;
				if (b.bytesused > b.length)
					bad++;
			}
			xioctl(fd, VIDIOC_STREAMOFF, &type);
		}
		ring_free(fd, &r);
		rounds++;
	}
	close(fd);
	waitpid(pid, &status, 0);
	printf("stdrace: parent: %u rounds, %u frames, %u mismatched buffers\n",
	       rounds, frames, bad);
	if (!WIFEXITED(status) || WEXITSTATUS(status) == 3) {
		printf("stdrace: NOT EXERCISED -- no standard change was accepted "
		       "(is audio holding the stream?)\n");
		return 2;
	}
	return bad ? 1 : 0;
}

/* Disconnect against in-flight non-queue ioctls (second review, 2026-10-16).
 * Those run under vdev_lock only and reach the cx25840 subdev and its control
 * handler; disconnect used to unbind and free both without taking that lock.
 * Two processes keep such ioctls in flight continuously; the operator pulls
 * the cable. Userspace sees only how each loop ended -- ENODEV is the correct
 * end -- and the verdict is the kernel log, under KASAN. */
static int ioctl_burst(const char *dev, double end, unsigned *n)
{
	int fd = open_dev(dev);
	struct v4l2_queryctrl qc = { .id = V4L2_CID_BRIGHTNESS };
	struct v4l2_control c = { .id = V4L2_CID_BRIGHTNESS };
	int lo = 0, hi = 255, i = 0, input, e = 0;

	if (xioctl(fd, VIDIOC_QUERYCTRL, &qc) == 0) {
		lo = qc.minimum;
		hi = qc.maximum;
	}
	while (now_s() < end) {
		v4l2_std_id std;

		c.value = (i++ & 1) ? lo + (hi - lo) / 4 : lo + (hi - lo) / 2;
		if ((xioctl(fd, VIDIOC_S_CTRL, &c) < 0 && errno == ENODEV) ||
		    (xioctl(fd, VIDIOC_QUERYSTD, &std) < 0 && errno == ENODEV) ||
		    (xioctl(fd, VIDIOC_G_INPUT, &input) < 0 && errno == ENODEV) ||
		    (xioctl(fd, VIDIOC_S_INPUT, &input) < 0 && errno == ENODEV) ||
		    /* one in 256: cx25840 prints a whole block per call */
		    ((i & 255) == 0 &&
		     xioctl(fd, VIDIOC_LOG_STATUS, NULL) < 0 && errno == ENODEV)) {
			e = ENODEV;
			break;
		}
		(*n)++;
	}
	close(fd);
	return e;
}

static int mode_ioctlloop(const char *dev, double secs)
{
	double end = now_s() + secs;
	unsigned n = 0;
	int status, e;
	pid_t pid = fork();

	if (pid < 0) {
		perror("fork");
		return 2;
	}
	e = ioctl_burst(dev, end, &n);
	printf("ioctlloop[%s]: %u rounds, ended on %s\n", pid ? "parent" : "child",
	       n, e ? strerror(e) : "timeout");
	fflush(stdout);
	if (pid == 0)
		_exit(e == ENODEV ? 0 : 1);
	waitpid(pid, &status, 0);
	return e == ENODEV && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1;
}

/* Close after disconnect, with a control event subscribed (third review,
 * 2026-10-16). close() reaches the bridge's control handler through the
 * subscription -- v4l2_fh_exit -> v4l2_ctrl_del_event -> find_ref_lock -- and
 * disconnect used to have freed that handler, mutex and all. CLOSE_MS 0 closes
 * as soon as the node reports ENODEV, inside disconnect's teardown if the
 * timing falls that way; a larger value closes after it. The verdict is the
 * kernel log (a DEBUG_MUTEXES warning, or KASAN). */
static int mode_ctrlevent(const char *dev, int close_ms)
{
	int fd = open_dev(dev);
	struct v4l2_event_subscription sub = { .type = V4L2_EVENT_CTRL,
		.id = V4L2_CID_BRIGHTNESS };
	struct v4l2_control c = { .id = V4L2_CID_BRIGHTNESS };
	double t0;

	if (xioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0) {
		fprintf(stderr, "SUBSCRIBE_EVENT: %s\n", strerror(errno));
		return 2;
	}
	printf("ctrlevent: subscribed; waiting for the node to go away\n");
	fflush(stdout);
	t0 = now_s();
	while (xioctl(fd, VIDIOC_G_CTRL, &c) == 0 || errno != ENODEV) {
		if (now_s() - t0 > 3600) {
			fprintf(stderr, "ctrlevent: no ENODEV in 3600 s\n");
			return 2;
		}
		sleep_ms(1);
	}
	if (close_ms > 0)
		sleep_ms(close_ms);
	close(fd);
	printf("ctrlevent: closed %d ms after ENODEV\n", close_ms);
	return 0;
}

static int mode_read(const char *dev, double secs)
{
	int fd = open_dev(dev);
	struct v4l2_format f = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
	unsigned frames = 0;
	double t0 = now_s();
	char *buf;

	if (xioctl(fd, VIDIOC_G_FMT, &f) < 0) {
		fprintf(stderr, "G_FMT: %s\n", strerror(errno));
		return 2;
	}
	buf = malloc(f.fmt.pix.sizeimage);
	if (!buf)
		return 2;
	while (now_s() - t0 < secs) {
		ssize_t n = read(fd, buf, f.fmt.pix.sizeimage);

		if (n < 0) {
			printf("read: ended after %u frames, %.1f s: %s\n",
			       frames, now_s() - t0, strerror(errno));
			free(buf);
			close(fd);
			return 0;
		}
		if (n == 0) {
			printf("read: EOF after %u frames, %.1f s\n", frames, now_s() - t0);
			free(buf);
			close(fd);
			return 0;
		}
		frames++;
	}
	printf("read: %u frames in %.1f s, still running\n", frames, now_s() - t0);
	free(buf);
	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc >= 6 && !strcmp(argv[1], "stream"))
		return mode_stream(argv[2], atof(argv[3]), atoi(argv[4]), atoi(argv[5]));
	if (argc >= 5 && !strcmp(argv[1], "cycle"))
		return mode_cycle(argv[2], atoi(argv[3]), atoi(argv[4]));
	if (argc >= 3 && !strcmp(argv[1], "sstd"))
		return mode_sstd(argv[2]);
	if (argc >= 4 && !strcmp(argv[1], "read"))
		return mode_read(argv[2], atof(argv[3]));
	if (argc >= 4 && !strcmp(argv[1], "stdrace"))
		return mode_stdrace(argv[2], atof(argv[3]));
	if (argc >= 4 && !strcmp(argv[1], "ioctlloop"))
		return mode_ioctlloop(argv[2], atof(argv[3]));
	if (argc >= 4 && !strcmp(argv[1], "ctrlevent"))
		return mode_ctrlevent(argv[2], atoi(argv[3]));
	fprintf(stderr,
		"usage: gbtest stream DEV SECS NBUFS SLOW_MS\n"
		"       gbtest cycle  DEV N FRAMES\n"
		"       gbtest sstd   DEV\n"
		"       gbtest read   DEV SECS\n"
		"       gbtest stdrace DEV SECS\n"
		"       gbtest ioctlloop DEV SECS\n"
		"       gbtest ctrlevent DEV CLOSE_MS\n");
	return 2;
}
