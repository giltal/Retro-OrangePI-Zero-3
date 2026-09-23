/*
 * seed-credit -- credit the saved RNG seed to the kernel, and nothing else.
 *
 * WHY THIS EXISTS
 *
 * The kernel CRNG is the gate the whole boot queues behind on this board.
 * Until "random: crng init done", the first userspace process that touches the
 * RNG blocks -- and it is whichever init script happens to run first, so the
 * stall moves around rather than going away when scripts are reordered.
 * Measured: syslogd first in line stalled 1.71 s; move it later and sysctl
 * stalled 0.96 s instead.
 *
 * busybox seedrng already credits the stored seed, but it also regenerates the
 * next boot's seed and fsync()s it to a slow SD card in the same run. Putting
 * that on the critical path costs seconds; backgrounding the whole thing (which
 * is what S01seedrng does) moves the credit off the critical path too, so the
 * CRNG comes up late again.
 *
 * This tool does only the cheap half -- the ioctl -- so it can run synchronously
 * while seedrng regenerates in the background.
 *
 * HISTORY: this was written once before, measured (crng 3.5 s -> 2.1 s,
 * reliably), and then DELETED because it made no difference to when the launcher
 * appeared. That was correct at the time: a udev rule was wasting 10.3 s and the
 * CRNG was nowhere near the binding constraint, so the gain sat inside the
 * board's +/-1.5 s boot noise. With that rule fixed the CRNG *is* the gate, so
 * the tool is worth having again. The earlier decision was not wrong; the
 * circumstances it was measured in changed.
 */
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/random.h>

#define SEED_DIR "/var/lib/seedrng"
#define MAXSEED  512

int main(void)
{
	/* seedrng writes seed.credit when the seed is trusted enough to count
	 * toward the entropy estimate, and seed.no-credit when it is not. Honour
	 * that distinction rather than crediting whatever is on disk. */
	static const char *const names[2] = {
		SEED_DIR "/seed.credit",
		SEED_DIR "/seed.no-credit",
	};
	unsigned char seed[MAXSEED];
	struct {
		int entropy_count;
		int buf_size;
		unsigned char buf[MAXSEED];
	} req;
	ssize_t n = -1;
	int i, fd, credit = 0;

	for (i = 0; i < 2; i++) {
		fd = open(names[i], O_RDONLY);
		if (fd < 0)
			continue;
		n = read(fd, seed, sizeof seed);
		close(fd);
		if (n > 0) {
			credit = (i == 0);
			break;
		}
		n = -1;
	}
	if (n <= 0)
		return 1;		/* no seed yet -- first boot; not an error worth noise */

	fd = open("/dev/urandom", O_WRONLY);
	if (fd < 0)
		return 1;

	/* entropy_count is in BITS. A no-credit seed is still worth mixing in,
	 * it just must not raise the estimate. */
	req.entropy_count = credit ? (int)(n * 8) : 0;
	req.buf_size      = (int)n;
	memcpy(req.buf, seed, (size_t)n);

	if (ioctl(fd, RNDADDENTROPY, &req) < 0) {
		close(fd);
		return 1;
	}
	close(fd);
	return 0;
}
