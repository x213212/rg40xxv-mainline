#define _POSIX_C_SOURCE 200809L

#include "cover_limits.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned int checks;

#define CHECK(condition)                                                        \
	do {                                                                      \
		++checks;                                                          \
		if (!(condition)) {                                                \
			(void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,         \
				      __LINE__, #condition);                          \
			return 1;                                                     \
		}                                                                 \
	} while (0)

static int write_all_at(int fd, const unsigned char *data, size_t length)
{
	size_t done = 0;

	if (ftruncate(fd, 0) != 0)
		return -1;
	while (done < length) {
		ssize_t count = pwrite(fd, data + done, length - done, (off_t)done);

		if (count < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		done += (size_t)count;
	}
	return 0;
}

int main(void)
{
	char path[] = "/tmp/rg40xxv-cover-probe-XXXXXX";
	struct cover_dimensions dimensions;
	unsigned char png[24] = {
		0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a,
		0x00, 0x00, 0x00, 0x0d, 'I', 'H', 'D', 'R',
		0x00, 0x00, 0x02, 0x80, 0x00, 0x00, 0x01, 0xe0,
	};
	unsigned char bmp[26] = {
		'B', 'M', 0, 0, 0, 0, 0, 0, 0, 0, 26, 0, 0, 0,
		40, 0, 0, 0, 0x80, 0x02, 0, 0, 0xe0, 0x01, 0, 0,
	};
	unsigned char jpeg[] = {
		0xff, 0xd8, 0xff, 0xe0, 0x00, 0x04, 0x00, 0x00,
		0xff, 0xc0, 0x00, 0x07, 0x08, 0x01, 0xe0, 0x02, 0x80,
	};
	static const unsigned char bad[] = "not an image";
	int fd = mkstemp(path);

	CHECK(fd >= 0);
	CHECK(write_all_at(fd, png, sizeof(png)) == 0);
	CHECK(cover_probe_fd(fd, sizeof(png), &dimensions) == COVER_PROBE_OK);
	CHECK(dimensions.width == 640 && dimensions.height == 480);
	CHECK(dimensions.decode_bytes == UINT64_C(1228800));

	png[18] = 0x27;
	png[19] = 0x10;
	png[22] = 0x27;
	png[23] = 0x10;
	CHECK(write_all_at(fd, png, sizeof(png)) == 0);
	CHECK(cover_probe_fd(fd, sizeof(png), &dimensions) == COVER_PROBE_LIMIT);

	CHECK(write_all_at(fd, bmp, sizeof(bmp)) == 0);
	CHECK(cover_probe_fd(fd, sizeof(bmp), &dimensions) == COVER_PROBE_OK);
	CHECK(dimensions.width == 640 && dimensions.height == 480);

	CHECK(write_all_at(fd, jpeg, sizeof(jpeg)) == 0);
	CHECK(cover_probe_fd(fd, sizeof(jpeg), &dimensions) == COVER_PROBE_OK);
	CHECK(dimensions.width == 640 && dimensions.height == 480);
	jpeg[13] = 0x27;
	jpeg[14] = 0x10;
	jpeg[15] = 0x27;
	jpeg[16] = 0x10;
	CHECK(write_all_at(fd, jpeg, sizeof(jpeg)) == 0);
	CHECK(cover_probe_fd(fd, sizeof(jpeg), &dimensions) == COVER_PROBE_LIMIT);

	CHECK(write_all_at(fd, bad, sizeof(bad)) == 0);
	CHECK(cover_probe_fd(fd, sizeof(bad), &dimensions) ==
	      COVER_PROBE_INVALID);
	CHECK(cover_probe_fd(fd, COVER_FILE_MAX_BYTES + UINT64_C(1),
			     &dimensions) == COVER_PROBE_LIMIT);
	CHECK(COVER_NEARBY_CACHE_COUNT == 5);
	CHECK(COVER_THUMBNAIL_MAX_WIDTH == 160);
	CHECK(COVER_THUMBNAIL_MAX_HEIGHT == 232);
	CHECK(COVER_THUMBNAIL_CACHE_MAX_BYTES == UINT64_C(742400));
	CHECK(COVER_THUMBNAIL_CACHE_MAX_BYTES < UINT64_C(1048576));
	CHECK(close(fd) == 0);
	CHECK(unlink(path) == 0);
	(void)printf("COVER_LIMITS_TEST PASS checks=%u max_pixels=%u max_bytes=%u cache_bytes=%u\n",
		     checks, COVER_SOURCE_MAX_PIXELS, COVER_DECODE_MAX_BYTES,
		     COVER_THUMBNAIL_CACHE_MAX_BYTES);
	return 0;
}
