#define _POSIX_C_SOURCE 200809L

#include <dbus/dbus.h>

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ADAPTER_PATH "/org/bluez/hci0"
#define DEVICE_PATH "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF"
#define DEVICE2_PATH "/org/bluez/hci0/dev_11_22_33_44_55_66"

struct fake_state {
	const char *pair_marker;
	bool powered;
	bool discovering;
	bool device_present;
	bool paired;
	bool trusted;
	bool connected;
	bool device2_present;
	bool device2_paired;
	bool device2_trusted;
	bool device2_connected;
};

static volatile sig_atomic_t running = 1;

static void stop_running(int signal_number)
{
	(void)signal_number;
	running = 0;
}

static bool append_basic_property(DBusMessageIter *properties,
	const char *name, int type, const char *signature, const void *value)
{
	DBusMessageIter entry;
	DBusMessageIter variant;

	return dbus_message_iter_open_container(properties, DBUS_TYPE_DICT_ENTRY,
		NULL, &entry) &&
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name) &&
		dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
			signature, &variant) &&
		dbus_message_iter_append_basic(&variant, type, value) &&
		dbus_message_iter_close_container(&entry, &variant) &&
		dbus_message_iter_close_container(properties, &entry);
}

static bool append_uuids(DBusMessageIter *properties, const char *uuid)
{
	const char *name = "UUIDs";
	DBusMessageIter entry;
	DBusMessageIter variant;
	DBusMessageIter values;

	return dbus_message_iter_open_container(properties, DBUS_TYPE_DICT_ENTRY,
		NULL, &entry) &&
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name) &&
		dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "as",
			&variant) &&
		dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "s",
			&values) &&
		dbus_message_iter_append_basic(&values, DBUS_TYPE_STRING, &uuid) &&
		dbus_message_iter_close_container(&variant, &values) &&
		dbus_message_iter_close_container(&entry, &variant) &&
		dbus_message_iter_close_container(properties, &entry);
}

static bool open_object(DBusMessageIter *objects, const char *path,
	DBusMessageIter *object_entry, DBusMessageIter *interfaces)
{
	return dbus_message_iter_open_container(objects, DBUS_TYPE_DICT_ENTRY,
		NULL, object_entry) &&
		dbus_message_iter_append_basic(object_entry, DBUS_TYPE_OBJECT_PATH,
			&path) &&
		dbus_message_iter_open_container(object_entry, DBUS_TYPE_ARRAY,
			"{sa{sv}}", interfaces);
}

static bool open_interface(DBusMessageIter *interfaces, const char *name,
	DBusMessageIter *interface_entry, DBusMessageIter *properties)
{
	return dbus_message_iter_open_container(interfaces, DBUS_TYPE_DICT_ENTRY,
		NULL, interface_entry) &&
		dbus_message_iter_append_basic(interface_entry, DBUS_TYPE_STRING,
			&name) &&
		dbus_message_iter_open_container(interface_entry, DBUS_TYPE_ARRAY,
			"{sv}", properties);
}

static bool close_interface(DBusMessageIter *interfaces,
	DBusMessageIter *interface_entry, DBusMessageIter *properties)
{
	return dbus_message_iter_close_container(interface_entry, properties) &&
		dbus_message_iter_close_container(interfaces, interface_entry);
}

static bool close_object(DBusMessageIter *objects,
	DBusMessageIter *object_entry, DBusMessageIter *interfaces)
{
	return dbus_message_iter_close_container(object_entry, interfaces) &&
		dbus_message_iter_close_container(objects, object_entry);
}

static bool append_device(DBusMessageIter *objects, const char *path,
	const char *address, const char *name, const char *alias,
	const char *icon, const char *uuid, dbus_uint32_t class_of_device,
	dbus_int16_t rssi, bool paired_value, bool trusted_value,
	bool connected_value)
{
	DBusMessageIter object_entry;
	DBusMessageIter interfaces;
	DBusMessageIter interface_entry;
	DBusMessageIter properties;
	dbus_bool_t paired = paired_value ? TRUE : FALSE;
	dbus_bool_t trusted = trusted_value ? TRUE : FALSE;
	dbus_bool_t connected = connected_value ? TRUE : FALSE;

	return open_object(objects, path, &object_entry, &interfaces) &&
		open_interface(&interfaces, "org.bluez.Device1", &interface_entry,
			&properties) &&
		append_basic_property(&properties, "Address", DBUS_TYPE_STRING,
			"s", &address) &&
		append_basic_property(&properties, "Name", DBUS_TYPE_STRING,
			"s", &name) &&
		append_basic_property(&properties, "Alias", DBUS_TYPE_STRING,
			"s", &alias) &&
		append_basic_property(&properties, "Icon", DBUS_TYPE_STRING,
			"s", &icon) &&
		append_basic_property(&properties, "Class", DBUS_TYPE_UINT32,
			"u", &class_of_device) &&
		append_basic_property(&properties, "RSSI", DBUS_TYPE_INT16,
			"n", &rssi) &&
		append_basic_property(&properties, "Paired", DBUS_TYPE_BOOLEAN,
			"b", &paired) &&
		append_basic_property(&properties, "Trusted", DBUS_TYPE_BOOLEAN,
			"b", &trusted) &&
		append_basic_property(&properties, "Connected", DBUS_TYPE_BOOLEAN,
			"b", &connected) &&
		append_uuids(&properties, uuid) &&
		close_interface(&interfaces, &interface_entry, &properties) &&
		close_object(objects, &object_entry, &interfaces);
}

static DBusMessage *managed_objects_reply(DBusMessage *request,
	struct fake_state *state)
{
	DBusMessage *reply = dbus_message_new_method_return(request);
	DBusMessageIter root;
	DBusMessageIter objects;
	DBusMessageIter object_entry;
	DBusMessageIter interfaces;
	DBusMessageIter interface_entry;
	DBusMessageIter properties;
	dbus_bool_t powered = state->powered ? TRUE : FALSE;
	dbus_bool_t discovering = state->discovering ? TRUE : FALSE;
	const char *adapter_address = "10:22:33:44:55:66";

	if (reply == NULL)
		return NULL;
	if (access(state->pair_marker, F_OK) == 0)
		state->paired = true;
	dbus_message_iter_init_append(reply, &root);
	if (!dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY,
			"{oa{sa{sv}}}", &objects) ||
	    !open_object(&objects, ADAPTER_PATH, &object_entry, &interfaces) ||
	    !open_interface(&interfaces, "org.bluez.Adapter1", &interface_entry,
			&properties) ||
	    !append_basic_property(&properties, "Address", DBUS_TYPE_STRING,
			"s", &adapter_address) ||
	    !append_basic_property(&properties, "Powered", DBUS_TYPE_BOOLEAN,
			"b", &powered) ||
	    !append_basic_property(&properties, "Discovering", DBUS_TYPE_BOOLEAN,
			"b", &discovering) ||
	    !close_interface(&interfaces, &interface_entry, &properties) ||
	    !close_object(&objects, &object_entry, &interfaces))
		goto failure;
	if (state->device_present) {
		const char *address = "AA:BB:CC:DD:EE:FF";
		const char *name = "Fixture Gamepad";
		const char *alias = "Gamepad 手把";
		const char *icon = "input-gaming";
		const char *uuid = "00001124-0000-1000-8000-00805f9b34fb";

		if (!append_device(&objects, DEVICE_PATH, address, name, alias, icon,
			uuid, 0x2508U, -42, state->paired, state->trusted,
			state->connected))
			goto failure;
	}
	if (state->device2_present) {
		const char *address = "11:22:33:44:55:66";
		const char *name = "Fixture Headset";
		const char *alias = "Headset 耳機";
		const char *icon = "audio-headset";
		const char *uuid = "0000110b-0000-1000-8000-00805f9b34fb";

		if (!append_device(&objects, DEVICE2_PATH, address, name, alias, icon,
			uuid, 0x240418U, -36, state->device2_paired,
			state->device2_trusted, state->device2_connected))
			goto failure;
	}
	if (!dbus_message_iter_close_container(&root, &objects))
		goto failure;
	return reply;

failure:
	dbus_message_unref(reply);
	return NULL;
}

static bool read_set_boolean(DBusMessage *message, const char **interface,
	const char **property, bool *enabled)
{
	DBusMessageIter fields;
	DBusMessageIter value;
	dbus_bool_t parsed;

	if (!dbus_message_iter_init(message, &fields) ||
	    dbus_message_iter_get_arg_type(&fields) != DBUS_TYPE_STRING)
		return false;
	dbus_message_iter_get_basic(&fields, interface);
	if (!dbus_message_iter_next(&fields) ||
	    dbus_message_iter_get_arg_type(&fields) != DBUS_TYPE_STRING)
		return false;
	dbus_message_iter_get_basic(&fields, property);
	if (!dbus_message_iter_next(&fields) ||
	    dbus_message_iter_get_arg_type(&fields) != DBUS_TYPE_VARIANT)
		return false;
	dbus_message_iter_recurse(&fields, &value);
	if (dbus_message_iter_get_arg_type(&value) != DBUS_TYPE_BOOLEAN)
		return false;
	dbus_message_iter_get_basic(&value, &parsed);
	*enabled = parsed != FALSE;
	return true;
}

static DBusMessage *handle_message(DBusMessage *message,
	struct fake_state *state)
{
	const char *path = dbus_message_get_path(message);
	const char *interface = dbus_message_get_interface(message);
	const char *member = dbus_message_get_member(message);

	if (path == NULL || interface == NULL || member == NULL)
		return dbus_message_new_error(message, DBUS_ERROR_INVALID_ARGS,
			"missing message metadata");
	if (strcmp(path, "/") == 0 &&
	    strcmp(interface, "org.freedesktop.DBus.ObjectManager") == 0 &&
	    strcmp(member, "GetManagedObjects") == 0)
		return managed_objects_reply(message, state);
	if (strcmp(interface, "org.freedesktop.DBus.Properties") == 0 &&
	    strcmp(member, "Set") == 0) {
		const char *target_interface;
		const char *property;
		bool enabled;

		if (!read_set_boolean(message, &target_interface, &property,
				&enabled))
			return dbus_message_new_error(message, DBUS_ERROR_INVALID_ARGS,
				"invalid property");
		if (strcmp(path, ADAPTER_PATH) == 0 &&
		    strcmp(target_interface, "org.bluez.Adapter1") == 0 &&
		    strcmp(property, "Powered") == 0) {
			state->powered = enabled;
			if (!enabled)
				state->discovering = false;
		} else if (strcmp(path, DEVICE_PATH) == 0 &&
			   strcmp(target_interface, "org.bluez.Device1") == 0 &&
			   strcmp(property, "Trusted") == 0) {
			state->trusted = enabled;
		} else if (strcmp(path, DEVICE2_PATH) == 0 &&
			   strcmp(target_interface, "org.bluez.Device1") == 0 &&
			   strcmp(property, "Trusted") == 0) {
			state->device2_trusted = enabled;
		} else {
			return dbus_message_new_error(message, DBUS_ERROR_UNKNOWN_PROPERTY,
				"unknown property");
		}
		return dbus_message_new_method_return(message);
	}
	if (strcmp(path, ADAPTER_PATH) == 0 &&
	    strcmp(interface, "org.bluez.Adapter1") == 0) {
		if (strcmp(member, "StartDiscovery") == 0)
			state->discovering = true;
		else if (strcmp(member, "StopDiscovery") == 0)
			state->discovering = false;
		else if (strcmp(member, "RemoveDevice") == 0) {
			DBusMessageIter argument;
			const char *device_path;

			if (!dbus_message_iter_init(message, &argument) ||
			    dbus_message_iter_get_arg_type(&argument) !=
				DBUS_TYPE_OBJECT_PATH)
				return dbus_message_new_error(message,
					DBUS_ERROR_INVALID_ARGS, "missing device path");
			dbus_message_iter_get_basic(&argument, &device_path);
			if (strcmp(device_path, DEVICE_PATH) == 0) {
				state->device_present = false;
				state->connected = false;
				state->paired = false;
			} else if (strcmp(device_path, DEVICE2_PATH) == 0) {
				state->device2_present = false;
				state->device2_connected = false;
				state->device2_paired = false;
			} else {
				return dbus_message_new_error(message,
					DBUS_ERROR_UNKNOWN_OBJECT,
					"unknown device path");
			}
		} else
			return dbus_message_new_error(message, DBUS_ERROR_UNKNOWN_METHOD,
				"unknown adapter method");
		return dbus_message_new_method_return(message);
	}
	if (strcmp(path, DEVICE_PATH) == 0 &&
	    strcmp(interface, "org.bluez.Device1") == 0) {
		if (strcmp(member, "Connect") == 0)
			state->connected = true;
		else if (strcmp(member, "Disconnect") == 0)
			state->connected = false;
		else
			return dbus_message_new_error(message, DBUS_ERROR_UNKNOWN_METHOD,
				"unknown device method");
		return dbus_message_new_method_return(message);
	}
	if (strcmp(path, DEVICE2_PATH) == 0 &&
	    strcmp(interface, "org.bluez.Device1") == 0) {
		if (strcmp(member, "Connect") == 0)
			state->device2_connected = true;
		else if (strcmp(member, "Disconnect") == 0)
			state->device2_connected = false;
		else
			return dbus_message_new_error(message,
				DBUS_ERROR_UNKNOWN_METHOD, "unknown device method");
		return dbus_message_new_method_return(message);
	}
	return dbus_message_new_error(message, DBUS_ERROR_UNKNOWN_METHOD,
		"unsupported method");
}

int main(int argc, char **argv)
{
	DBusConnection *connection;
	DBusError error;
	struct fake_state state = {
		.powered = true,
		.device_present = true,
		.device2_present = true,
		.device2_paired = true,
		.device2_trusted = true,
	};
	FILE *ready;
	int request_result;

	if (argc != 3)
		return 64;
	state.pair_marker = argv[2];
	(void)signal(SIGTERM, stop_running);
	(void)signal(SIGINT, stop_running);
	dbus_error_init(&error);
	connection = dbus_bus_get_private(DBUS_BUS_SYSTEM, &error);
	if (connection == NULL)
		return 69;
	dbus_connection_set_exit_on_disconnect(connection, FALSE);
	request_result = dbus_bus_request_name(connection, "org.bluez",
		DBUS_NAME_FLAG_DO_NOT_QUEUE, &error);
	if (request_result != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER)
		return 69;
	ready = fopen(argv[1], "w");
	if (ready == NULL)
		return 73;
	(void)fputs("ready\n", ready);
	if (fclose(ready) != 0)
		return 73;
	while (running) {
		DBusMessage *message;
		DBusMessage *reply;

		if (!dbus_connection_read_write(connection, 100))
			break;
		message = dbus_connection_pop_message(connection);
		if (message == NULL)
			continue;
		if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_METHOD_CALL) {
			dbus_message_unref(message);
			continue;
		}
		reply = handle_message(message, &state);
		if (reply == NULL || !dbus_connection_send(connection, reply, NULL)) {
			if (reply != NULL)
				dbus_message_unref(reply);
			dbus_message_unref(message);
			break;
		}
		dbus_connection_flush(connection);
		dbus_message_unref(reply);
		dbus_message_unref(message);
	}
	dbus_connection_close(connection);
	dbus_connection_unref(connection);
	if (dbus_error_is_set(&error))
		dbus_error_free(&error);
	return 0;
}
