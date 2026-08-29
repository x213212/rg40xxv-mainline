#define _POSIX_C_SOURCE 200809L

#include "cover_limits.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static int read_exact_at(int fd, uint64_t offset, unsigned char *buffer,
			 size_t length)
{
	size_t done = 0;

	while (done < length) {
		ssize_t count = pread(fd, buffer + done, length - done,
				      (off_t)(offset + done));

		if (count < 0) {
			if (errno == EINTR)
				continue;
			return COVER_PROBE_IO;
		}
		if (count == 0)
			return COVER_PROBE_INVALID;
		done += (size_t)count;
	}
	return COVER_PROBE_OK;
}

static uint16_t read_be16(const unsigned char *value)
{
	return (uint16_t)((uint16_t)value[0] << 8 | value[1]);
}

static uint32_t read_be32(const unsigned char *value)
{
	return (uint32_t)value[0] << 24 | (uint32_t)value[1] << 16 |
		(uint32_t)value[2] << 8 | value[3];
}

static uint16_t read_le16(const unsigned char *value)
{
	return (uint16_t)((uint16_t)value[1] << 8 | value[0]);
}

static uint32_t read_le32(const unsigned char *value)
{
	return (uint32_t)value[3] << 24 | (uint32_t)value[2] << 16 |
		(uint32_t)value[1] << 8 | value[0];
}

static int validate_dimensions(uint64_t width, uint64_t height,
			       struct cover_dimensions *result)
{
	uint64_t pixels;

	if (width == 0 || height == 0)
		return COVER_PROBE_INVALID;
	if (width > COVER_SOURCE_MAX_WIDTH || height > COVER_SOURCE_MAX_HEIGHT)
		return COVER_PROBE_LIMIT;
	pixels = width * height;
	if (pixels > COVER_SOURCE_MAX_PIXELS ||
	    pixels > COVER_DECODE_MAX_BYTES / 4U)
		return COVER_PROBE_LIMIT;
	result->width = (uint32_t)width;
	result->height = (uint32_t)height;
	result->decode_bytes = pixels * 4U;
	return COVER_PROBE_OK;
}

static int probe_png(int fd, uint64_t file_size,
		     struct cover_dimensions *result)
{
	static const unsigned char signature[] = {
		0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a,
	};
	unsigned char header[24];
	int status;

	if (file_size < sizeof(header))
		return COVER_PROBE_INVALID;
	status = read_exact_at(fd, 0, header, sizeof(header));
	if (status != COVER_PROBE_OK)
		return status;
	if (memcmp(header, signature, sizeof(signature)) != 0 ||
	    read_be32(header + 8) != 13U ||
	    memcmp(header + 12, "IHDR", 4) != 0)
		return COVER_PROBE_INVALID;
	return validate_dimensions(read_be32(header + 16),
				   read_be32(header + 20), result);
}

static int probe_bmp(int fd, uint64_t file_size,
		     struct cover_dimensions *result)
{
	unsigned char header[26];
	uint32_t dib_size;
	uint64_t width;
	uint64_t height;
	int32_t signed_width;
	int32_t signed_height;
	int status;

	if (file_size < sizeof(header))
		return COVER_PROBE_INVALID;
	status = read_exact_at(fd, 0, header, sizeof(header));
	if (status != COVER_PROBE_OK)
		return status;
	if (header[0] != 'B' || header[1] != 'M')
		return COVER_PROBE_INVALID;
	dib_size = read_le32(header + 14);
	if (dib_size == 12U) {
		width = read_le16(header + 18);
		height = read_le16(header + 20);
	} else if (dib_size >= 40U) {
		signed_width = (int32_t)read_le32(header + 18);
		signed_height = (int32_t)read_le32(header + 22);
		if (signed_width <= 0 || signed_height == INT32_MIN)
			return COVER_PROBE_INVALID;
		width = (uint32_t)signed_width;
		height = signed_height < 0 ? (uint64_t)(-(int64_t)signed_height) :
			(uint32_t)signed_height;
	} else {
		return COVER_PROBE_INVALID;
	}
	return validate_dimensions(width, height, result);
}

static int is_sof_marker(unsigned char marker)
{
	switch (marker) {
	case 0xc0:
	case 0xc1:
	case 0xc2:
	case 0xc3:
	case 0xc5:
	case 0xc6:
	case 0xc7:
	case 0xc9:
	case 0xca:
	case 0xcb:
	case 0xcd:
	case 0xce:
	case 0xcf:
		return 1;
	default:
		return 0;
	}
}

static int probe_jpeg(int fd, uint64_t file_size,
		      struct cover_dimensions *result)
{
	unsigned char bytes[5];
	uint64_t offset = 2;
	int status;

	if (file_size < 4)
		return COVER_PROBE_INVALID;
	status = read_exact_at(fd, 0, bytes, 2);
	if (status != COVER_PROBE_OK)
		return status;
	if (bytes[0] != 0xff || bytes[1] != 0xd8)
		return COVER_PROBE_INVALID;
	while (offset + 1 < file_size &&
	       offset < COVER_HEADER_SCAN_MAX_BYTES) {
		unsigned char marker;
		uint16_t segment_size;
		uint64_t payload_size;

		status = read_exact_at(fd, offset++, &marker, 1);
		if (status != COVER_PROBE_OK)
			return status;
		if (marker != 0xff)
			continue;
		do {
			status = read_exact_at(fd, offset++, &marker, 1);
			if (status != COVER_PROBE_OK)
				return status;
		} while (marker == 0xff && offset < file_size);
		if (marker == 0x00)
			continue;
		if (marker == 0xd9 || marker == 0xda)
			return COVER_PROBE_INVALID;
		if (marker == 0xd8 || marker == 0x01 ||
		    (marker >= 0xd0 && marker <= 0xd7))
			continue;
		if (offset + 2 > file_size)
			return COVER_PROBE_INVALID;
		status = read_exact_at(fd, offset, bytes, 2);
		if (status != COVER_PROBE_OK)
			return status;
		segment_size = read_be16(bytes);
		offset += 2;
		if (segment_size < 2)
			return COVER_PROBE_INVALID;
		payload_size = (uint64_t)segment_size - 2U;
		if (payload_size > file_size - offset ||
		    offset + payload_size > COVER_HEADER_SCAN_MAX_BYTES)
			return COVER_PROBE_INVALID;
		if (is_sof_marker(marker)) {
			if (payload_size < sizeof(bytes))
				return COVER_PROBE_INVALID;
			status = read_exact_at(fd, offset, bytes, sizeof(bytes));
			if (status != COVER_PROBE_OK)
				return status;
			return validate_dimensions(read_be16(bytes + 3),
						   read_be16(bytes + 1), result);
		}
		offset += payload_size;
	}
	return COVER_PROBE_INVALID;
}

int cover_probe_fd(int fd, uint64_t file_size, struct cover_dimensions *result)
{
	unsigned char signature[2];
	int status;

	if (fd < 0 || result == NULL || file_size == 0)
		return COVER_PROBE_INVALID;
	memset(result, 0, sizeof(*result));
	if (file_size > COVER_FILE_MAX_BYTES)
		return COVER_PROBE_LIMIT;
	status = read_exact_at(fd, 0, signature, sizeof(signature));
	if (status != COVER_PROBE_OK)
		return status;
	if (signature[0] == 0x89 && signature[1] == 'P')
		return probe_png(fd, file_size, result);
	if (signature[0] == 'B' && signature[1] == 'M')
		return probe_bmp(fd, file_size, result);
	if (signature[0] == 0xff && signature[1] == 0xd8)
		return probe_jpeg(fd, file_size, result);
	return COVER_PROBE_INVALID;
}
