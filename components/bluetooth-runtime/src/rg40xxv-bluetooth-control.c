#define _POSIX_C_SOURCE 200809L

#include "bluetooth_model.h"

#include <dbus/dbus.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BLUEZ_SERVICE "org.bluez"
#define OBJECT_MANAGER_INTERFACE "org.freedesktop.DBus.ObjectManager"
#define PROPERTIES_INTERFACE "org.freedesktop.DBus.Properties"
#define ADAPTER_INTERFACE "org.bluez.Adapter1"
#define DEVICE_INTERFACE "org.bluez.Device1"
#define HCI0_PATH "/org/bluez/hci0"

#ifndef RG40XXV_BLUETOOTHCTL_PATH
#define RG40XXV_BLUETOOTHCTL_PATH "/usr/bin/bluetoothctl"
#endif

#ifndef RG40XXV_DISCOVERY_SECONDS
#define RG40XXV_DISCOVERY_SECONDS 7
#endif

enum {
	DBUS_STATUS_TIMEOUT_MS = 4000,
	DBUS_ACTION_TIMEOUT_MS = 10000,
};

static char *const bluetoothctl_environment[] = {
	(char *)"PATH=/usr/sbin:/usr/bin:/sbin:/bin",
	(char *)"LC_ALL=C",
	(char *)"LANG=C",
	(char *)"TERM=dumb",
	(char *)"NO_COLOR=1",
	NULL,
};

static void usage(const char *program)
{
	(void)fprintf(stderr,
		"usage: %s status | scan | power on|off | pair MAC | "
		"connect MAC | disconnect MAC | forget MAC\n", program);
}

static bool valid_invocation(int argc, char **argv)
{
	char normalized[RG40XXV_BT_ADDRESS_SIZE];

	if (argc == 2 && (strcmp(argv[1], "status") == 0 ||
			 strcmp(argv[1], "scan") == 0))
		return true;
	if (argc != 3)
		return false;
	if (strcmp(argv[1], "power") == 0)
		return strcmp(argv[2], "on") == 0 || strcmp(argv[2], "off") == 0;
	if (strcmp(argv[1], "pair") != 0 && strcmp(argv[1], "connect") != 0 &&
	    strcmp(argv[1], "disconnect") != 0 && strcmp(argv[1], "forget") != 0)
		return false;
	return rg40xxv_bt_normalize_mac(argv[2], normalized);
}

static int copy_string(char *destination, size_t size, const char *source)
{
	int length;

	if (destination == NULL || size == 0U || source == NULL)
		return EINVAL;
	length = snprintf(destination, size, "%s", source);
	return length >= 0 && (size_t)length < size ? 0 : ENAMETOOLONG;
}

static int dbus_error_code(const DBusError *error)
{
	if (error == NULL || !dbus_error_is_set(error))
		return EIO;
	if (strcmp(error->name, DBUS_ERROR_NO_REPLY) == 0 ||
	    strcmp(error->name, DBUS_ERROR_TIMEOUT) == 0)
		return ETIMEDOUT;
	if (strcmp(error->name, DBUS_ERROR_SERVICE_UNKNOWN) == 0 ||
	    strcmp(error->name, DBUS_ERROR_NAME_HAS_NO_OWNER) == 0)
		return ENODEV;
	if (strcmp(error->name, DBUS_ERROR_ACCESS_DENIED) == 0 ||
	    strcmp(error->name, DBUS_ERROR_AUTH_FAILED) == 0)
		return EACCES;
	return EIO;
}

static void report_dbus_error(const char *operation, const DBusError *error)
{
	(void)fprintf(stderr, "%s: %s: %s\n", operation,
		error != NULL && error->name != NULL ? error->name : "D-Bus error",
		error != NULL && error->message != NULL ? error->message : "unknown");
}

static bool error_is(const DBusError *error, const char *name)
{
	return error != NULL && dbus_error_is_set(error) && error->name != NULL &&
		strcmp(error->name, name) == 0;
}

static int call_and_discard(DBusConnection *connection, DBusMessage *message,
	int timeout_ms, const char *operation, const char *accepted_error)
{
	DBusError error;
	DBusMessage *reply;
	int result = 0;

	if (message == NULL)
		return ENOMEM;
	dbus_error_init(&error);
	reply = dbus_connection_send_with_reply_and_block(connection, message,
		timeout_ms, &error);
	dbus_message_unref(message);
	if (reply != NULL) {
		dbus_message_unref(reply);
	} else if (accepted_error != NULL && error_is(&error, accepted_error)) {
		result = 0;
	} else {
		report_dbus_error(operation, &error);
		result = dbus_error_code(&error);
	}
	if (dbus_error_is_set(&error))
		dbus_error_free(&error);
	return result;
}

static int call_no_arguments(DBusConnection *connection, const char *path,
	const char *interface, const char *method, const char *accepted_error)
{
	DBusMessage *message = dbus_message_new_method_call(BLUEZ_SERVICE, path,
		interface, method);

	return call_and_discard(connection, message, DBUS_ACTION_TIMEOUT_MS,
		method, accepted_error);
}

static int call_object_path(DBusConnection *connection, const char *path,
	const char *interface, const char *method, const char *object_path)
{
	DBusMessage *message = dbus_message_new_method_call(BLUEZ_SERVICE, path,
		interface, method);

	if (message == NULL)
		return ENOMEM;
	if (!dbus_message_append_args(message, DBUS_TYPE_OBJECT_PATH, &object_path,
		DBUS_TYPE_INVALID)) {
		dbus_message_unref(message);
		return ENOMEM;
	}
	return call_and_discard(connection, message, DBUS_ACTION_TIMEOUT_MS,
		method, NULL);
}

static int set_boolean_property(DBusConnection *connection, const char *path,
	const char *interface, const char *property, bool enabled)
{
	DBusMessage *message = dbus_message_new_method_call(BLUEZ_SERVICE, path,
		PROPERTIES_INTERFACE, "Set");
	DBusMessageIter arguments;
	DBusMessageIter variant;
	dbus_bool_t value = enabled ? TRUE : FALSE;
	const char *variant_signature = DBUS_TYPE_BOOLEAN_AS_STRING;

	if (message == NULL)
		return ENOMEM;
	dbus_message_iter_init_append(message, &arguments);
	if (!dbus_message_iter_append_basic(&arguments, DBUS_TYPE_STRING,
			&interface) ||
	    !dbus_message_iter_append_basic(&arguments, DBUS_TYPE_STRING,
			&property) ||
	    !dbus_message_iter_open_container(&arguments, DBUS_TYPE_VARIANT,
			variant_signature, &variant) ||
	    !dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &value) ||
	    !dbus_message_iter_close_container(&arguments, &variant)) {
		dbus_message_unref(message);
		return ENOMEM;
	}
	return call_and_discard(connection, message, DBUS_ACTION_TIMEOUT_MS,
		property, NULL);
}

static bool variant_boolean(DBusMessageIter *variant, bool *output)
{
	DBusMessageIter value;
	dbus_bool_t parsed;

	if (dbus_message_iter_get_arg_type(variant) != DBUS_TYPE_VARIANT)
		return false;
	dbus_message_iter_recurse(variant, &value);
	if (dbus_message_iter_get_arg_type(&value) != DBUS_TYPE_BOOLEAN)
		return false;
	dbus_message_iter_get_basic(&value, &parsed);
	*output = parsed != FALSE;
	return true;
}

static bool variant_string(DBusMessageIter *variant, char *output,
	size_t output_size)
{
	DBusMessageIter value;
	const char *parsed;

	if (dbus_message_iter_get_arg_type(variant) != DBUS_TYPE_VARIANT)
		return false;
	dbus_message_iter_recurse(variant, &value);
	if (dbus_message_iter_get_arg_type(&value) != DBUS_TYPE_STRING)
		return false;
	dbus_message_iter_get_basic(&value, &parsed);
	return copy_string(output, output_size, parsed) == 0;
}

static bool variant_uint32(DBusMessageIter *variant, uint32_t *output)
{
	DBusMessageIter value;
	dbus_uint32_t parsed;

	if (dbus_message_iter_get_arg_type(variant) != DBUS_TYPE_VARIANT)
		return false;
	dbus_message_iter_recurse(variant, &value);
	if (dbus_message_iter_get_arg_type(&value) != DBUS_TYPE_UINT32)
		return false;
	dbus_message_iter_get_basic(&value, &parsed);
	*output = parsed;
	return true;
}

static bool variant_int16(DBusMessageIter *variant, int *output)
{
	DBusMessageIter value;
	dbus_int16_t parsed;

	if (dbus_message_iter_get_arg_type(variant) != DBUS_TYPE_VARIANT)
		return false;
	dbus_message_iter_recurse(variant, &value);
	if (dbus_message_iter_get_arg_type(&value) != DBUS_TYPE_INT16)
		return false;
	dbus_message_iter_get_basic(&value, &parsed);
	*output = parsed;
	return true;
}

static bool uuid_prefix(const char *uuid, const char *short_uuid)
{
	return uuid != NULL && strncasecmp(uuid, short_uuid, strlen(short_uuid)) == 0;
}

static void parse_uuids(DBusMessageIter *variant,
	struct rg40xxv_bt_device *device)
{
	DBusMessageIter value;
	DBusMessageIter entries;

	if (dbus_message_iter_get_arg_type(variant) != DBUS_TYPE_VARIANT)
		return;
	dbus_message_iter_recurse(variant, &value);
	if (dbus_message_iter_get_arg_type(&value) != DBUS_TYPE_ARRAY)
		return;
	dbus_message_iter_recurse(&value, &entries);
	while (dbus_message_iter_get_arg_type(&entries) == DBUS_TYPE_STRING) {
		const char *uuid;

		dbus_message_iter_get_basic(&entries, &uuid);
		if (uuid_prefix(uuid, "00001124-") ||
		    uuid_prefix(uuid, "00001812-"))
			device->has_hid_uuid = true;
		if (uuid_prefix(uuid, "00001108-") ||
		    uuid_prefix(uuid, "0000110a-") ||
		    uuid_prefix(uuid, "0000110b-") ||
		    uuid_prefix(uuid, "0000110d-") ||
		    uuid_prefix(uuid, "0000110e-") ||
		    uuid_prefix(uuid, "0000111e-"))
			device->has_audio_uuid = true;
		if (!dbus_message_iter_next(&entries))
			break;
	}
}

static bool property_entry(DBusMessageIter *entry, const char **name,
	DBusMessageIter *variant)
{
	DBusMessageIter fields;

	if (dbus_message_iter_get_arg_type(entry) != DBUS_TYPE_DICT_ENTRY)
		return false;
	dbus_message_iter_recurse(entry, &fields);
	if (dbus_message_iter_get_arg_type(&fields) != DBUS_TYPE_STRING)
		return false;
	dbus_message_iter_get_basic(&fields, name);
	if (!dbus_message_iter_next(&fields) ||
	    dbus_message_iter_get_arg_type(&fields) != DBUS_TYPE_VARIANT)
		return false;
	*variant = fields;
	return true;
}

static void parse_adapter_properties(DBusMessageIter *properties,
	struct rg40xxv_bt_model *adapter)
{
	DBusMessageIter entries;

	dbus_message_iter_recurse(properties, &entries);
	while (dbus_message_iter_get_arg_type(&entries) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter variant;
		const char *name;

		if (property_entry(&entries, &name, &variant)) {
			if (strcmp(name, "Address") == 0)
				(void)variant_string(&variant, adapter->adapter_address,
					sizeof(adapter->adapter_address));
			else if (strcmp(name, "Powered") == 0)
				(void)variant_boolean(&variant, &adapter->powered);
			else if (strcmp(name, "Discovering") == 0)
				(void)variant_boolean(&variant, &adapter->discovering);
		}
		if (!dbus_message_iter_next(&entries))
			break;
	}
}

static void parse_device_properties(DBusMessageIter *properties,
	struct rg40xxv_bt_device *device)
{
	DBusMessageIter entries;

	device->rssi = 127;
	dbus_message_iter_recurse(properties, &entries);
	while (dbus_message_iter_get_arg_type(&entries) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter variant;
		const char *name;

		if (property_entry(&entries, &name, &variant)) {
			if (strcmp(name, "Address") == 0)
				(void)variant_string(&variant, device->address,
					sizeof(device->address));
			else if (strcmp(name, "Name") == 0)
				(void)variant_string(&variant, device->raw_name,
					sizeof(device->raw_name));
			else if (strcmp(name, "Alias") == 0)
				(void)variant_string(&variant, device->raw_alias,
					sizeof(device->raw_alias));
			else if (strcmp(name, "Icon") == 0)
				(void)variant_string(&variant, device->icon,
					sizeof(device->icon));
			else if (strcmp(name, "Class") == 0)
				(void)variant_uint32(&variant,
					&device->class_of_device);
			else if (strcmp(name, "RSSI") == 0)
				(void)variant_int16(&variant, &device->rssi);
			else if (strcmp(name, "Paired") == 0)
				(void)variant_boolean(&variant, &device->paired);
			else if (strcmp(name, "Trusted") == 0)
				(void)variant_boolean(&variant, &device->trusted);
			else if (strcmp(name, "Connected") == 0)
				(void)variant_boolean(&variant, &device->connected);
			else if (strcmp(name, "UUIDs") == 0)
				parse_uuids(&variant, device);
		}
		if (!dbus_message_iter_next(&entries))
			break;
	}
}

static bool interface_entry(DBusMessageIter *entry, const char **name,
	DBusMessageIter *properties)
{
	DBusMessageIter fields;

	if (dbus_message_iter_get_arg_type(entry) != DBUS_TYPE_DICT_ENTRY)
		return false;
	dbus_message_iter_recurse(entry, &fields);
	if (dbus_message_iter_get_arg_type(&fields) != DBUS_TYPE_STRING)
		return false;
	dbus_message_iter_get_basic(&fields, name);
	if (!dbus_message_iter_next(&fields) ||
	    dbus_message_iter_get_arg_type(&fields) != DBUS_TYPE_ARRAY)
		return false;
	*properties = fields;
	return true;
}

static void select_adapter(struct rg40xxv_bt_model *model, const char *path,
	DBusMessageIter *properties)
{
	bool prefer = !model->adapter_present || strcmp(path, HCI0_PATH) == 0;

	if (!prefer || (strcmp(model->adapter_path, HCI0_PATH) == 0 &&
		strcmp(path, HCI0_PATH) != 0))
		return;
	model->powered = false;
	model->discovering = false;
	model->adapter_address[0] = '\0';
	if (copy_string(model->adapter_path, sizeof(model->adapter_path), path) != 0)
		return;
	parse_adapter_properties(properties, model);
	model->adapter_present = true;
}

static void parse_interfaces(struct rg40xxv_bt_model *model, const char *path,
	DBusMessageIter *interfaces)
{
	DBusMessageIter entries;

	dbus_message_iter_recurse(interfaces, &entries);
	while (dbus_message_iter_get_arg_type(&entries) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter properties;
		const char *name;

		if (interface_entry(&entries, &name, &properties)) {
			if (strcmp(name, ADAPTER_INTERFACE) == 0) {
				select_adapter(model, path, &properties);
			} else if (strcmp(name, DEVICE_INTERFACE) == 0 &&
				   model->device_count < RG40XXV_BT_MAX_DEVICES) {
				struct rg40xxv_bt_device *device =
					&model->devices[model->device_count];

				memset(device, 0, sizeof(*device));
				if (copy_string(device->path, sizeof(device->path),
						path) == 0) {
					parse_device_properties(&properties, device);
					++model->device_count;
				}
			}
		}
		if (!dbus_message_iter_next(&entries))
			break;
	}
}

static int parse_managed_objects(DBusMessage *reply,
	struct rg40xxv_bt_model *model)
{
	DBusMessageIter top;
	DBusMessageIter objects;

	memset(model, 0, sizeof(*model));
	if (!dbus_message_iter_init(reply, &top) ||
	    dbus_message_iter_get_arg_type(&top) != DBUS_TYPE_ARRAY)
		return EPROTO;
	dbus_message_iter_recurse(&top, &objects);
	while (dbus_message_iter_get_arg_type(&objects) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter fields;
		const char *path;

		dbus_message_iter_recurse(&objects, &fields);
		if (dbus_message_iter_get_arg_type(&fields) != DBUS_TYPE_OBJECT_PATH)
			return EPROTO;
		dbus_message_iter_get_basic(&fields, &path);
		if (!dbus_message_iter_next(&fields) ||
		    dbus_message_iter_get_arg_type(&fields) != DBUS_TYPE_ARRAY)
			return EPROTO;
		parse_interfaces(model, path, &fields);
		if (!dbus_message_iter_next(&objects))
			break;
	}
	return 0;
}

static bool device_belongs_to_adapter(const char *adapter, const char *path)
{
	size_t length = strlen(adapter);

	return length > 0U && strncmp(adapter, path, length) == 0 &&
		path[length] == '/';
}

static bool address_already_present(const struct rg40xxv_bt_model *model,
	size_t count, const char *address)
{
	for (size_t index = 0U; index < count; ++index) {
		if (strcmp(model->devices[index].address, address) == 0)
			return true;
	}
	return false;
}

static void finalize_model(struct rg40xxv_bt_model *model)
{
	size_t kept = 0U;

	if (!model->adapter_present) {
		model->device_count = 0U;
		return;
	}
	for (size_t index = 0U; index < model->device_count; ++index) {
		struct rg40xxv_bt_device device = model->devices[index];
		char normalized[RG40XXV_BT_ADDRESS_SIZE];
		const char *display_name;

		if (!device_belongs_to_adapter(model->adapter_path, device.path) ||
		    !rg40xxv_bt_normalize_mac(device.address, normalized) ||
		    address_already_present(model, kept, normalized))
			continue;
		(void)copy_string(device.address, sizeof(device.address), normalized);
		display_name = device.raw_alias[0] != '\0' ? device.raw_alias :
			device.raw_name;
		rg40xxv_bt_sanitize_name(display_name, device.address, device.name);
		device.kind = rg40xxv_bt_classify(device.icon,
			device.class_of_device, device.has_hid_uuid,
			device.has_audio_uuid);
		model->devices[kept++] = device;
	}
	model->device_count = kept;
	rg40xxv_bt_sort(model);
}

static int get_model(DBusConnection *connection,
	struct rg40xxv_bt_model *model)
{
	DBusMessage *message = dbus_message_new_method_call(BLUEZ_SERVICE, "/",
		OBJECT_MANAGER_INTERFACE, "GetManagedObjects");
	DBusMessage *reply;
	DBusError error;
	int result;

	if (message == NULL)
		return ENOMEM;
	dbus_error_init(&error);
	reply = dbus_connection_send_with_reply_and_block(connection, message,
		DBUS_STATUS_TIMEOUT_MS, &error);
	dbus_message_unref(message);
	if (reply == NULL) {
		report_dbus_error("GetManagedObjects", &error);
		result = dbus_error_code(&error);
		if (dbus_error_is_set(&error))
			dbus_error_free(&error);
		return result;
	}
	result = parse_managed_objects(reply, model);
	dbus_message_unref(reply);
	if (dbus_error_is_set(&error))
		dbus_error_free(&error);
	if (result == 0)
		finalize_model(model);
	return result;
}

static struct rg40xxv_bt_device *find_device(struct rg40xxv_bt_model *model,
	const char *address)
{
	for (size_t index = 0U; index < model->device_count; ++index) {
		if (strcmp(model->devices[index].address, address) == 0)
			return &model->devices[index];
	}
	return NULL;
}

static int run_bluetoothctl_pair(const char *address)
{
	const char *program = RG40XXV_BLUETOOTHCTL_PATH;
	char *const arguments[] = {
		(char *)program, (char *)"--timeout", (char *)"90",
		(char *)"pair", (char *)address, NULL,
	};
	posix_spawn_file_actions_t actions;
	posix_spawnattr_t attributes;
	sigset_t defaults;
	sigset_t empty;
	pid_t child;
	int null_fd;
	int status;
	int result;
	bool actions_initialized = false;
	bool attributes_initialized = false;

	null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
	if (null_fd < 0)
		return errno;
	result = posix_spawn_file_actions_init(&actions);
	if (result == 0)
		actions_initialized = true;
	if (result == 0)
		result = posix_spawn_file_actions_adddup2(&actions, null_fd,
			STDIN_FILENO);
	if (result == 0)
		result = posix_spawn_file_actions_adddup2(&actions, null_fd,
			STDOUT_FILENO);
	if (result == 0)
		result = posix_spawn_file_actions_adddup2(&actions, null_fd,
			STDERR_FILENO);
	if (result == 0)
		result = posix_spawnattr_init(&attributes);
	if (result == 0)
		attributes_initialized = true;
	(void)sigemptyset(&empty);
	(void)sigemptyset(&defaults);
	(void)sigaddset(&defaults, SIGINT);
	(void)sigaddset(&defaults, SIGTERM);
	(void)sigaddset(&defaults, SIGHUP);
	(void)sigaddset(&defaults, SIGQUIT);
	(void)sigaddset(&defaults, SIGPIPE);
	if (result == 0)
		result = posix_spawnattr_setflags(&attributes,
			POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
	if (result == 0)
		result = posix_spawnattr_setsigmask(&attributes, &empty);
	if (result == 0)
		result = posix_spawnattr_setsigdefault(&attributes, &defaults);
	if (result == 0)
		result = posix_spawn(&child, program, &actions, &attributes,
			arguments, bluetoothctl_environment);
	if (result == 0) {
		do {
			result = waitpid(child, &status, 0) < 0 ? errno : 0;
		} while (result == EINTR);
		if (result == 0 && (!WIFEXITED(status) || WEXITSTATUS(status) != 0))
			result = EIO;
	}
	if (attributes_initialized)
		(void)posix_spawnattr_destroy(&attributes);
	if (actions_initialized)
		(void)posix_spawn_file_actions_destroy(&actions);
	(void)close(null_fd);
	return result;
}

static int sleep_discovery_window(void)
{
	struct timespec remaining = {
		.tv_sec = RG40XXV_DISCOVERY_SECONDS,
		.tv_nsec = 0,
	};

	while (nanosleep(&remaining, &remaining) != 0) {
		if (errno != EINTR)
			return errno;
	}
	return 0;
}

static int perform_action(DBusConnection *connection, int argc, char **argv,
	struct rg40xxv_bt_model *model)
{
	char address[RG40XXV_BT_ADDRESS_SIZE];
	struct rg40xxv_bt_device *device;
	int result = get_model(connection, model);

	if (result != 0)
		return result;
	if (strcmp(argv[1], "status") == 0)
		return argc == 2 ? 0 : EINVAL;
	if (!model->adapter_present)
		return ENODEV;
	if (strcmp(argv[1], "scan") == 0) {
		if (argc != 2 || !model->powered)
			return argc != 2 ? EINVAL : EHOSTDOWN;
		result = call_no_arguments(connection, model->adapter_path,
			ADAPTER_INTERFACE, "StartDiscovery",
			"org.bluez.Error.InProgress");
		if (result == 0)
			result = sleep_discovery_window();
		if (call_no_arguments(connection, model->adapter_path,
			ADAPTER_INTERFACE, "StopDiscovery",
			"org.bluez.Error.NotReady") != 0 && result == 0)
			result = EIO;
		return result == 0 ? get_model(connection, model) : result;
	}
	if (strcmp(argv[1], "power") == 0) {
		bool enabled;

		if (argc != 3 || (strcmp(argv[2], "on") != 0 &&
				strcmp(argv[2], "off") != 0))
			return EINVAL;
		enabled = strcmp(argv[2], "on") == 0;
		if (enabled != model->powered)
			result = set_boolean_property(connection, model->adapter_path,
				ADAPTER_INTERFACE, "Powered", enabled);
		return result == 0 ? get_model(connection, model) : result;
	}
	if (argc != 3 || !rg40xxv_bt_normalize_mac(argv[2], address))
		return EINVAL;
	device = find_device(model, address);
	if (strcmp(argv[1], "forget") == 0) {
		if (device == NULL)
			return 0;
		result = call_object_path(connection, model->adapter_path,
			ADAPTER_INTERFACE, "RemoveDevice", device->path);
		return result == 0 ? get_model(connection, model) : result;
	}
	if (device == NULL)
		return ENODEV;
	if (strcmp(argv[1], "pair") == 0) {
		if (!device->paired)
			result = run_bluetoothctl_pair(address);
		if (result == 0) {
			result = get_model(connection, model);
			device = result == 0 ? find_device(model, address) : NULL;
			if (result == 0 && device == NULL)
				result = ENODEV;
		}
		if (result == 0)
			result = set_boolean_property(connection, device->path,
				DEVICE_INTERFACE, "Trusted", true);
		/* Pairing from the handheld is one user action.  Do not leave a
		 * successfully paired controller in BlueZ's disconnected state and
		 * require a second press before it can produce input. */
		if (result == 0) {
			result = get_model(connection, model);
			device = result == 0 ? find_device(model, address) : NULL;
			if (result == 0 && device == NULL)
				result = ENODEV;
		}
		if (result == 0 && !device->connected)
			result = call_no_arguments(connection, device->path,
				DEVICE_INTERFACE, "Connect",
				"org.bluez.Error.AlreadyConnected");
		return result == 0 ? get_model(connection, model) : result;
	}
	if (strcmp(argv[1], "connect") == 0) {
		if (device->paired && !device->trusted) {
			result = set_boolean_property(connection, device->path,
				DEVICE_INTERFACE, "Trusted", true);
			if (result != 0)
				return result;
		}
		if (!device->connected)
			result = call_no_arguments(connection, device->path,
				DEVICE_INTERFACE, "Connect",
				"org.bluez.Error.AlreadyConnected");
		return result == 0 ? get_model(connection, model) : result;
	}
	if (strcmp(argv[1], "disconnect") == 0) {
		if (device->connected)
			result = call_no_arguments(connection, device->path,
				DEVICE_INTERFACE, "Disconnect",
				"org.bluez.Error.NotConnected");
		return result == 0 ? get_model(connection, model) : result;
	}
	return EINVAL;
}

int main(int argc, char **argv)
{
	DBusConnection *connection;
	DBusError error;
	struct rg40xxv_bt_model model;
	int result;

	if (!valid_invocation(argc, argv)) {
		usage(argv[0]);
		return 64;
	}
	dbus_error_init(&error);
	connection = dbus_bus_get_private(DBUS_BUS_SYSTEM, &error);
	if (connection == NULL) {
		report_dbus_error("system bus", &error);
		if (dbus_error_is_set(&error))
			dbus_error_free(&error);
		return 69;
	}
	dbus_connection_set_exit_on_disconnect(connection, FALSE);
	result = perform_action(connection, argc, argv, &model);
	if (result == 0)
		result = rg40xxv_bt_write_snapshot(stdout, &model);
	if (result != 0)
		(void)fprintf(stderr, "bluetooth action failed: %s\n",
			strerror(result));
	dbus_connection_close(connection);
	dbus_connection_unref(connection);
	if (dbus_error_is_set(&error))
		dbus_error_free(&error);
	return result == 0 ? 0 : result == EINVAL ? 64 :
		result == ENODEV ? 69 : result == ETIMEDOUT ? 75 : 1;
}
