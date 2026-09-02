#define _GNU_SOURCE

#include "bluetooth_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *expression, int line)
{
	(void)fprintf(stderr, "bluetooth model assertion failed at %d: %s\n",
		line, expression);
	exit(1);
}

#define CHECK(expression) do { \
	if (!(expression)) \
		fail(#expression, __LINE__); \
} while (0)

int main(void)
{
	struct rg40xxv_bt_model model;
	char normalized[RG40XXV_BT_ADDRESS_SIZE];
	char sanitized[RG40XXV_BT_NAME_SIZE];
	char encoded[RG40XXV_BT_ENCODED_NAME_SIZE];
	char *snapshot = NULL;
	size_t snapshot_size = 0U;
	FILE *stream;

	CHECK(rg40xxv_bt_normalize_mac("aa:0b:CC:dd:01:fF", normalized));
	CHECK(strcmp(normalized, "AA:0B:CC:DD:01:FF") == 0);
	CHECK(!rg40xxv_bt_normalize_mac("AA:BB;CC:DD:EE:FF", normalized));
	CHECK(!rg40xxv_bt_normalize_mac("AA:BB:CC:DD:EE", normalized));

	rg40xxv_bt_sanitize_name("  手把\t一  ", "fallback", sanitized);
	CHECK(strcmp(sanitized, "  手把 一") == 0);
	rg40xxv_bt_sanitize_name("\001\177", "AA:BB:CC:DD:EE:FF", sanitized);
	CHECK(strcmp(sanitized, "AA:BB:CC:DD:EE:FF") == 0);
	rg40xxv_bt_sanitize_name("bad\300name", "fallback", sanitized);
	CHECK(strcmp(sanitized, "bad?name") == 0);

	CHECK(rg40xxv_bt_percent_encode("Game Pad/手把", encoded,
		sizeof(encoded)) == 0);
	CHECK(strcmp(encoded,
		"Game%20Pad%2F%E6%89%8B%E6%8A%8A") == 0);
	CHECK(rg40xxv_bt_percent_encode("x", encoded, 1U) != 0);

	CHECK(rg40xxv_bt_classify("input-gaming", 0U, false, false) ==
		RG40XXV_BT_CONTROLLER);
	CHECK(rg40xxv_bt_classify("input-keyboard", 0U, true, false) ==
		RG40XXV_BT_KEYBOARD);
	CHECK(rg40xxv_bt_classify("audio-headset", 0U, false, true) ==
		RG40XXV_BT_AUDIO);
	CHECK(rg40xxv_bt_classify("", 0x02508U, false, false) ==
		RG40XXV_BT_CONTROLLER);
	CHECK(rg40xxv_bt_classify("", 0U, false, false) ==
		RG40XXV_BT_UNKNOWN);

	memset(&model, 0, sizeof(model));
	model.adapter_present = true;
	(void)snprintf(model.adapter_address, sizeof(model.adapter_address), "%s",
		"10:22:33:44:55:66");
	model.powered = true;
	model.discovering = false;
	model.device_count = 3U;
	(void)snprintf(model.devices[0].address,
		sizeof(model.devices[0].address), "%s", "AA:BB:CC:DD:EE:01");
	(void)snprintf(model.devices[0].name, sizeof(model.devices[0].name),
		"%s", "Weak Pad");
	model.devices[0].kind = RG40XXV_BT_CONTROLLER;
	model.devices[0].rssi = -80;
	(void)snprintf(model.devices[1].address,
		sizeof(model.devices[1].address), "%s", "AA:BB:CC:DD:EE:02");
	(void)snprintf(model.devices[1].name, sizeof(model.devices[1].name),
		"%s", "Paired Pad");
	model.devices[1].kind = RG40XXV_BT_CONTROLLER;
	model.devices[1].paired = true;
	model.devices[1].trusted = true;
	model.devices[1].rssi = -60;
	(void)snprintf(model.devices[2].address,
		sizeof(model.devices[2].address), "%s", "AA:BB:CC:DD:EE:03");
	(void)snprintf(model.devices[2].name, sizeof(model.devices[2].name),
		"%s", "Connected 手把");
	model.devices[2].kind = RG40XXV_BT_CONTROLLER;
	model.devices[2].paired = true;
	model.devices[2].trusted = true;
	model.devices[2].connected = true;
	model.devices[2].rssi = -70;
	rg40xxv_bt_sort(&model);
	CHECK(strcmp(model.devices[0].address, "AA:BB:CC:DD:EE:03") == 0);
	CHECK(strcmp(model.devices[1].address, "AA:BB:CC:DD:EE:02") == 0);
	CHECK(strcmp(model.devices[2].address, "AA:BB:CC:DD:EE:01") == 0);

	stream = open_memstream(&snapshot, &snapshot_size);
	CHECK(stream != NULL);
	CHECK(rg40xxv_bt_write_snapshot(stream, &model) == 0);
	CHECK(fclose(stream) == 0);
	CHECK(snapshot_size > 0U);
	CHECK(strstr(snapshot, "RG40XXV_BLUETOOTH_SNAPSHOT\t2\n") == snapshot);
	CHECK(strstr(snapshot, "A\t1\t1\t0\t10:22:33:44:55:66\n") != NULL);
	CHECK(strstr(snapshot,
		"D\tAA:BB:CC:DD:EE:03\tConnected%20%E6%89%8B%E6%8A%8A\t"
		"controller\t1\t1\t1\t-70\n") != NULL);
	free(snapshot);

	/* A present adapter with the firmware-failure zero address is not READY. */
	memset(&model, 0, sizeof(model));
	model.adapter_present = true;
	(void)snprintf(model.adapter_address, sizeof(model.adapter_address), "%s",
		"00:00:00:00:00:00");
	snapshot = NULL;
	snapshot_size = 0U;
	stream = open_memstream(&snapshot, &snapshot_size);
	CHECK(stream != NULL);
	CHECK(rg40xxv_bt_write_snapshot(stream, &model) != 0);
	CHECK(fclose(stream) == 0);
	free(snapshot);

	memset(&model, 0, sizeof(model));
	snapshot = NULL;
	snapshot_size = 0U;
	stream = open_memstream(&snapshot, &snapshot_size);
	CHECK(stream != NULL);
	CHECK(rg40xxv_bt_write_snapshot(stream, &model) == 0);
	CHECK(fclose(stream) == 0);
	CHECK(strcmp(snapshot,
		"RG40XXV_BLUETOOTH_SNAPSHOT\t2\nA\t0\t0\t0\t-\n") == 0);
	free(snapshot);

	(void)printf("BLUETOOTH_MODEL_TEST PASS\n");
	return 0;
}
