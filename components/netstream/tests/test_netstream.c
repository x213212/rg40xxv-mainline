#define _GNU_SOURCE

#include "netstream.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static unsigned int checks;

#define CHECK(condition)                                                        \
    do {                                                                        \
        ++checks;                                                               \
        if (!(condition)) {                                                     \
            (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                          #condition);                                           \
            return -1;                                                          \
        }                                                                       \
    } while (0)

static int write_all(int fd, const char *data, size_t length)
{
    size_t offset = 0;

    while (offset < length) {
        ssize_t result = write(fd, data + offset, length - offset);

        if (result < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        offset += (size_t)result;
    }
    return 0;
}

static int write_named(const char *directory, const char *name,
                       const char *contents, mode_t mode)
{
    char path[512];
    int fd;

    if (snprintf(path, sizeof(path), "%s/%s", directory, name) >=
        (int)sizeof(path))
        return -1;
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0)
        return -1;
    if (fchmod(fd, mode) < 0 || write_all(fd, contents, strlen(contents)) < 0) {
        close(fd);
        return -1;
    }
    return close(fd);
}

static int file_mode(const char *directory, const char *name, mode_t *mode)
{
    char path[512];
    struct stat st;

    if (snprintf(path, sizeof(path), "%s/%s", directory, name) >=
        (int)sizeof(path) ||
        stat(path, &st) < 0)
        return -1;
    *mode = st.st_mode & 0777;
    return 0;
}

static int no_temporary_files(const char *directory)
{
    DIR *dir = opendir(directory);
    struct dirent *entry;

    if (dir == NULL)
        return 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, ".tmp.") != NULL) {
            closedir(dir);
            return 0;
        }
    }
    closedir(dir);
    return 1;
}

static void cleanup_directory(const char *directory)
{
    DIR *dir = opendir(directory);
    struct dirent *entry;

    if (dir == NULL)
        return;
    while ((entry = readdir(dir)) != NULL) {
        char path[512];

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;
        if (snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name) <
            (int)sizeof(path))
            (void)unlink(path);
    }
    closedir(dir);
    (void)rmdir(directory);
}

static int test_host_defaults_and_validation(void)
{
    NsHost host;
    char error[NS_ERROR_MAX];
    static const char invalid_utf8[] = {'x', (char)0xc0, (char)0xaf, '\0'};

    ns_host_defaults(&host);
    CHECK(host.resolution.width == 640);
    CHECK(host.resolution.height == 480);
    CHECK(host.resolution.custom == 0);
    CHECK(host.fps == 60);
    CHECK(host.bitrate_kbps == 5000);
    CHECK(host.packet_size == 1392);
    CHECK(host.codec == NS_CODEC_H264);
    CHECK(host.aspect == NS_ASPECT_FIT);
    CHECK(snprintf(host.name, sizeof(host.name), "%s", "客廳電腦") > 0);
    CHECK(snprintf(host.address, sizeof(host.address), "%s",
                   "sunshine-box.local") > 0);
    CHECK(ns_validate_host(&host, error, sizeof(error)) == 0);

    CHECK(snprintf(host.address, sizeof(host.address), "%s", "192.168.50.12") >
          0);
    CHECK(ns_validate_host(&host, error, sizeof(error)) == 0);
    CHECK(snprintf(host.address, sizeof(host.address), "%s", "2001:db8::10") >
          0);
    CHECK(ns_validate_host(&host, error, sizeof(error)) == 0);

    CHECK(snprintf(host.address, sizeof(host.address), "%s", "https://host") >
          0);
    CHECK(ns_validate_host(&host, error, sizeof(error)) < 0);
    CHECK(snprintf(host.address, sizeof(host.address), "%s", "host;shutdown") >
          0);
    CHECK(ns_validate_host(&host, error, sizeof(error)) < 0);
    CHECK(snprintf(host.address, sizeof(host.address), "%s", "-bad.local") > 0);
    CHECK(ns_validate_host(&host, error, sizeof(error)) < 0);
    CHECK(snprintf(host.address, sizeof(host.address), "%s", "999.999.1.1") >
          0);
    CHECK(ns_validate_host(&host, error, sizeof(error)) < 0);
    CHECK(snprintf(host.address, sizeof(host.address), "%s", "host.local") > 0);

    host.resolution.width = 1024;
    host.resolution.height = 600;
    CHECK(ns_validate_host(&host, error, sizeof(error)) < 0);
    host.resolution.custom = 1;
    CHECK(ns_validate_host(&host, error, sizeof(error)) == 0);
    host.resolution.width = 159;
    CHECK(ns_validate_host(&host, error, sizeof(error)) < 0);
    host.resolution.width = 1024;
    host.fps = 241;
    CHECK(ns_validate_host(&host, error, sizeof(error)) < 0);
    host.fps = 60;
    host.bitrate_kbps = 499;
    CHECK(ns_validate_host(&host, error, sizeof(error)) < 0);
    host.bitrate_kbps = 5000;
    host.packet_size = 1501;
    CHECK(ns_validate_host(&host, error, sizeof(error)) < 0);
    host.packet_size = 1392;
    host.last_used = UINT64_C(4102444801);
    CHECK(ns_validate_host(&host, error, sizeof(error)) < 0);
    host.last_used = 0;
    host.codec = (NsCodec)99;
    CHECK(ns_validate_host(&host, error, sizeof(error)) < 0);

    CHECK(ns_validate_utf8_text("繁體中文名稱", 1, 96));
    CHECK(!ns_validate_utf8_text(invalid_utf8, 1, 96));
    CHECK(!ns_validate_utf8_text("line\nbreak", 1, 96));
    CHECK(!ns_validate_utf8_text("\342\200\256spoof", 1, 96));
    return 0;
}

static int test_host_crud_and_roundtrip(void)
{
    char directory[] = "/tmp/netstream-hosts-XXXXXX";
    char error[NS_ERROR_MAX];
    NsStore store;
    NsHostDb db = {0};
    NsHostDb loaded;
    NsHost first;
    NsHost second;
    mode_t mode;

    CHECK(mkdtemp(directory) != NULL);
    CHECK(ns_store_open(&store, directory, error, sizeof(error)) == 0);
    ns_host_defaults(&first);
    CHECK(snprintf(first.name, sizeof(first.name), "%s", "客廳 %% 主機") > 0);
    CHECK(snprintf(first.address, sizeof(first.address), "%s", "desktop.local") >
          0);
    first.paired = 1;
    first.last_used = UINT64_C(1777777777);
    CHECK(ns_host_upsert(&db, &first, error, sizeof(error)) == 0);

    ns_host_defaults(&second);
    CHECK(snprintf(second.name, sizeof(second.name), "%s", "書房電腦") > 0);
    CHECK(snprintf(second.address, sizeof(second.address), "%s", "2001:db8::2") >
          0);
    second.resolution.width = 1024;
    second.resolution.height = 600;
    second.resolution.custom = 1;
    second.fps = 120;
    second.bitrate_kbps = 42000;
    second.packet_size = 1200;
    second.codec = NS_CODEC_H265;
    second.aspect = NS_ASPECT_FILL;
    CHECK(ns_host_upsert(&db, &second, error, sizeof(error)) == 0);
    CHECK(ns_host_select(&db, second.name, error, sizeof(error)) == 0);
    CHECK(ns_hosts_save(&store, &db, error, sizeof(error)) == 0);
    CHECK(file_mode(directory, "hosts.v1", &mode) == 0 && mode == 0600);
    CHECK(no_temporary_files(directory));
    CHECK(ns_hosts_load(&store, &loaded, error, sizeof(error)) == 0);
    CHECK(loaded.count == 2);
    CHECK(strcmp(loaded.default_name, "書房電腦") == 0);
    CHECK(strcmp(loaded.hosts[0].name, first.name) == 0);
    CHECK(strcmp(loaded.hosts[0].address, first.address) == 0);
    CHECK(loaded.hosts[0].paired == 1);
    CHECK(loaded.hosts[1].resolution.width == 1024);
    CHECK(loaded.hosts[1].resolution.height == 600);
    CHECK(loaded.hosts[1].resolution.custom == 1);
    CHECK(loaded.hosts[1].fps == 120);
    CHECK(loaded.hosts[1].bitrate_kbps == 42000);
    CHECK(loaded.hosts[1].packet_size == 1200);
    CHECK(loaded.hosts[1].codec == NS_CODEC_H265);
    CHECK(loaded.hosts[1].aspect == NS_ASPECT_FILL);

    loaded.hosts[0].fps = 30;
    CHECK(ns_host_upsert(&loaded, &loaded.hosts[0], error, sizeof(error)) == 0);
    CHECK(loaded.count == 2);
    CHECK(ns_host_delete(&loaded, "書房電腦", error, sizeof(error)) == 0);
    CHECK(loaded.count == 1);
    CHECK(loaded.default_name[0] == '\0');
    CHECK(ns_host_delete(&loaded, "missing", error, sizeof(error)) < 0);
    CHECK(loaded.count == 1);
    CHECK(ns_hosts_save(&store, &loaded, error, sizeof(error)) == 0);
    ns_store_close(&store);
    cleanup_directory(directory);
    return 0;
}

static int test_wifi_crud_and_vendor_import(void)
{
    char directory[] = "/tmp/netstream-wifi-XXXXXX";
    char vendor_path[512];
    char error[NS_ERROR_MAX];
    NsStore store;
    NsWifiDb db = {0};
    NsWifiDb loaded;
    NsWifiProfile first = {0};
    NsWifiProfile second = {0};
    size_t imported;
    int insecure;
    mode_t mode;

    CHECK(mkdtemp(directory) != NULL);
    CHECK(ns_store_open(&store, directory, error, sizeof(error)) == 0);
    CHECK(snprintf(first.ssid, sizeof(first.ssid), "%s", "家用網路") > 0);
    first.security = NS_WIFI_WPA2_PSK;
    CHECK(snprintf(first.secret, sizeof(first.secret), "%s", "first-secret") >
          0);
    CHECK(ns_wifi_upsert(&db, &first, error, sizeof(error)) == 0);
    CHECK(snprintf(second.ssid, sizeof(second.ssid), "%s", "Guest") > 0);
    second.security = NS_WIFI_OPEN;
    CHECK(ns_wifi_upsert(&db, &second, error, sizeof(error)) == 0);
    CHECK(ns_wifi_select(&db, first.ssid, error, sizeof(error)) == 0);
    CHECK(ns_wifi_save(&store, &db, error, sizeof(error)) == 0);
    CHECK(file_mode(directory, "wifi.v1", &mode) == 0 && mode == 0600);
    CHECK(ns_wifi_load(&store, &loaded, error, sizeof(error)) == 0);
    CHECK(loaded.count == 2);
    CHECK(strcmp(loaded.profiles[0].secret, "first-secret") == 0);
    CHECK(ns_wifi_upsert(&loaded, &loaded.profiles[0], error, sizeof(error)) ==
          0);
    CHECK(strcmp(loaded.profiles[0].secret, "first-secret") == 0);
    CHECK(ns_wifi_forget(&loaded, "Guest", error, sizeof(error)) == 0);
    CHECK(loaded.count == 1);
    CHECK(strcmp(loaded.profiles[0].ssid, "家用網路") == 0);
    CHECK(ns_wifi_forget(&loaded, "Guest", error, sizeof(error)) < 0);
    CHECK(loaded.count == 1);

    CHECK(snprintf(vendor_path, sizeof(vendor_path), "%s/vendor.wifi",
                   directory) < (int)sizeof(vendor_path));
    CHECK(write_named(directory, "vendor.wifi",
                      "S:廚房WiFi\tP:vendor-secret\n"
                      "S:OpenGuest\tP:\n",
                      0644) == 0);
    CHECK(ns_wifi_import_vendor(&loaded, vendor_path, 1, &imported, &insecure,
                                error, sizeof(error)) == 0);
    CHECK(imported == 2);
    CHECK(insecure == 1);
    CHECK(loaded.count == 3);
    CHECK(strcmp(loaded.default_ssid, loaded.profiles[1].ssid) == 0);
    CHECK(strcmp(loaded.profiles[1].secret, "vendor-secret") == 0);
    CHECK(loaded.profiles[2].security == NS_WIFI_OPEN);
    CHECK(ns_wifi_save(&store, &loaded, error, sizeof(error)) == 0);
    ns_secure_zero(&db, sizeof(db));
    ns_secure_zero(&loaded, sizeof(loaded));
    ns_store_close(&store);
    cleanup_directory(directory);
    return 0;
}

static int test_malformed_hosts(void)
{
    static const char *fixtures[] = {
        "BAD\t1\nD\t\n",
        "RG40XXV_NETSTREAM_HOSTS\t1\nD\t",
        "RG40XXV_NETSTREAM_HOSTS\t1\n",
        "RG40XXV_NETSTREAM_HOSTS\t1\nD\t\nD\t\n",
        "RG40XXV_NETSTREAM_HOSTS\t1\nH\tDesk\t10.0.0.1\t0\t0\t640\t480\t0\t60\t5000\t1392\tH264\tfit\nD\t\n",
        "RG40XXV_NETSTREAM_HOSTS\t1\nD\tMissing\n",
        "RG40XXV_NETSTREAM_HOSTS\t1\nD\t\nH\tBad%ZZ\t10.0.0.1\t0\t0\t640\t480\t0\t60\t5000\t1392\tH264\tfit\n",
        "RG40XXV_NETSTREAM_HOSTS\t1\nD\t\nH\t%C0%AF\t10.0.0.1\t0\t0\t640\t480\t0\t60\t5000\t1392\tH264\tfit\n",
        "RG40XXV_NETSTREAM_HOSTS\t1\nD\t\nH\tDesk\thttps%3A%2F%2Fhost\t0\t0\t640\t480\t0\t60\t5000\t1392\tH264\tfit\n",
        "RG40XXV_NETSTREAM_HOSTS\t1\nD\t\nH\tDesk\t10.0.0.1\tmaybe\t0\t640\t480\t0\t60\t5000\t1392\tH264\tfit\n",
        "RG40XXV_NETSTREAM_HOSTS\t1\nD\t\nH\tDesk\t10.0.0.1\t0\t99999999999999999999\t640\t480\t0\t60\t5000\t1392\tH264\tfit\n",
        "RG40XXV_NETSTREAM_HOSTS\t1\nD\t\nH\tDesk\t10.0.0.1\t0\t0\t1024\t600\t0\t60\t5000\t1392\tH264\tfit\n",
        "RG40XXV_NETSTREAM_HOSTS\t1\nD\t\nH\tDesk\t10.0.0.1\t0\t0\t640\t480\t0\t0\t5000\t1392\tH264\tfit\n",
        "RG40XXV_NETSTREAM_HOSTS\t1\nD\t\nH\tDesk\t10.0.0.1\t0\t0\t640\t480\t0\t60\t499\t1392\tH264\tfit\n",
        "RG40XXV_NETSTREAM_HOSTS\t1\nD\t\nH\tDesk\t10.0.0.1\t0\t0\t640\t480\t0\t60\t5000\t1501\tH264\tfit\n",
        "RG40XXV_NETSTREAM_HOSTS\t1\nD\t\nH\tDesk\t10.0.0.1\t0\t0\t640\t480\t0\t60\t5000\t1392\tVP9\tfit\n",
        "RG40XXV_NETSTREAM_HOSTS\t1\nD\t\nH\tDesk\t10.0.0.1\t0\t0\t640\t480\t0\t60\t5000\t1392\tH264\tcrop\n",
        "RG40XXV_NETSTREAM_HOSTS\t1\nD\t\nH\tDesk\t10.0.0.1\t0\t0\t640\t480\t0\t60\t5000\t1392\tH264\tfit\textra\n",
        "RG40XXV_NETSTREAM_HOSTS\t1\nD\t\nH\tDesk\t10.0.0.1\t0\t0\t640\t480\t0\t60\t5000\t1392\tH264\tfit\nH\tDesk\t10.0.0.2\t0\t0\t640\t480\t0\t60\t5000\t1392\tH264\tfit\n",
    };
    char directory[] = "/tmp/netstream-malformed-host-XXXXXX";
    char error[NS_ERROR_MAX];
    NsStore store;
    NsHostDb db;
    size_t i;

    CHECK(mkdtemp(directory) != NULL);
    CHECK(ns_store_open(&store, directory, error, sizeof(error)) == 0);
    for (i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); ++i) {
        CHECK(write_named(directory, "hosts.v1", fixtures[i], 0600) == 0);
        CHECK(ns_hosts_load(&store, &db, error, sizeof(error)) < 0);
        CHECK(db.count == 0);
    }
    ns_store_close(&store);
    cleanup_directory(directory);
    return 0;
}

static int test_malformed_wifi_and_symlinks(void)
{
    static const char *fixtures[] = {
        "BAD\t1\nD\t\n",
        "RG40XXV_NETSTREAM_WIFI\t1\nD\t",
        "RG40XXV_NETSTREAM_WIFI\t1\nW\tHome\twpa2-psk\t0\t0\tpassword1\n",
        "RG40XXV_NETSTREAM_WIFI\t1\nW\tHome\topen\t0\t0\t\nD\t\n",
        "RG40XXV_NETSTREAM_WIFI\t1\nD\tMissing\nW\tHome\topen\t0\t0\t\n",
        "RG40XXV_NETSTREAM_WIFI\t1\nD\t\nW\tBad%ZZ\topen\t0\t0\t\n",
        "RG40XXV_NETSTREAM_WIFI\t1\nD\t\nW\tHome\twpa2-psk\t0\t0\tshort\n",
        "RG40XXV_NETSTREAM_WIFI\t1\nD\t\nW\tHome\topen\t0\t0\tleaked-secret\n",
        "RG40XXV_NETSTREAM_WIFI\t1\nD\t\nW\tHome\twep\t0\t0\tpassword1\n",
        "RG40XXV_NETSTREAM_WIFI\t1\nD\t\nW\tHome\topen\t2\t0\t\n",
        "RG40XXV_NETSTREAM_WIFI\t1\nD\t\nW\tHome\topen\t0\t1000\t\n",
        "RG40XXV_NETSTREAM_WIFI\t1\nD\t\nW\tHome\topen\t0\t0\t\nW\tHome\topen\t0\t0\t\n",
    };
    char directory[] = "/tmp/netstream-malformed-wifi-XXXXXX";
    char target[512];
    char error[NS_ERROR_MAX];
    NsStore store;
    NsWifiDb db;
    size_t i;

    CHECK(mkdtemp(directory) != NULL);
    CHECK(ns_store_open(&store, directory, error, sizeof(error)) == 0);
    for (i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); ++i) {
        CHECK(write_named(directory, "wifi.v1", fixtures[i], 0600) == 0);
        CHECK(ns_wifi_load(&store, &db, error, sizeof(error)) < 0);
        CHECK(db.count == 0);
    }
    CHECK(snprintf(target, sizeof(target), "%s/wifi.v1", directory) <
          (int)sizeof(target));
    CHECK(unlink(target) == 0);
    CHECK(symlink("/etc/passwd", target) == 0);
    CHECK(ns_wifi_load(&store, &db, error, sizeof(error)) < 0);
    ns_store_close(&store);
    cleanup_directory(directory);
    return 0;
}

static int test_state_directory_security(void)
{
    char base[] = "/tmp/netstream-dir-XXXXXX";
    char real_path[512];
    char link_path[512];
    char traversal[512];
    char error[NS_ERROR_MAX];
    NsStore store;

    CHECK(mkdtemp(base) != NULL);
    CHECK(snprintf(real_path, sizeof(real_path), "%s/real", base) <
          (int)sizeof(real_path));
    CHECK(snprintf(link_path, sizeof(link_path), "%s/link", base) <
          (int)sizeof(link_path));
    CHECK(mkdir(real_path, 0700) == 0);
    CHECK(symlink(real_path, link_path) == 0);
    CHECK(ns_store_open(&store, link_path, error, sizeof(error)) < 0);
    CHECK(snprintf(traversal, sizeof(traversal), "%s/real/../other", base) <
          (int)sizeof(traversal));
    CHECK(ns_store_open(&store, traversal, error, sizeof(error)) < 0);
    CHECK(unlink(link_path) == 0);
    CHECK(rmdir(real_path) == 0);
    CHECK(rmdir(base) == 0);
    return 0;
}

int main(void)
{
    if (test_host_defaults_and_validation() < 0 ||
        test_host_crud_and_roundtrip() < 0 ||
        test_wifi_crud_and_vendor_import() < 0 ||
        test_malformed_hosts() < 0 ||
        test_malformed_wifi_and_symlinks() < 0 ||
        test_state_directory_security() < 0)
        return 1;
    (void)printf("PASS: %u checks\n", checks);
    return 0;
}
