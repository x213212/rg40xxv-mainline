#include "netstream.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_STATE_DIR "/var/lib/rg40xxv/netstream"

static void usage(FILE *stream)
{
    (void)fprintf(
        stream,
        "usage: netstreamctl [--state-dir ABSOLUTE_DIR] wifi list\n"
        "       netstreamctl [--state-dir DIR] wifi set --ssid SSID"
        " --security open|wpa2-psk|wpa3-sae [options]\n"
        "       netstreamctl [--state-dir DIR] wifi forget --ssid SSID\n"
        "       netstreamctl [--state-dir DIR] wifi select --ssid SSID\n"
        "       netstreamctl [--state-dir DIR] wifi import-vendor"
        " [--file /mnt/data/.wifi] [--make-default]\n"
        "       netstreamctl [--state-dir DIR] host list\n"
        "       netstreamctl [--state-dir DIR] host set --name NAME"
        " [--address ADDRESS] [options]\n"
        "       netstreamctl [--state-dir DIR] host delete --name NAME\n"
        "       netstreamctl [--state-dir DIR] host select --name NAME\n"
        "\n"
        "Wi-Fi set options: --hidden true|false --priority -999..999\n"
        "  --password-stdin | --password-file ABSOLUTE_FILE --make-default\n"
        "Host set options: --paired true|false --last-used UNIX_SECONDS\n"
        "  --width PX --height PX --custom true|false --fps FPS\n"
        "  --bitrate KBPS --packet-size BYTES --codec H264|H265|AV1\n"
        "  --aspect fit|fill|stretch --make-default\n");
}

static int report(const char *error)
{
    (void)fprintf(stderr, "error: %s\n", error[0] == '\0' ? "operation failed"
                                                              : error);
    return 1;
}

static int copy_text(char *destination, size_t capacity, const char *source,
                     const char *label, char *error, size_t error_size)
{
    size_t length;

    if (source == NULL) {
        (void)snprintf(error, error_size, "missing %s", label);
        return -1;
    }
    length = strlen(source);
    if (length >= capacity) {
        (void)snprintf(error, error_size, "%s is too long", label);
        return -1;
    }
    memcpy(destination, source, length + 1);
    return 0;
}

static int parse_u32_cli(const char *text, uint32_t *value)
{
    char *end;
    unsigned long parsed;

    if (text == NULL || *text == '\0' || *text == '+' || *text == '-')
        return -1;
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || *end != '\0' || parsed > UINT32_MAX)
        return -1;
    *value = (uint32_t)parsed;
    return 0;
}

static int parse_u64_cli(const char *text, uint64_t *value)
{
    char *end;
    unsigned long long parsed;

    if (text == NULL || *text == '\0' || *text == '+' || *text == '-')
        return -1;
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0')
        return -1;
    *value = (uint64_t)parsed;
    return 0;
}

static int parse_priority_cli(const char *text, int *value)
{
    char *end;
    long parsed;

    if (text == NULL || *text == '\0' || *text == '+')
        return -1;
    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno != 0 || *end != '\0' || parsed < -999 || parsed > 999)
        return -1;
    *value = (int)parsed;
    return 0;
}

static int require_value(int argc, char **argv, int *index, const char **value,
                         char *error, size_t error_size)
{
    if (*index + 1 >= argc) {
        (void)snprintf(error, error_size, "%s requires a value", argv[*index]);
        return -1;
    }
    *value = argv[++*index];
    return 0;
}

static int wifi_list(NsStore *store, char *error, size_t error_size)
{
    NsWifiDb db;
    size_t i;

    if (ns_wifi_load(store, &db, error, error_size) < 0)
        return -1;
    (void)puts("selected\tssid\tsecurity\thidden\tpriority");
    for (i = 0; i < db.count; ++i) {
        const NsWifiProfile *profile = &db.profiles[i];

        (void)printf("%s\t%s\t%s\t%s\t%d\n",
                     strcmp(profile->ssid, db.default_ssid) == 0 ? "*" : "",
                     profile->ssid, ns_wifi_security_name(profile->security),
                     profile->hidden ? "true" : "false", profile->priority);
    }
    ns_secure_zero(&db, sizeof(db));
    return 0;
}

static int wifi_set(NsStore *store, int argc, char **argv, char *error,
                    size_t error_size)
{
    NsWifiDb db;
    NsWifiProfile profile = {0};
    const char *ssid = NULL;
    const char *security_text = NULL;
    const char *secret_file = NULL;
    int security_seen = 0;
    int hidden_seen = 0;
    int priority_seen = 0;
    int secret_stdin = 0;
    int make_default = 0;
    int hidden = 0;
    int priority = 0;
    int i;
    int index;
    int result = -1;

    for (i = 0; i < argc; ++i) {
        const char *value;

        if (strcmp(argv[i], "--ssid") == 0) {
            if (ssid != NULL ||
                require_value(argc, argv, &i, &ssid, error, error_size) < 0)
                goto out;
        } else if (strcmp(argv[i], "--security") == 0) {
            if (security_seen || require_value(argc, argv, &i, &value, error,
                                               error_size) < 0)
                goto duplicate;
            security_text = value;
            security_seen = 1;
        } else if (strcmp(argv[i], "--hidden") == 0) {
            if (hidden_seen || require_value(argc, argv, &i, &value, error,
                                             error_size) < 0)
                goto duplicate;
            if (ns_parse_bool(value, &hidden) < 0) {
                (void)snprintf(error, error_size,
                               "--hidden must be true or false");
                goto out;
            }
            hidden_seen = 1;
        } else if (strcmp(argv[i], "--priority") == 0) {
            if (priority_seen || require_value(argc, argv, &i, &value, error,
                                               error_size) < 0)
                goto duplicate;
            if (parse_priority_cli(value, &priority) < 0) {
                (void)snprintf(error, error_size,
                               "--priority must be between -999 and 999");
                goto out;
            }
            priority_seen = 1;
        } else if (strcmp(argv[i], "--password-stdin") == 0) {
            if (secret_stdin || secret_file != NULL)
                goto duplicate;
            secret_stdin = 1;
        } else if (strcmp(argv[i], "--password-file") == 0) {
            if (secret_stdin || secret_file != NULL ||
                require_value(argc, argv, &i, &secret_file, error, error_size) <
                    0)
                goto duplicate;
        } else if (strcmp(argv[i], "--make-default") == 0) {
            if (make_default)
                goto duplicate;
            make_default = 1;
        } else {
            (void)snprintf(error, error_size, "unknown Wi-Fi option");
            goto out;
        }
    }
    if (ssid == NULL) {
        (void)snprintf(error, error_size, "--ssid is required");
        goto out;
    }
    if (ns_wifi_load(store, &db, error, error_size) < 0)
        goto out;
    index = ns_wifi_find(&db, ssid);
    if (index >= 0) {
        profile = db.profiles[index];
    } else {
        if (!security_seen) {
            (void)snprintf(error, error_size,
                           "--security is required for a new Wi-Fi profile");
            goto wipe_db;
        }
        if (copy_text(profile.ssid, sizeof(profile.ssid), ssid, "SSID", error,
                      error_size) < 0)
            goto wipe_db;
    }
    if (security_seen &&
        ns_parse_wifi_security(security_text, &profile.security) < 0) {
        (void)snprintf(error, error_size,
                       "--security must be open, wpa2-psk, or wpa3-sae");
        goto wipe_db;
    }
    if (hidden_seen)
        profile.hidden = hidden;
    if (priority_seen)
        profile.priority = priority;
    if (profile.security == NS_WIFI_OPEN) {
        if (secret_stdin || secret_file != NULL) {
            (void)snprintf(error, error_size,
                           "open Wi-Fi does not accept a password source");
            goto wipe_db;
        }
        ns_secure_zero(profile.secret, sizeof(profile.secret));
    } else if (secret_stdin) {
        if (ns_read_secret_stdin(profile.secret, sizeof(profile.secret), error,
                                 error_size) < 0)
            goto wipe_db;
    } else if (secret_file != NULL) {
        if (ns_read_secret_file(secret_file, profile.secret,
                                sizeof(profile.secret), error, error_size) < 0)
            goto wipe_db;
    } else if (profile.secret[0] == '\0') {
        (void)snprintf(error, error_size,
                       "secured Wi-Fi requires --password-stdin or --password-file");
        goto wipe_db;
    }
    if (ns_wifi_upsert(&db, &profile, error, error_size) < 0 ||
        (make_default &&
         ns_wifi_select(&db, profile.ssid, error, error_size) < 0) ||
        ns_wifi_save(store, &db, error, error_size) < 0)
        goto wipe_db;
    (void)puts("ok");
    result = 0;
wipe_db:
    ns_secure_zero(&db, sizeof(db));
out:
    ns_secure_zero(&profile, sizeof(profile));
    return result;
duplicate:
    (void)snprintf(error, error_size,
                   "duplicate or conflicting Wi-Fi option");
    goto out;
}

static int one_wifi_name(NsStore *store, int argc, char **argv, int forget,
                         char *error, size_t error_size)
{
    NsWifiDb db;
    const char *ssid = NULL;
    int result = -1;

    if (argc != 2 || strcmp(argv[0], "--ssid") != 0) {
        (void)snprintf(error, error_size, "exactly one --ssid is required");
        return -1;
    }
    ssid = argv[1];
    if (ns_wifi_load(store, &db, error, error_size) < 0)
        return -1;
    if ((forget ? ns_wifi_forget(&db, ssid, error, error_size)
                : ns_wifi_select(&db, ssid, error, error_size)) < 0 ||
        ns_wifi_save(store, &db, error, error_size) < 0)
        goto out;
    (void)puts("ok");
    result = 0;
out:
    ns_secure_zero(&db, sizeof(db));
    return result;
}

static int wifi_import(NsStore *store, int argc, char **argv, char *error,
                       size_t error_size)
{
    NsWifiDb db;
    const char *path = "/mnt/data/.wifi";
    int file_seen = 0;
    int make_default = 0;
    int insecure = 0;
    size_t imported = 0;
    int i;
    int result = -1;

    for (i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "--file") == 0) {
            if (file_seen ||
                require_value(argc, argv, &i, &path, error, error_size) < 0)
                goto duplicate;
            file_seen = 1;
        } else if (strcmp(argv[i], "--make-default") == 0) {
            if (make_default)
                goto duplicate;
            make_default = 1;
        } else {
            (void)snprintf(error, error_size, "unknown import option");
            return -1;
        }
    }
    if (ns_wifi_load(store, &db, error, error_size) < 0)
        return -1;
    if (ns_wifi_import_vendor(&db, path, make_default, &imported, &insecure,
                              error, error_size) < 0 ||
        ns_wifi_save(store, &db, error, error_size) < 0)
        goto out;
    if (insecure) {
        (void)fprintf(stderr,
                      "warning: vendor Wi-Fi source is group/world accessible; "
                      "the usual /mnt/data/.wifi mode 0644 exposes plaintext "
                      "credentials. Restrict or remove it manually after migration.\n");
    }
    (void)printf("imported=%zu\n", imported);
    result = 0;
out:
    ns_secure_zero(&db, sizeof(db));
    return result;
duplicate:
    (void)snprintf(error, error_size, "duplicate import option");
    return -1;
}

static int handle_wifi(NsStore *store, int argc, char **argv, char *error,
                       size_t error_size)
{
    if (argc < 1) {
        (void)snprintf(error, error_size, "missing Wi-Fi action");
        return -1;
    }
    if (strcmp(argv[0], "list") == 0 && argc == 1)
        return wifi_list(store, error, error_size);
    if (strcmp(argv[0], "set") == 0)
        return wifi_set(store, argc - 1, argv + 1, error, error_size);
    if (strcmp(argv[0], "forget") == 0)
        return one_wifi_name(store, argc - 1, argv + 1, 1, error, error_size);
    if (strcmp(argv[0], "select") == 0 || strcmp(argv[0], "default") == 0)
        return one_wifi_name(store, argc - 1, argv + 1, 0, error, error_size);
    if (strcmp(argv[0], "import-vendor") == 0)
        return wifi_import(store, argc - 1, argv + 1, error, error_size);
    (void)snprintf(error, error_size, "unknown Wi-Fi action");
    return -1;
}

static int host_list(NsStore *store, char *error, size_t error_size)
{
    NsHostDb db;
    size_t i;

    if (ns_hosts_load(store, &db, error, error_size) < 0)
        return -1;
    (void)puts("selected\tname\taddress\tpaired\tlast_used\twidth\theight\tcustom\tfps\tbitrate_kbps\tpacket_size\tcodec\taspect");
    for (i = 0; i < db.count; ++i) {
        const NsHost *host = &db.hosts[i];

        (void)printf("%s\t%s\t%s\t%s\t%" PRIu64
                     "\t%u\t%u\t%s\t%u\t%u\t%u\t%s\t%s\n",
                     strcmp(host->name, db.default_name) == 0 ? "*" : "",
                     host->name, host->address,
                     host->paired ? "true" : "false", host->last_used,
                     host->resolution.width, host->resolution.height,
                     host->resolution.custom ? "true" : "false", host->fps,
                     host->bitrate_kbps, host->packet_size,
                     ns_codec_name(host->codec), ns_aspect_name(host->aspect));
    }
    return 0;
}

static int host_set(NsStore *store, int argc, char **argv, char *error,
                    size_t error_size)
{
    NsHostDb db;
    NsHost host;
    const char *name = NULL;
    const char *address = NULL;
    const char *codec_text = NULL;
    const char *aspect_text = NULL;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 0;
    uint32_t bitrate = 0;
    uint32_t packet_size = 0;
    uint64_t last_used = 0;
    int paired = 0;
    int custom = 0;
    int name_seen = 0;
    int address_seen = 0;
    int paired_seen = 0;
    int last_used_seen = 0;
    int width_seen = 0;
    int height_seen = 0;
    int custom_seen = 0;
    int fps_seen = 0;
    int bitrate_seen = 0;
    int packet_seen = 0;
    int codec_seen = 0;
    int aspect_seen = 0;
    int make_default = 0;
    int index;
    int i;

    for (i = 0; i < argc; ++i) {
        const char *value;

        if (strcmp(argv[i], "--name") == 0) {
            if (name_seen || require_value(argc, argv, &i, &name, error,
                                           error_size) < 0)
                goto duplicate;
            name_seen = 1;
        } else if (strcmp(argv[i], "--address") == 0) {
            if (address_seen || require_value(argc, argv, &i, &address, error,
                                              error_size) < 0)
                goto duplicate;
            address_seen = 1;
        } else if (strcmp(argv[i], "--paired") == 0) {
            if (paired_seen || require_value(argc, argv, &i, &value, error,
                                             error_size) < 0)
                goto duplicate;
            if (ns_parse_bool(value, &paired) < 0) {
                (void)snprintf(error, error_size,
                               "--paired must be true or false");
                return -1;
            }
            paired_seen = 1;
        } else if (strcmp(argv[i], "--last-used") == 0) {
            if (last_used_seen ||
                require_value(argc, argv, &i, &value, error, error_size) < 0)
                goto duplicate;
            if (parse_u64_cli(value, &last_used) < 0) {
                (void)snprintf(error, error_size,
                               "--last-used must be an unsigned integer");
                return -1;
            }
            last_used_seen = 1;
        } else if (strcmp(argv[i], "--width") == 0) {
            if (width_seen || require_value(argc, argv, &i, &value, error,
                                            error_size) < 0)
                goto duplicate;
            if (parse_u32_cli(value, &width) < 0)
                goto bad_integer;
            width_seen = 1;
        } else if (strcmp(argv[i], "--height") == 0) {
            if (height_seen || require_value(argc, argv, &i, &value, error,
                                             error_size) < 0)
                goto duplicate;
            if (parse_u32_cli(value, &height) < 0)
                goto bad_integer;
            height_seen = 1;
        } else if (strcmp(argv[i], "--custom") == 0) {
            if (custom_seen || require_value(argc, argv, &i, &value, error,
                                             error_size) < 0)
                goto duplicate;
            if (ns_parse_bool(value, &custom) < 0) {
                (void)snprintf(error, error_size,
                               "--custom must be true or false");
                return -1;
            }
            custom_seen = 1;
        } else if (strcmp(argv[i], "--fps") == 0) {
            if (fps_seen || require_value(argc, argv, &i, &value, error,
                                          error_size) < 0)
                goto duplicate;
            if (parse_u32_cli(value, &fps) < 0)
                goto bad_integer;
            fps_seen = 1;
        } else if (strcmp(argv[i], "--bitrate") == 0) {
            if (bitrate_seen || require_value(argc, argv, &i, &value, error,
                                              error_size) < 0)
                goto duplicate;
            if (parse_u32_cli(value, &bitrate) < 0)
                goto bad_integer;
            bitrate_seen = 1;
        } else if (strcmp(argv[i], "--packet-size") == 0) {
            if (packet_seen || require_value(argc, argv, &i, &value, error,
                                             error_size) < 0)
                goto duplicate;
            if (parse_u32_cli(value, &packet_size) < 0)
                goto bad_integer;
            packet_seen = 1;
        } else if (strcmp(argv[i], "--codec") == 0) {
            if (codec_seen || require_value(argc, argv, &i, &codec_text, error,
                                            error_size) < 0)
                goto duplicate;
            codec_seen = 1;
        } else if (strcmp(argv[i], "--aspect") == 0) {
            if (aspect_seen || require_value(argc, argv, &i, &aspect_text,
                                             error, error_size) < 0)
                goto duplicate;
            aspect_seen = 1;
        } else if (strcmp(argv[i], "--make-default") == 0) {
            if (make_default)
                goto duplicate;
            make_default = 1;
        } else {
            (void)snprintf(error, error_size, "unknown host option");
            return -1;
        }
    }
    if (!name_seen) {
        (void)snprintf(error, error_size, "--name is required");
        return -1;
    }
    if (width_seen != height_seen) {
        (void)snprintf(error, error_size,
                       "--width and --height must be supplied together");
        return -1;
    }
    if (ns_hosts_load(store, &db, error, error_size) < 0)
        return -1;
    index = ns_host_find(&db, name);
    if (index >= 0) {
        host = db.hosts[index];
    } else {
        ns_host_defaults(&host);
        if (!address_seen) {
            (void)snprintf(error, error_size,
                           "--address is required for a new host");
            return -1;
        }
        if (copy_text(host.name, sizeof(host.name), name, "host name", error,
                      error_size) < 0)
            return -1;
    }
    if (address_seen &&
        copy_text(host.address, sizeof(host.address), address, "address", error,
                  error_size) < 0)
        return -1;
    if (paired_seen)
        host.paired = paired;
    if (last_used_seen)
        host.last_used = last_used;
    if (width_seen) {
        host.resolution.width = width;
        host.resolution.height = height;
    }
    if (custom_seen)
        host.resolution.custom = custom;
    if (fps_seen)
        host.fps = fps;
    if (bitrate_seen)
        host.bitrate_kbps = bitrate;
    if (packet_seen)
        host.packet_size = packet_size;
    if (codec_seen && ns_parse_codec(codec_text, &host.codec) < 0) {
        (void)snprintf(error, error_size, "--codec must be H264, H265, or AV1");
        return -1;
    }
    if (aspect_seen && ns_parse_aspect(aspect_text, &host.aspect) < 0) {
        (void)snprintf(error, error_size,
                       "--aspect must be fit, fill, or stretch");
        return -1;
    }
    if (ns_host_upsert(&db, &host, error, error_size) < 0 ||
        (make_default && ns_host_select(&db, host.name, error, error_size) < 0) ||
        ns_hosts_save(store, &db, error, error_size) < 0)
        return -1;
    (void)puts("ok");
    return 0;

duplicate:
    (void)snprintf(error, error_size, "duplicate host option");
    return -1;
bad_integer:
    (void)snprintf(error, error_size,
                   "numeric host option must be an unsigned integer");
    return -1;
}

static int one_host_name(NsStore *store, int argc, char **argv, int delete_host,
                         char *error, size_t error_size)
{
    NsHostDb db;
    const char *name;

    if (argc != 2 || strcmp(argv[0], "--name") != 0) {
        (void)snprintf(error, error_size, "exactly one --name is required");
        return -1;
    }
    name = argv[1];
    if (ns_hosts_load(store, &db, error, error_size) < 0)
        return -1;
    if ((delete_host ? ns_host_delete(&db, name, error, error_size)
                     : ns_host_select(&db, name, error, error_size)) < 0 ||
        ns_hosts_save(store, &db, error, error_size) < 0)
        return -1;
    (void)puts("ok");
    return 0;
}

static int handle_host(NsStore *store, int argc, char **argv, char *error,
                       size_t error_size)
{
    if (argc < 1) {
        (void)snprintf(error, error_size, "missing host action");
        return -1;
    }
    if (strcmp(argv[0], "list") == 0 && argc == 1)
        return host_list(store, error, error_size);
    if (strcmp(argv[0], "set") == 0)
        return host_set(store, argc - 1, argv + 1, error, error_size);
    if (strcmp(argv[0], "delete") == 0)
        return one_host_name(store, argc - 1, argv + 1, 1, error, error_size);
    if (strcmp(argv[0], "select") == 0 || strcmp(argv[0], "default") == 0)
        return one_host_name(store, argc - 1, argv + 1, 0, error, error_size);
    (void)snprintf(error, error_size, "unknown host action");
    return -1;
}

int main(int argc, char **argv)
{
    const char *state_dir = DEFAULT_STATE_DIR;
    char error[NS_ERROR_MAX] = {0};
    NsStore store;
    int offset = 1;
    int result;

    if (argc > offset && strcmp(argv[offset], "--state-dir") == 0) {
        if (argc <= offset + 1) {
            usage(stderr);
            return 2;
        }
        state_dir = argv[offset + 1];
        offset += 2;
    }
    if (argc <= offset || strcmp(argv[offset], "--help") == 0 ||
        strcmp(argv[offset], "-h") == 0) {
        usage(argc <= offset ? stderr : stdout);
        return argc <= offset ? 2 : 0;
    }
    if (ns_store_open(&store, state_dir, error, sizeof(error)) < 0)
        return report(error);
    if (strcmp(argv[offset], "wifi") == 0)
        result = handle_wifi(&store, argc - offset - 1, argv + offset + 1,
                             error, sizeof(error));
    else if (strcmp(argv[offset], "host") == 0)
        result = handle_host(&store, argc - offset - 1, argv + offset + 1,
                             error, sizeof(error));
    else {
        (void)snprintf(error, sizeof(error), "domain must be wifi or host");
        result = -1;
    }
    ns_store_close(&store);
    return result < 0 ? report(error) : 0;
}
