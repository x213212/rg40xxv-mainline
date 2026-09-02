#ifndef RG40XXV_BLUETOOTH_MODEL_H
#define RG40XXV_BLUETOOTH_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum {
	RG40XXV_BT_MAX_DEVICES = 64,
	RG40XXV_BT_OUTPUT_DEVICES = 16,
	RG40XXV_BT_ADDRESS_SIZE = 18,
	RG40XXV_BT_NAME_SIZE = 97,
	RG40XXV_BT_ICON_SIZE = 65,
	RG40XXV_BT_PATH_SIZE = 256,
	RG40XXV_BT_ENCODED_NAME_SIZE = (RG40XXV_BT_NAME_SIZE - 1) * 3 + 1,
};

enum rg40xxv_bt_kind {
	RG40XXV_BT_UNKNOWN,
	RG40XXV_BT_CONTROLLER,
	RG40XXV_BT_AUDIO,
	RG40XXV_BT_KEYBOARD,
	RG40XXV_BT_MOUSE,
};

struct rg40xxv_bt_device {
	char path[RG40XXV_BT_PATH_SIZE];
	char address[RG40XXV_BT_ADDRESS_SIZE];
	char name[RG40XXV_BT_NAME_SIZE];
	char raw_name[249];
	char raw_alias[249];
	char icon[RG40XXV_BT_ICON_SIZE];
	uint32_t class_of_device;
	int rssi;
	bool paired;
	bool trusted;
	bool connected;
	bool has_hid_uuid;
	bool has_audio_uuid;
	enum rg40xxv_bt_kind kind;
};

struct rg40xxv_bt_model {
	struct rg40xxv_bt_device devices[RG40XXV_BT_MAX_DEVICES];
	size_t device_count;
	char adapter_path[RG40XXV_BT_PATH_SIZE];
	char adapter_address[RG40XXV_BT_ADDRESS_SIZE];
	bool adapter_present;
	bool powered;
	bool discovering;
};

bool rg40xxv_bt_normalize_mac(const char *input,
	char output[RG40XXV_BT_ADDRESS_SIZE]);
void rg40xxv_bt_sanitize_name(const char *input, const char *fallback,
	char output[RG40XXV_BT_NAME_SIZE]);
enum rg40xxv_bt_kind rg40xxv_bt_classify(const char *icon,
	uint32_t class_of_device, bool has_hid_uuid, bool has_audio_uuid);
const char *rg40xxv_bt_kind_name(enum rg40xxv_bt_kind kind);
int rg40xxv_bt_percent_encode(const char *input, char *output,
	size_t output_size);
void rg40xxv_bt_sort(struct rg40xxv_bt_model *model);
int rg40xxv_bt_write_snapshot(FILE *output,
	const struct rg40xxv_bt_model *model);

#endif
