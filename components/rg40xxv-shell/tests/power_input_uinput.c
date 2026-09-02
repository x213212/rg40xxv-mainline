#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static void fail(const char *operation)
{
	perror(operation);
	exit(EXIT_FAILURE);
}

static void sleep_ms(long milliseconds)
{
	struct timespec duration = {
		.tv_sec = milliseconds / 1000,
		.tv_nsec = milliseconds % 1000 * 1000000L,
	};

	while (nanosleep(&duration, &duration) != 0 && errno == EINTR)
		;
}

static void set_abs(int fd, unsigned int code, int minimum, int maximum)
{
	struct uinput_abs_setup setup = {
		.code = code,
		.absinfo = {
			.minimum = minimum,
			.maximum = maximum,
		},
	};

	if (ioctl(fd, UI_SET_ABSBIT, code) < 0 ||
	    ioctl(fd, UI_ABS_SETUP, &setup) < 0)
		fail("uinput abs setup");
}

static int create_device(const char *name, int power)
{
	static const unsigned int gamepad_keys[] = {
		BTN_DPAD_UP, BTN_DPAD_DOWN, BTN_DPAD_LEFT, BTN_DPAD_RIGHT,
		BTN_EAST, BTN_SOUTH, BTN_NORTH, BTN_WEST, BTN_START,
		BTN_SELECT, BTN_MODE, BTN_TL, BTN_TR, BTN_THUMBL, BTN_THUMBR,
	};
	static const unsigned int axes[] = {
		ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_HAT0X, ABS_HAT0Y,
	};
	struct uinput_setup setup = {
		.id = {
			.bustype = BUS_USB,
			.vendor = 0x1209,
			.product = power != 0 ? 0x0001 : 0x0002,
			.version = 1,
		},
	};
	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);

	if (fd < 0)
		fail("open /dev/uinput");
	if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0)
		fail("uinput EV_KEY");
	if (power != 0) {
		if (ioctl(fd, UI_SET_KEYBIT, KEY_POWER) < 0)
			fail("uinput KEY_POWER");
	} else {
		if (ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0)
			fail("uinput EV_ABS");
		for (size_t i = 0; i < sizeof(gamepad_keys) /
		     sizeof(gamepad_keys[0]); ++i) {
			if (ioctl(fd, UI_SET_KEYBIT, gamepad_keys[i]) < 0)
				fail("uinput gamepad key");
		}
		for (size_t i = 0; i < sizeof(axes) / sizeof(axes[0]); ++i)
			set_abs(fd, axes[i], axes[i] == ABS_HAT0X ||
				axes[i] == ABS_HAT0Y ? -1 : -32768,
				axes[i] == ABS_HAT0X || axes[i] == ABS_HAT0Y ?
				1 : 32767);
	}
	(void)snprintf(setup.name, sizeof(setup.name), "%s", name);
	if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 ||
	    ioctl(fd, UI_DEV_CREATE) < 0)
		fail("uinput create");
	return fd;
}

static void emit_event(int fd, unsigned int type, unsigned int code, int value)
{
	struct input_event event = {
		.type = type,
		.code = code,
		.value = value,
	};

	if (write(fd, &event, sizeof(event)) != (ssize_t)sizeof(event))
		fail("uinput write");
}

static void emit_power_press(int fd)
{
	emit_event(fd, EV_KEY, KEY_POWER, 1);
	emit_event(fd, EV_SYN, SYN_REPORT, 0);
	sleep_ms(80);
	emit_event(fd, EV_KEY, KEY_POWER, 0);
	emit_event(fd, EV_SYN, SYN_REPORT, 0);
}

static void report_event_nodes(void)
{
	for (int index = 0; index < 8; ++index) {
		char path[64];
		char name[64] = { 0 };
		int fd;

		(void)snprintf(path, sizeof(path), "/dev/input/event%d", index);
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0)
			(void)fprintf(stderr, "NODE path=%s name=%s\n", path, name);
		close(fd);
	}
}

int main(void)
{
	int gamepad = create_device("H700 Gamepad", 0);
	int power = create_device("axp20x-pek", 1);

	sleep_ms(500);
	report_event_nodes();
	puts("READY");
	(void)fflush(stdout);
	sleep_ms(1500);
	emit_power_press(power);
	sleep_ms(500);
	emit_power_press(power);
	sleep_ms(500);
	if (ioctl(power, UI_DEV_DESTROY) < 0)
		fail("uinput destroy power");
	close(power);
	sleep_ms(1200);
	power = create_device("axp20x-pek", 1);
	sleep_ms(1500);
	emit_power_press(power);
	sleep_ms(500);
	emit_power_press(power);
	sleep_ms(500);
	(void)ioctl(power, UI_DEV_DESTROY);
	(void)ioctl(gamepad, UI_DEV_DESTROY);
	close(power);
	close(gamepad);
	return EXIT_SUCCESS;
}
