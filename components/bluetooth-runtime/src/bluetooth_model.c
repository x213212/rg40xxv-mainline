#define _POSIX_C_SOURCE 200809L

#include "bluetooth_model.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static bool contains_case_insensitive(const char *text, const char *needle)
{
	size_t needle_length;

	if (text == NULL || needle == NULL)
		return false;
	needle_length = strlen(needle);
	if (needle_length == 0U)
		return true;
	for (; *text != '\0'; ++text) {
		size_t offset;

		for (offset = 0U; offset < needle_length; ++offset) {
			unsigned char left = (unsigned char)text[offset];
			unsigned char right = (unsigned char)needle[offset];

			if (left == '\0' || tolower(left) != tolower(right))
				break;
		}
		if (offset == needle_length)
			return true;
	}
	return false;
}

bool rg40xxv_bt_normalize_mac(const char *input,
	char output[RG40XXV_BT_ADDRESS_SIZE])
{
	if (input == NULL || strlen(input) != RG40XXV_BT_ADDRESS_SIZE - 1U)
		return false;
	for (size_t offset = 0U; offset < RG40XXV_BT_ADDRESS_SIZE - 1U;
	     ++offset) {
		unsigned char value = (unsigned char)input[offset];

		if (offset % 3U == 2U) {
			if (value != ':')
				return false;
			output[offset] = ':';
		} else {
			if (!isxdigit(value))
				return false;
			output[offset] = (char)toupper(value);
		}
	}
	output[RG40XXV_BT_ADDRESS_SIZE - 1U] = '\0';
	return true;
}

static size_t valid_utf8_sequence(const unsigned char *text)
{
	unsigned char first = text[0];
	size_t length;

	if (first >= 0xc2U && first <= 0xdfU)
		length = 2U;
	else if (first >= 0xe0U && first <= 0xefU)
		length = 3U;
	else if (first >= 0xf0U && first <= 0xf4U)
		length = 4U;
	else
		return 0U;
	for (size_t offset = 1U; offset < length; ++offset) {
		if (text[offset] == '\0' || (text[offset] & 0xc0U) != 0x80U)
			return 0U;
	}
	if ((first == 0xe0U && text[1] < 0xa0U) ||
	    (first == 0xedU && text[1] >= 0xa0U) ||
	    (first == 0xf0U && text[1] < 0x90U) ||
	    (first == 0xf4U && text[1] >= 0x90U))
		return 0U;
	return length;
}

void rg40xxv_bt_sanitize_name(const char *input, const char *fallback,
	char output[RG40XXV_BT_NAME_SIZE])
{
	const unsigned char *cursor = (const unsigned char *)input;
	size_t used = 0U;

	if (cursor == NULL)
		cursor = (const unsigned char *)"";
	while (*cursor != '\0' && used + 1U < RG40XXV_BT_NAME_SIZE) {
		if (*cursor < 0x80U) {
			unsigned char value = *cursor++;

			if (value < 0x20U || value == 0x7fU)
				value = ' ';
			output[used++] = (char)value;
		} else {
			size_t sequence = valid_utf8_sequence(cursor);

			if (sequence == 0U) {
				output[used++] = '?';
				++cursor;
			} else if (used + sequence >= RG40XXV_BT_NAME_SIZE) {
				break;
			} else {
				memcpy(output + used, cursor, sequence);
				used += sequence;
				cursor += sequence;
			}
		}
	}
	while (used > 0U && output[used - 1U] == ' ')
		--used;
	output[used] = '\0';
	if (used == 0U) {
		const char *safe_fallback = fallback != NULL && fallback[0] != '\0' ?
			fallback : "Bluetooth device";

		(void)snprintf(output, RG40XXV_BT_NAME_SIZE, "%s", safe_fallback);
	}
}

enum rg40xxv_bt_kind rg40xxv_bt_classify(const char *icon,
	uint32_t class_of_device, bool has_hid_uuid, bool has_audio_uuid)
{
	unsigned int major = (class_of_device >> 8U) & 0x1fU;
	unsigned int minor = (class_of_device >> 2U) & 0x3fU;

	if (contains_case_insensitive(icon, "keyboard"))
		return RG40XXV_BT_KEYBOARD;
	if (contains_case_insensitive(icon, "mouse"))
		return RG40XXV_BT_MOUSE;
	if (contains_case_insensitive(icon, "input-gaming") ||
	    contains_case_insensitive(icon, "gamepad") ||
	    contains_case_insensitive(icon, "joystick"))
		return RG40XXV_BT_CONTROLLER;
	if (contains_case_insensitive(icon, "audio") ||
	    contains_case_insensitive(icon, "headset") ||
	    contains_case_insensitive(icon, "headphone") ||
	    contains_case_insensitive(icon, "speaker"))
		return RG40XXV_BT_AUDIO;
	if (has_audio_uuid)
		return RG40XXV_BT_AUDIO;
	if (major == 0x05U) {
		unsigned int combo = minor & 0x30U;
		unsigned int subtype = minor & 0x0fU;

		if (subtype == 0x01U || subtype == 0x02U)
			return RG40XXV_BT_CONTROLLER;
		if (combo == 0x10U)
			return RG40XXV_BT_KEYBOARD;
		if (combo == 0x20U)
			return RG40XXV_BT_MOUSE;
		return RG40XXV_BT_CONTROLLER;
	}
	if (has_hid_uuid)
		return RG40XXV_BT_CONTROLLER;
	if (major == 0x04U)
		return RG40XXV_BT_AUDIO;
	return RG40XXV_BT_UNKNOWN;
}

const char *rg40xxv_bt_kind_name(enum rg40xxv_bt_kind kind)
{
	switch (kind) {
	case RG40XXV_BT_CONTROLLER:
		return "controller";
	case RG40XXV_BT_AUDIO:
		return "audio";
	case RG40XXV_BT_KEYBOARD:
		return "keyboard";
	case RG40XXV_BT_MOUSE:
		return "mouse";
	case RG40XXV_BT_UNKNOWN:
	default:
		return "unknown";
	}
}

int rg40xxv_bt_percent_encode(const char *input, char *output,
	size_t output_size)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t used = 0U;

	if (input == NULL || output == NULL || output_size == 0U)
		return EINVAL;
	for (const unsigned char *cursor = (const unsigned char *)input;
	     *cursor != '\0'; ++cursor) {
		unsigned char value = *cursor;
		bool plain = isalnum(value) || value == '-' || value == '_' ||
			value == '.' || value == '~';
		size_t needed = plain ? 1U : 3U;

		if (used + needed >= output_size)
			return ENOSPC;
		if (plain) {
			output[used++] = (char)value;
		} else {
			output[used++] = '%';
			output[used++] = hex[value >> 4U];
			output[used++] = hex[value & 0x0fU];
		}
	}
	output[used] = '\0';
	return 0;
}

static int compare_devices(const void *left_pointer, const void *right_pointer)
{
	const struct rg40xxv_bt_device *left = left_pointer;
	const struct rg40xxv_bt_device *right = right_pointer;

	if (left->connected != right->connected)
		return left->connected ? -1 : 1;
	if (left->paired != right->paired)
		return left->paired ? -1 : 1;
	if (left->rssi != right->rssi)
		return left->rssi > right->rssi ? -1 : 1;
	if (strcmp(left->name, right->name) != 0)
		return strcmp(left->name, right->name);
	return strcmp(left->address, right->address);
}

void rg40xxv_bt_sort(struct rg40xxv_bt_model *model)
{
	if (model == NULL)
		return;
	qsort(model->devices, model->device_count, sizeof(model->devices[0]),
		compare_devices);
}

int rg40xxv_bt_write_snapshot(FILE *output,
	const struct rg40xxv_bt_model *model)
{
	size_t emitted = 0U;
	char adapter_address[RG40XXV_BT_ADDRESS_SIZE] = "-";

	if (output == NULL || model == NULL)
		return EINVAL;
	if (model->adapter_present &&
	    (!rg40xxv_bt_normalize_mac(model->adapter_address, adapter_address) ||
	     strcmp(adapter_address, "00:00:00:00:00:00") == 0))
		return EPROTO;
	if (fprintf(output, "RG40XXV_BLUETOOTH_SNAPSHOT\t2\n"
		"A\t%d\t%d\t%d\t%s\n", model->adapter_present ? 1 : 0,
		model->adapter_present && model->powered ? 1 : 0,
		model->adapter_present && model->powered && model->discovering ?
			1 : 0, adapter_address) < 0)
		return EIO;
	if (!model->adapter_present)
		return fflush(output) == 0 ? 0 : EIO;
	for (size_t index = 0U; index < model->device_count &&
	     emitted < RG40XXV_BT_OUTPUT_DEVICES; ++index) {
		const struct rg40xxv_bt_device *device = &model->devices[index];
		char normalized[RG40XXV_BT_ADDRESS_SIZE];
		char encoded[RG40XXV_BT_ENCODED_NAME_SIZE];
		int rssi = device->rssi;

		if (!rg40xxv_bt_normalize_mac(device->address, normalized))
			continue;
		if (rg40xxv_bt_percent_encode(device->name, encoded,
				sizeof(encoded)) != 0)
			return EOVERFLOW;
		if (!((rssi >= -127 && rssi <= 20) || rssi == 127))
			rssi = 127;
		if (fprintf(output, "D\t%s\t%s\t%s\t%d\t%d\t%d\t%d\n",
			normalized, encoded, rg40xxv_bt_kind_name(device->kind),
			device->paired ? 1 : 0, device->trusted ? 1 : 0,
			device->connected ? 1 : 0, rssi) < 0)
			return EIO;
		++emitted;
	}
	return fflush(output) == 0 ? 0 : EIO;
}
