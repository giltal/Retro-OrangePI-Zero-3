/*
 * inject-input -- synthesise key presses on the target through uinput.
 *
 * Why this exists: the launcher and RetroArch both read input through
 * evdev/udev, so there was no way to drive them from a shell over SSH -- and
 * RetroArch's stdin command interface never initialises here (the udev input
 * driver owns stdin, and read_stdin() is a blocking read besides). This
 * creates a virtual keyboard, so injected events travel the exact same path a
 * real gamepad's do, which is what makes the test meaningful.
 *
 * Usage:  inject-input KEY [KEY...]     press and release each in turn
 *         inject-input -d MS KEY        hold KEY for MS milliseconds
 *         inject-input -s MS            settle delay before injecting
 *                                       (default 1500ms, so udev clients
 *                                        notice the new device first)
 * KEY is a name from the table below or a raw numeric keycode.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/uinput.h>

struct keyname { const char *name; int code; };

static const struct keyname keys[] = {
	{ "F1", KEY_F1 }, { "F2", KEY_F2 }, { "F3", KEY_F3 }, { "F4", KEY_F4 },
	{ "ENTER", KEY_ENTER }, { "ESC", KEY_ESC }, { "SPACE", KEY_SPACE },
	{ "UP", KEY_UP }, { "DOWN", KEY_DOWN }, { "LEFT", KEY_LEFT },
	{ "RIGHT", KEY_RIGHT }, { "BACKSPACE", KEY_BACKSPACE },
	{ "X", KEY_X }, { "Z", KEY_Z }, { "A", KEY_A }, { "S", KEY_S },
	{ "Q", KEY_Q }, { "P", KEY_P }, { "TAB", KEY_TAB },
	{ NULL, 0 }
};

static int lookup(const char *s)
{
	int i;
	for (i = 0; keys[i].name; i++)
		if (!strcasecmp(s, keys[i].name))
			return keys[i].code;
	if (s[0] >= '0' && s[0] <= '9')
		return atoi(s);
	return -1;
}

static void emit(int fd, int type, int code, int val)
{
	struct input_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = type;
	ev.code = code;
	ev.value = val;
	if (write(fd, &ev, sizeof(ev)) != sizeof(ev))
		fprintf(stderr, "inject-input: write failed: %s\n", strerror(errno));
}

static void sync_ev(int fd) { emit(fd, EV_SYN, SYN_REPORT, 0); }

int main(int argc, char **argv)
{
	struct uinput_setup us;
	int fd, i, hold_ms = 60, settle_ms = 1500;
	int first = 1;

	while (first < argc && argv[first][0] == '-') {
		if (!strcmp(argv[first], "-d") && first + 1 < argc)
			hold_ms = atoi(argv[++first]);
		else if (!strcmp(argv[first], "-s") && first + 1 < argc)
			settle_ms = atoi(argv[++first]);
		else { fprintf(stderr, "unknown option %s\n", argv[first]); return 2; }
		first++;
	}
	if (first >= argc) {
		fprintf(stderr, "usage: %s [-d hold_ms] [-s settle_ms] KEY [KEY...]\n", argv[0]);
		return 2;
	}

	if ((fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK)) < 0) {
		fprintf(stderr, "inject-input: cannot open /dev/uinput: %s\n"
		        "  (is CONFIG_INPUT_UINPUT enabled?)\n", strerror(errno));
		return 1;
	}

	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	ioctl(fd, UI_SET_EVBIT, EV_SYN);
	/* Advertise every key we might send, plus a broad range so RetroArch
	 * classifies the device as a real keyboard rather than ignoring it. */
	for (i = 0; keys[i].name; i++)
		ioctl(fd, UI_SET_KEYBIT, keys[i].code);
	for (i = KEY_ESC; i <= KEY_COMPOSE; i++)
		ioctl(fd, UI_SET_KEYBIT, i);

	memset(&us, 0, sizeof(us));
	us.id.bustype = BUS_VIRTUAL;
	us.id.vendor  = 0x1d6b;
	us.id.product = 0x0001;
	us.id.version = 1;
	strcpy(us.name, "retrobpi-test-injector");

	if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
		fprintf(stderr, "inject-input: device create failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	/* Clients discover the device asynchronously via udev; injecting before
	 * they have opened it means the events go nowhere. */
	usleep(settle_ms * 1000);

	for (i = first; i < argc; i++) {
		int code = lookup(argv[i]);
		if (code < 0) {
			fprintf(stderr, "inject-input: unknown key '%s'\n", argv[i]);
			continue;
		}
		printf("inject: %s (code %d)\n", argv[i], code);
		fflush(stdout);
		emit(fd, EV_KEY, code, 1); sync_ev(fd);
		usleep(hold_ms * 1000);
		emit(fd, EV_KEY, code, 0); sync_ev(fd);
		usleep(120 * 1000);
	}

	usleep(300 * 1000);
	ioctl(fd, UI_DEV_DESTROY);
	close(fd);
	return 0;
}
