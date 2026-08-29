#define _GNU_SOURCE

#include "netstream.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define NS_WIFI_FILE "wifi.v1"
#define NS_HOSTS_FILE "hosts.v1"
#define NS_LOCK_FILE ".lock"
#define NS_DB_MAX_BYTES (256U * 1024U)
#define NS_VENDOR_MAX_BYTES (64U * 1024U)

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} NsBuffer;

static int set_error(char *error, size_t size, const char *format, ...)
{
    va_list args;

    if (error != NULL && size > 0) {
        va_start(args, format);
        (void)vsnprintf(error, size, format, args);
        va_end(args);
    }
    return -1;
}

void ns_secure_zero(void *ptr, size_t len)
{
    volatile unsigned char *p = ptr;

    while (len-- > 0)
        *p++ = 0;
}

static int utf8_next(const unsigned char **cursor, const unsigned char *end,
                     uint32_t *codepoint)
{
    const unsigned char *p = *cursor;
    uint32_t cp;
    size_t need;

    if (p >= end)
        return -1;
    if (*p < 0x80) {
        cp = *p++;
    } else if (*p >= 0xc2 && *p <= 0xdf) {
        cp = (uint32_t)(*p++ & 0x1f);
        need = 1;
        if ((size_t)(end - p) < need)
            return -1;
        if ((p[0] & 0xc0) != 0x80)
            return -1;
        cp = (cp << 6) | (uint32_t)(*p++ & 0x3f);
    } else if (*p >= 0xe0 && *p <= 0xef) {
        unsigned char first = *p++;

        need = 2;
        if ((size_t)(end - p) < need || (p[0] & 0xc0) != 0x80 ||
            (p[1] & 0xc0) != 0x80)
            return -1;
        if ((first == 0xe0 && p[0] < 0xa0) ||
            (first == 0xed && p[0] >= 0xa0))
            return -1;
        cp = (uint32_t)(first & 0x0f);
        cp = (cp << 6) | (uint32_t)(*p++ & 0x3f);
        cp = (cp << 6) | (uint32_t)(*p++ & 0x3f);
    } else if (*p >= 0xf0 && *p <= 0xf4) {
        unsigned char first = *p++;

        need = 3;
        if ((size_t)(end - p) < need || (p[0] & 0xc0) != 0x80 ||
            (p[1] & 0xc0) != 0x80 || (p[2] & 0xc0) != 0x80)
            return -1;
        if ((first == 0xf0 && p[0] < 0x90) ||
            (first == 0xf4 && p[0] >= 0x90))
            return -1;
        cp = (uint32_t)(first & 0x07);
        cp = (cp << 6) | (uint32_t)(*p++ & 0x3f);
        cp = (cp << 6) | (uint32_t)(*p++ & 0x3f);
        cp = (cp << 6) | (uint32_t)(*p++ & 0x3f);
    } else {
        return -1;
    }

    *cursor = p;
    *codepoint = cp;
    return 0;
}

int ns_validate_utf8_text(const char *text, size_t min_bytes,
                          size_t max_bytes)
{
    size_t length;
    const unsigned char *p;
    const unsigned char *end;

    if (text == NULL)
        return 0;
    length = strnlen(text, max_bytes + 1);
    if (length < min_bytes || length > max_bytes)
        return 0;
    p = (const unsigned char *)text;
    end = p + length;
    while (p < end) {
        uint32_t cp;

        if (utf8_next(&p, end, &cp) < 0)
            return 0;
        if (cp <= 0x1f || (cp >= 0x7f && cp <= 0x9f) || cp == 0x2028 ||
            cp == 0x2029 || (cp >= 0x202a && cp <= 0x202e) ||
            (cp >= 0x2066 && cp <= 0x2069) ||
            (cp >= 0xfdd0 && cp <= 0xfdef) || (cp & 0xffffU) == 0xfffeU ||
            (cp & 0xffffU) == 0xffffU)
            return 0;
    }
    return 1;
}

const char *ns_wifi_security_name(NsWifiSecurity value)
{
    switch (value) {
    case NS_WIFI_OPEN:
        return "open";
    case NS_WIFI_WPA2_PSK:
        return "wpa2-psk";
    case NS_WIFI_WPA3_SAE:
        return "wpa3-sae";
    }
    return NULL;
}

const char *ns_codec_name(NsCodec value)
{
    switch (value) {
    case NS_CODEC_H264:
        return "H264";
    case NS_CODEC_H265:
        return "H265";
    case NS_CODEC_AV1:
        return "AV1";
    }
    return NULL;
}

const char *ns_aspect_name(NsAspect value)
{
    switch (value) {
    case NS_ASPECT_FIT:
        return "fit";
    case NS_ASPECT_FILL:
        return "fill";
    case NS_ASPECT_STRETCH:
        return "stretch";
    }
    return NULL;
}

int ns_parse_wifi_security(const char *text, NsWifiSecurity *out)
{
    if (out == NULL)
        return -1;
    if (text != NULL && strcmp(text, "open") == 0)
        *out = NS_WIFI_OPEN;
    else if (text != NULL && strcmp(text, "wpa2-psk") == 0)
        *out = NS_WIFI_WPA2_PSK;
    else if (text != NULL && strcmp(text, "wpa3-sae") == 0)
        *out = NS_WIFI_WPA3_SAE;
    else
        return -1;
    return 0;
}

int ns_parse_codec(const char *text, NsCodec *out)
{
    if (out == NULL)
        return -1;
    if (text != NULL && strcmp(text, "H264") == 0)
        *out = NS_CODEC_H264;
    else if (text != NULL && strcmp(text, "H265") == 0)
        *out = NS_CODEC_H265;
    else if (text != NULL && strcmp(text, "AV1") == 0)
        *out = NS_CODEC_AV1;
    else
        return -1;
    return 0;
}

int ns_parse_aspect(const char *text, NsAspect *out)
{
    if (out == NULL)
        return -1;
    if (text != NULL && strcmp(text, "fit") == 0)
        *out = NS_ASPECT_FIT;
    else if (text != NULL && strcmp(text, "fill") == 0)
        *out = NS_ASPECT_FILL;
    else if (text != NULL && strcmp(text, "stretch") == 0)
        *out = NS_ASPECT_STRETCH;
    else
        return -1;
    return 0;
}

int ns_parse_bool(const char *text, int *out)
{
    if (out == NULL)
        return -1;
    if (text != NULL && (strcmp(text, "true") == 0 || strcmp(text, "1") == 0))
        *out = 1;
    else if (text != NULL &&
             (strcmp(text, "false") == 0 || strcmp(text, "0") == 0))
        *out = 0;
    else
        return -1;
    return 0;
}

static int is_hex_string(const char *text, size_t length)
{
    size_t i;

    for (i = 0; i < length; ++i) {
        char c = text[i];

        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return 0;
    }
    return 1;
}

int ns_validate_wifi(const NsWifiProfile *profile, char *error,
                     size_t error_size)
{
    size_t secret_length;

    if (profile == NULL)
        return set_error(error, error_size, "missing Wi-Fi profile");
    if (!ns_validate_utf8_text(profile->ssid, 1, NS_SSID_MAX_BYTES))
        return set_error(error, error_size,
                         "SSID must be valid control-free UTF-8 (1..32 bytes)");
    if (profile->ssid[0] == ' ' ||
        profile->ssid[strlen(profile->ssid) - 1] == ' ')
        return set_error(error, error_size,
                         "SSID must not start or end with a space");
    if (ns_wifi_security_name(profile->security) == NULL)
        return set_error(error, error_size, "invalid Wi-Fi security mode");
    if (profile->hidden != 0 && profile->hidden != 1)
        return set_error(error, error_size, "hidden must be true or false");
    if (profile->priority < -999 || profile->priority > 999)
        return set_error(error, error_size,
                         "Wi-Fi priority must be between -999 and 999");
    secret_length = strnlen(profile->secret, NS_WIFI_SECRET_MAX_BYTES + 1);
    if (secret_length > NS_WIFI_SECRET_MAX_BYTES)
        return set_error(error, error_size, "Wi-Fi secret is too long");
    if (profile->security == NS_WIFI_OPEN) {
        if (secret_length != 0)
            return set_error(error, error_size,
                             "open Wi-Fi must not contain a secret");
        return 0;
    }
    if (profile->security == NS_WIFI_WPA2_PSK && secret_length == 64 &&
        is_hex_string(profile->secret, secret_length))
        return 0;
    if (secret_length < 8 || secret_length > 63 ||
        !ns_validate_utf8_text(profile->secret, 8, 63))
        return set_error(error, error_size,
                         "Wi-Fi passphrase must be 8..63 control-free UTF-8 bytes"
                         " (or 64 hex bytes for WPA2)");
    return 0;
}

void ns_host_defaults(NsHost *host)
{
    if (host == NULL)
        return;
    memset(host, 0, sizeof(*host));
    host->resolution.width = 640;
    host->resolution.height = 480;
    host->resolution.custom = 0;
    host->fps = 60;
    host->bitrate_kbps = 5000;
    host->packet_size = 1392;
    host->codec = NS_CODEC_H264;
    host->aspect = NS_ASPECT_FIT;
}

static int valid_dns_name(const char *address)
{
    size_t length = strlen(address);
    size_t label_length = 0;
    size_t i;

    if (length == 0 || length > NS_HOST_ADDRESS_MAX_BYTES ||
        address[0] == '.' || address[length - 1] == '.')
        return 0;
    for (i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)address[i];

        if (c == '.') {
            if (label_length == 0 || label_length > 63 ||
                address[i - 1] == '-')
                return 0;
            label_length = 0;
            continue;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-'))
            return 0;
        if (label_length == 0 && c == '-')
            return 0;
        ++label_length;
    }
    return label_length > 0 && label_length <= 63 && address[length - 1] != '-';
}

static int valid_address(const char *address)
{
    unsigned char parsed[sizeof(struct in6_addr)];
    const unsigned char *p;
    int numeric_dotted = 1;

    if (address == NULL ||
        strnlen(address, NS_HOST_ADDRESS_MAX_BYTES + 1) >
            NS_HOST_ADDRESS_MAX_BYTES)
        return 0;
    if (inet_pton(AF_INET, address, parsed) == 1 ||
        inet_pton(AF_INET6, address, parsed) == 1)
        return 1;
    for (p = (const unsigned char *)address; *p != '\0'; ++p) {
        if (!((*p >= '0' && *p <= '9') || *p == '.')) {
            numeric_dotted = 0;
            break;
        }
    }
    if (numeric_dotted)
        return 0;
    return valid_dns_name(address);
}

static int standard_resolution(uint32_t width, uint32_t height)
{
    static const uint32_t presets[][2] = {
        {640, 480}, {1280, 720}, {1920, 1080},
        {2560, 1440}, {3840, 2160},
    };
    size_t i;

    for (i = 0; i < sizeof(presets) / sizeof(presets[0]); ++i) {
        if (presets[i][0] == width && presets[i][1] == height)
            return 1;
    }
    return 0;
}

int ns_validate_host(const NsHost *host, char *error, size_t error_size)
{
    size_t name_length;

    if (host == NULL)
        return set_error(error, error_size, "missing host");
    if (!ns_validate_utf8_text(host->name, 1, NS_HOST_NAME_MAX_BYTES))
        return set_error(error, error_size,
                         "host name must be valid control-free UTF-8 (1..96 bytes)");
    name_length = strlen(host->name);
    if (host->name[0] == ' ' || host->name[name_length - 1] == ' ')
        return set_error(error, error_size,
                         "host name must not start or end with a space");
    if (!valid_address(host->address))
        return set_error(error, error_size,
                         "address must be an IPv4, IPv6, or ASCII DNS name");
    if (host->paired != 0 && host->paired != 1)
        return set_error(error, error_size, "paired must be true or false");
    if (host->last_used > UINT64_C(4102444800))
        return set_error(error, error_size,
                         "last_used must be a Unix timestamp no later than 2100");
    if (host->resolution.custom != 0 && host->resolution.custom != 1)
        return set_error(error, error_size,
                         "resolution.custom must be true or false");
    if (host->resolution.width < 160 || host->resolution.width > 7680 ||
        host->resolution.height < 120 || host->resolution.height > 4320)
        return set_error(error, error_size,
                         "resolution must be within 160x120 and 7680x4320");
    if (!host->resolution.custom &&
        !standard_resolution(host->resolution.width, host->resolution.height))
        return set_error(error, error_size,
                         "non-preset resolution requires custom=true");
    if (host->fps < 1 || host->fps > 240)
        return set_error(error, error_size, "fps must be between 1 and 240");
    if (host->bitrate_kbps < 500 || host->bitrate_kbps > 200000)
        return set_error(error, error_size,
                         "bitrate must be between 500 and 200000 kbps");
    if (host->packet_size < 256 || host->packet_size > 1500)
        return set_error(error, error_size,
                         "packet_size must be between 256 and 1500 bytes");
    if (ns_codec_name(host->codec) == NULL)
        return set_error(error, error_size, "codec must be H264, H265, or AV1");
    if (ns_aspect_name(host->aspect) == NULL)
        return set_error(error, error_size,
                         "aspect must be fit, fill, or stretch");
    return 0;
}

static int open_state_directory(const char *path, char *error,
                                size_t error_size)
{
    const char *scan;
    char *copy = NULL;
    char *cursor;
    char *component;
    int dir_fd = -1;

    if (path == NULL || path[0] != '/' || path[1] == '\0')
        return set_error(error, error_size,
                         "state directory must be a non-root absolute path");
    scan = path + 1;
    while (*scan != '\0') {
        const char *start;
        size_t component_length;

        while (*scan == '/')
            ++scan;
        if (*scan == '\0')
            break;
        start = scan;
        while (*scan != '\0' && *scan != '/')
            ++scan;
        component_length = (size_t)(scan - start);
        if ((component_length == 1 && start[0] == '.') ||
            (component_length == 2 && start[0] == '.' && start[1] == '.'))
            return set_error(error, error_size,
                             "state directory must not contain . or ..");
    }
    copy = strdup(path);
    if (copy == NULL)
        return set_error(error, error_size, "out of memory");
    dir_fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd < 0) {
        free(copy);
        return set_error(error, error_size, "cannot open filesystem root: %s",
                         strerror(errno));
    }

    cursor = copy + 1;
    while ((component = strsep(&cursor, "/")) != NULL) {
        int next_fd;

        if (*component == '\0')
            continue;
        if (strcmp(component, ".") == 0 || strcmp(component, "..") == 0) {
            close(dir_fd);
            free(copy);
            return set_error(error, error_size,
                             "state directory must not contain . or ..");
        }
        if (mkdirat(dir_fd, component, 0700) < 0 && errno != EEXIST) {
            int saved = errno;

            close(dir_fd);
            free(copy);
            return set_error(error, error_size,
                             "cannot create state directory: %s",
                             strerror(saved));
        }
        next_fd = openat(dir_fd, component,
                         O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next_fd < 0) {
            int saved = errno;

            close(dir_fd);
            free(copy);
            return set_error(error, error_size,
                             "cannot securely open state directory: %s",
                             strerror(saved));
        }
        close(dir_fd);
        dir_fd = next_fd;
    }
    free(copy);

    {
        struct stat st;

        if (fstat(dir_fd, &st) < 0) {
            int saved = errno;

            close(dir_fd);
            return set_error(error, error_size,
                             "cannot inspect state directory: %s",
                             strerror(saved));
        }
        if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
            (st.st_mode & 07777) != 0700) {
            close(dir_fd);
            return set_error(error, error_size,
                             "state directory must be owned by this user and mode 0700");
        }
    }
    return dir_fd;
}

int ns_store_open(NsStore *store, const char *state_dir, char *error,
                  size_t error_size)
{
    struct stat st;

    if (store == NULL)
        return set_error(error, error_size, "missing store");
    store->dir_fd = -1;
    store->lock_fd = -1;
    store->dir_fd = open_state_directory(state_dir, error, error_size);
    if (store->dir_fd < 0)
        return -1;
    store->lock_fd = openat(store->dir_fd, NS_LOCK_FILE,
                            O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (store->lock_fd < 0)
        goto lock_error;
    if (fstat(store->lock_fd, &st) < 0 || !S_ISREG(st.st_mode) ||
        st.st_uid != geteuid() || st.st_nlink != 1)
        goto unsafe_lock;
    if (fchmod(store->lock_fd, 0600) < 0)
        goto lock_error;
    if (flock(store->lock_fd, LOCK_EX) < 0)
        goto lock_error;
    return 0;

unsafe_lock:
    (void)set_error(error, error_size, "unsafe state lock file");
    ns_store_close(store);
    return -1;
lock_error:
    (void)set_error(error, error_size, "cannot lock state directory: %s",
                    strerror(errno));
    ns_store_close(store);
    return -1;
}

void ns_store_close(NsStore *store)
{
    if (store == NULL)
        return;
    if (store->lock_fd >= 0) {
        (void)flock(store->lock_fd, LOCK_UN);
        close(store->lock_fd);
    }
    if (store->dir_fd >= 0)
        close(store->dir_fd);
    store->lock_fd = -1;
    store->dir_fd = -1;
}

static int read_all_fd(int fd, char *buffer, size_t length)
{
    size_t offset = 0;

    while (offset < length) {
        ssize_t got = read(fd, buffer + offset, length - offset);

        if (got < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (got == 0)
            return -1;
        offset += (size_t)got;
    }
    return 0;
}

static int read_database(NsStore *store, const char *name, char **data,
                         size_t *length, int *missing, char *error,
                         size_t error_size)
{
    struct stat st;
    int fd;
    char *buffer;

    *data = NULL;
    *length = 0;
    *missing = 0;
    fd = openat(store->dir_fd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT) {
            *missing = 1;
            return 0;
        }
        return set_error(error, error_size, "cannot open %s: %s", name,
                         strerror(errno));
    }
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) ||
        st.st_uid != geteuid() || st.st_nlink != 1) {
        close(fd);
        return set_error(error, error_size, "%s is not a safe regular file",
                         name);
    }
    if (st.st_size <= 0 || (uintmax_t)st.st_size > NS_DB_MAX_BYTES) {
        close(fd);
        return set_error(error, error_size, "%s has an invalid size", name);
    }
    if (fchmod(fd, 0600) < 0) {
        int saved = errno;

        close(fd);
        return set_error(error, error_size, "cannot secure %s: %s", name,
                         strerror(saved));
    }
    buffer = malloc((size_t)st.st_size + 1);
    if (buffer == NULL) {
        close(fd);
        return set_error(error, error_size, "out of memory");
    }
    if (read_all_fd(fd, buffer, (size_t)st.st_size) < 0) {
        int saved = errno;

        close(fd);
        free(buffer);
        return set_error(error, error_size, "cannot read %s: %s", name,
                         strerror(saved));
    }
    close(fd);
    buffer[st.st_size] = '\0';
    *data = buffer;
    *length = (size_t)st.st_size;
    return 0;
}

static int write_all_fd(int fd, const char *data, size_t length)
{
    size_t offset = 0;

    while (offset < length) {
        ssize_t written = write(fd, data + offset, length - offset);

        if (written < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        offset += (size_t)written;
    }
    return 0;
}

static int atomic_write(NsStore *store, const char *name, const char *data,
                        size_t length, char *error, size_t error_size)
{
    static unsigned long serial;
    char temporary[96];
    int fd = -1;
    unsigned int attempt;

    for (attempt = 0; attempt < 100; ++attempt) {
        ++serial;
        (void)snprintf(temporary, sizeof(temporary), ".%s.tmp.%ld.%lu", name,
                       (long)getpid(), serial);
        fd = openat(store->dir_fd, temporary,
                    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                    0600);
        if (fd >= 0)
            break;
        if (errno != EEXIST)
            return set_error(error, error_size,
                             "cannot create temporary state file: %s",
                             strerror(errno));
    }
    if (fd < 0)
        return set_error(error, error_size,
                         "cannot allocate a temporary state file");
    if (fchmod(fd, 0600) < 0 || write_all_fd(fd, data, length) < 0 ||
        fsync(fd) < 0) {
        int saved = errno;

        close(fd);
        (void)unlinkat(store->dir_fd, temporary, 0);
        return set_error(error, error_size, "cannot persist state: %s",
                         strerror(saved));
    }
    if (close(fd) < 0) {
        int saved = errno;

        (void)unlinkat(store->dir_fd, temporary, 0);
        return set_error(error, error_size, "cannot close state file: %s",
                         strerror(saved));
    }
    if (renameat(store->dir_fd, temporary, store->dir_fd, name) < 0) {
        int saved = errno;

        (void)unlinkat(store->dir_fd, temporary, 0);
        return set_error(error, error_size, "cannot publish state: %s",
                         strerror(saved));
    }
    if (fsync(store->dir_fd) < 0)
        return set_error(error, error_size,
                         "state was renamed but directory fsync failed: %s",
                         strerror(errno));
    return 0;
}

static int buffer_reserve(NsBuffer *buffer, size_t extra)
{
    size_t wanted;
    size_t capacity;
    char *new_data;

    if (extra > NS_DB_MAX_BYTES || buffer->length > NS_DB_MAX_BYTES - extra)
        return -1;
    wanted = buffer->length + extra + 1;
    if (wanted <= buffer->capacity)
        return 0;
    capacity = buffer->capacity == 0 ? 1024 : buffer->capacity;
    while (capacity < wanted) {
        if (capacity > NS_DB_MAX_BYTES / 2) {
            capacity = NS_DB_MAX_BYTES + 1;
            break;
        }
        capacity *= 2;
    }
    new_data = realloc(buffer->data, capacity);
    if (new_data == NULL)
        return -1;
    buffer->data = new_data;
    buffer->capacity = capacity;
    return 0;
}

static int buffer_append(NsBuffer *buffer, const char *text, size_t length)
{
    if (buffer_reserve(buffer, length) < 0)
        return -1;
    memcpy(buffer->data + buffer->length, text, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return 0;
}

static int buffer_printf(NsBuffer *buffer, const char *format, ...)
{
    va_list args;
    va_list copy;
    int needed;

    va_start(args, format);
    va_copy(copy, args);
    needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (needed < 0 || buffer_reserve(buffer, (size_t)needed) < 0) {
        va_end(args);
        return -1;
    }
    (void)vsnprintf(buffer->data + buffer->length,
                    buffer->capacity - buffer->length, format, args);
    va_end(args);
    buffer->length += (size_t)needed;
    return 0;
}

static int unreserved(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
           c == '~';
}

static int buffer_encoded(NsBuffer *buffer, const char *text)
{
    static const char hex[] = "0123456789ABCDEF";
    const unsigned char *p = (const unsigned char *)text;

    while (*p != '\0') {
        if (unreserved(*p)) {
            if (buffer_append(buffer, (const char *)p, 1) < 0)
                return -1;
        } else {
            char escaped[3] = {'%', hex[*p >> 4], hex[*p & 0x0f]};

            if (buffer_append(buffer, escaped, sizeof(escaped)) < 0)
                return -1;
        }
        ++p;
    }
    return 0;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int percent_decode(const char *encoded, char *decoded, size_t capacity)
{
    size_t out = 0;
    size_t i;

    for (i = 0; encoded[i] != '\0'; ++i) {
        unsigned char value;

        if (unreserved((unsigned char)encoded[i])) {
            value = (unsigned char)encoded[i];
        } else if (encoded[i] == '%' && encoded[i + 1] != '\0' &&
                   encoded[i + 2] != '\0') {
            int high = hex_value(encoded[i + 1]);
            int low = hex_value(encoded[i + 2]);

            if (high < 0 || low < 0)
                return -1;
            value = (unsigned char)((high << 4) | low);
            if (value == 0)
                return -1;
            i += 2;
        } else {
            return -1;
        }
        if (out + 1 >= capacity)
            return -1;
        decoded[out++] = (char)value;
    }
    decoded[out] = '\0';
    return 0;
}

static int split_fields(char *line, char **fields, size_t expected)
{
    size_t count = 0;
    char *cursor = line;
    char *field;

    while ((field = strsep(&cursor, "\t")) != NULL) {
        if (count >= expected)
            return -1;
        fields[count++] = field;
    }
    return count == expected ? 0 : -1;
}

static int parse_u32(const char *text, uint32_t min, uint32_t max,
                     uint32_t *value)
{
    char *end;
    unsigned long parsed;

    if (text == NULL || *text == '\0' || *text == '+' || *text == '-')
        return -1;
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || *end != '\0' || parsed < min || parsed > max)
        return -1;
    *value = (uint32_t)parsed;
    return 0;
}

static int parse_u64(const char *text, uint64_t min, uint64_t max,
                     uint64_t *value)
{
    char *end;
    unsigned long long parsed;

    if (text == NULL || *text == '\0' || *text == '+' || *text == '-')
        return -1;
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0' || parsed < min || parsed > max)
        return -1;
    *value = (uint64_t)parsed;
    return 0;
}

static int parse_priority(const char *text, int *value)
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

int ns_wifi_find(const NsWifiDb *db, const char *ssid)
{
    size_t i;

    if (db == NULL || db->count > NS_MAX_WIFI_PROFILES || ssid == NULL ||
        strnlen(ssid, NS_SSID_MAX_BYTES + 1) > NS_SSID_MAX_BYTES)
        return -1;
    for (i = 0; i < db->count; ++i) {
        if (strcmp(db->profiles[i].ssid, ssid) == 0)
            return (int)i;
    }
    return -1;
}

int ns_wifi_upsert(NsWifiDb *db, const NsWifiProfile *profile, char *error,
                   size_t error_size)
{
    NsWifiProfile copy;
    int index;

    if (db == NULL || db->count > NS_MAX_WIFI_PROFILES)
        return set_error(error, error_size, "invalid Wi-Fi database");
    if (ns_validate_wifi(profile, error, error_size) < 0)
        return -1;
    copy = *profile;
    index = ns_wifi_find(db, copy.ssid);
    if (index >= 0) {
        ns_secure_zero(&db->profiles[index], sizeof(db->profiles[index]));
        db->profiles[index] = copy;
        ns_secure_zero(&copy, sizeof(copy));
        return 0;
    }
    if (db->count >= NS_MAX_WIFI_PROFILES) {
        ns_secure_zero(&copy, sizeof(copy));
        return set_error(error, error_size, "too many Wi-Fi profiles");
    }
    db->profiles[db->count++] = copy;
    ns_secure_zero(&copy, sizeof(copy));
    return 0;
}

int ns_wifi_forget(NsWifiDb *db, const char *ssid, char *error,
                   size_t error_size)
{
    int index;
    size_t i;

    if (db == NULL || !ns_validate_utf8_text(ssid, 1, NS_SSID_MAX_BYTES))
        return set_error(error, error_size, "invalid SSID");
    index = ns_wifi_find(db, ssid);
    if (index < 0)
        return set_error(error, error_size, "Wi-Fi profile was not found");
    if (strcmp(db->default_ssid, ssid) == 0)
        db->default_ssid[0] = '\0';
    ns_secure_zero(&db->profiles[index], sizeof(db->profiles[index]));
    for (i = (size_t)index; i + 1 < db->count; ++i)
        db->profiles[i] = db->profiles[i + 1];
    --db->count;
    ns_secure_zero(&db->profiles[db->count], sizeof(db->profiles[db->count]));
    return 0;
}

int ns_wifi_select(NsWifiDb *db, const char *ssid, char *error,
                   size_t error_size)
{
    size_t length;

    if (db == NULL || ns_wifi_find(db, ssid) < 0)
        return set_error(error, error_size, "Wi-Fi profile was not found");
    length = strlen(ssid);
    memmove(db->default_ssid, ssid, length + 1);
    return 0;
}

int ns_host_find(const NsHostDb *db, const char *name)
{
    size_t i;

    if (db == NULL || db->count > NS_MAX_HOSTS || name == NULL ||
        strnlen(name, NS_HOST_NAME_MAX_BYTES + 1) > NS_HOST_NAME_MAX_BYTES)
        return -1;
    for (i = 0; i < db->count; ++i) {
        if (strcmp(db->hosts[i].name, name) == 0)
            return (int)i;
    }
    return -1;
}

int ns_host_upsert(NsHostDb *db, const NsHost *host, char *error,
                   size_t error_size)
{
    int index;

    if (db == NULL || db->count > NS_MAX_HOSTS)
        return set_error(error, error_size, "invalid host database");
    if (ns_validate_host(host, error, error_size) < 0)
        return -1;
    index = ns_host_find(db, host->name);
    if (index >= 0) {
        db->hosts[index] = *host;
        return 0;
    }
    if (db->count >= NS_MAX_HOSTS)
        return set_error(error, error_size, "too many streaming hosts");
    db->hosts[db->count++] = *host;
    return 0;
}

int ns_host_delete(NsHostDb *db, const char *name, char *error,
                   size_t error_size)
{
    int index;
    size_t i;

    if (db == NULL || !ns_validate_utf8_text(name, 1, NS_HOST_NAME_MAX_BYTES))
        return set_error(error, error_size, "invalid host name");
    index = ns_host_find(db, name);
    if (index < 0)
        return set_error(error, error_size, "streaming host was not found");
    if (strcmp(db->default_name, name) == 0)
        db->default_name[0] = '\0';
    for (i = (size_t)index; i + 1 < db->count; ++i)
        db->hosts[i] = db->hosts[i + 1];
    --db->count;
    memset(&db->hosts[db->count], 0, sizeof(db->hosts[db->count]));
    return 0;
}

int ns_host_select(NsHostDb *db, const char *name, char *error,
                   size_t error_size)
{
    size_t length;

    if (db == NULL || ns_host_find(db, name) < 0)
        return set_error(error, error_size, "streaming host was not found");
    length = strlen(name);
    memmove(db->default_name, name, length + 1);
    return 0;
}

static int validate_wifi_db(const NsWifiDb *db, char *error,
                            size_t error_size)
{
    size_t i;
    size_t j;

    if (db->count > NS_MAX_WIFI_PROFILES)
        return set_error(error, error_size, "too many Wi-Fi profiles");
    if (strnlen(db->default_ssid, sizeof(db->default_ssid)) ==
        sizeof(db->default_ssid))
        return set_error(error, error_size,
                         "default Wi-Fi profile is not terminated");
    for (i = 0; i < db->count; ++i) {
        if (ns_validate_wifi(&db->profiles[i], error, error_size) < 0)
            return -1;
        for (j = 0; j < i; ++j) {
            if (strcmp(db->profiles[i].ssid, db->profiles[j].ssid) == 0)
                return set_error(error, error_size,
                                 "duplicate SSID in Wi-Fi database");
        }
    }
    if (db->default_ssid[0] != '\0' &&
        ns_wifi_find(db, db->default_ssid) < 0)
        return set_error(error, error_size,
                         "default Wi-Fi profile does not exist");
    return 0;
}

static int validate_host_db(const NsHostDb *db, char *error,
                            size_t error_size)
{
    size_t i;
    size_t j;

    if (db->count > NS_MAX_HOSTS)
        return set_error(error, error_size, "too many streaming hosts");
    if (strnlen(db->default_name, sizeof(db->default_name)) ==
        sizeof(db->default_name))
        return set_error(error, error_size,
                         "default streaming host is not terminated");
    for (i = 0; i < db->count; ++i) {
        if (ns_validate_host(&db->hosts[i], error, error_size) < 0)
            return -1;
        for (j = 0; j < i; ++j) {
            if (strcmp(db->hosts[i].name, db->hosts[j].name) == 0)
                return set_error(error, error_size,
                                 "duplicate name in host database");
        }
    }
    if (db->default_name[0] != '\0' &&
        ns_host_find(db, db->default_name) < 0)
        return set_error(error, error_size,
                         "default streaming host does not exist");
    return 0;
}

int ns_wifi_save(NsStore *store, const NsWifiDb *db, char *error,
                 size_t error_size)
{
    NsBuffer buffer = {0};
    size_t i;
    int result = -1;

    if (store == NULL || store->dir_fd < 0 || db == NULL)
        return set_error(error, error_size, "invalid Wi-Fi save request");
    if (validate_wifi_db(db, error, error_size) < 0)
        return -1;
    if (buffer_append(&buffer, "RG40XXV_NETSTREAM_WIFI\t1\nD\t",
                      sizeof("RG40XXV_NETSTREAM_WIFI\t1\nD\t") - 1) < 0 ||
        buffer_encoded(&buffer, db->default_ssid) < 0 ||
        buffer_append(&buffer, "\n", 1) < 0)
        goto memory_error;
    for (i = 0; i < db->count; ++i) {
        const NsWifiProfile *profile = &db->profiles[i];

        if (buffer_append(&buffer, "W\t", 2) < 0 ||
            buffer_encoded(&buffer, profile->ssid) < 0 ||
            buffer_printf(&buffer, "\t%s\t%d\t%d\t",
                          ns_wifi_security_name(profile->security),
                          profile->hidden, profile->priority) < 0 ||
            buffer_encoded(&buffer, profile->secret) < 0 ||
            buffer_append(&buffer, "\n", 1) < 0)
            goto memory_error;
    }
    result = atomic_write(store, NS_WIFI_FILE, buffer.data, buffer.length,
                          error, error_size);
    goto out;

memory_error:
    (void)set_error(error, error_size, "Wi-Fi database is too large");
out:
    if (buffer.data != NULL) {
        ns_secure_zero(buffer.data, buffer.capacity);
        free(buffer.data);
    }
    return result;
}

int ns_hosts_save(NsStore *store, const NsHostDb *db, char *error,
                  size_t error_size)
{
    NsBuffer buffer = {0};
    size_t i;
    int result = -1;

    if (store == NULL || store->dir_fd < 0 || db == NULL)
        return set_error(error, error_size, "invalid host save request");
    if (validate_host_db(db, error, error_size) < 0)
        return -1;
    if (buffer_append(&buffer, "RG40XXV_NETSTREAM_HOSTS\t1\nD\t",
                      sizeof("RG40XXV_NETSTREAM_HOSTS\t1\nD\t") - 1) < 0 ||
        buffer_encoded(&buffer, db->default_name) < 0 ||
        buffer_append(&buffer, "\n", 1) < 0)
        goto memory_error;
    for (i = 0; i < db->count; ++i) {
        const NsHost *host = &db->hosts[i];

        if (buffer_append(&buffer, "H\t", 2) < 0 ||
            buffer_encoded(&buffer, host->name) < 0 ||
            buffer_append(&buffer, "\t", 1) < 0 ||
            buffer_encoded(&buffer, host->address) < 0 ||
            buffer_printf(&buffer,
                          "\t%d\t%llu\t%u\t%u\t%d\t%u\t%u\t%u\t%s\t%s\n",
                          host->paired, (unsigned long long)host->last_used,
                          host->resolution.width, host->resolution.height,
                          host->resolution.custom, host->fps,
                          host->bitrate_kbps, host->packet_size,
                          ns_codec_name(host->codec),
                          ns_aspect_name(host->aspect)) < 0)
            goto memory_error;
    }
    result = atomic_write(store, NS_HOSTS_FILE, buffer.data, buffer.length,
                          error, error_size);
    goto out;

memory_error:
    (void)set_error(error, error_size, "host database is too large");
out:
    free(buffer.data);
    return result;
}

int ns_wifi_load(NsStore *store, NsWifiDb *db, char *error,
                 size_t error_size)
{
    char *data = NULL;
    size_t length = 0;
    size_t line_number = 0;
    char *cursor;
    char *end;
    int missing;
    int saw_default = 0;
    int result = -1;

    if (store == NULL || store->dir_fd < 0 || db == NULL)
        return set_error(error, error_size, "invalid Wi-Fi load request");
    memset(db, 0, sizeof(*db));
    if (read_database(store, NS_WIFI_FILE, &data, &length, &missing, error,
                      error_size) < 0)
        return -1;
    if (missing)
        return 0;
    if (data[length - 1] != '\n') {
        (void)set_error(error, error_size,
                        "Wi-Fi database is truncated (missing final newline)");
        goto out;
    }
    cursor = data;
    end = data + length;
    while (cursor < end) {
        char *newline = memchr(cursor, '\n', (size_t)(end - cursor));
        char *fields[6];

        ++line_number;
        *newline = '\0';
        if (line_number == 1) {
            if (strcmp(cursor, "RG40XXV_NETSTREAM_WIFI\t1") != 0) {
                (void)set_error(error, error_size,
                                "unsupported Wi-Fi database header");
                goto out;
            }
        } else if (line_number == 2 && strncmp(cursor, "D\t", 2) == 0) {
            if (saw_default || split_fields(cursor, fields, 2) < 0 ||
                strcmp(fields[0], "D") != 0 ||
                percent_decode(fields[1], db->default_ssid,
                               sizeof(db->default_ssid)) < 0) {
                (void)set_error(error, error_size,
                                "Wi-Fi database line %zu is malformed",
                                line_number);
                goto out;
            }
            saw_default = 1;
        } else if (line_number > 2 && saw_default &&
                   strncmp(cursor, "W\t", 2) == 0) {
            NsWifiProfile profile = {0};

            if (db->count >= NS_MAX_WIFI_PROFILES ||
                split_fields(cursor, fields, 6) < 0 ||
                strcmp(fields[0], "W") != 0 ||
                percent_decode(fields[1], profile.ssid,
                               sizeof(profile.ssid)) < 0 ||
                ns_parse_wifi_security(fields[2], &profile.security) < 0 ||
                ns_parse_bool(fields[3], &profile.hidden) < 0 ||
                parse_priority(fields[4], &profile.priority) < 0 ||
                percent_decode(fields[5], profile.secret,
                               sizeof(profile.secret)) < 0 ||
                ns_validate_wifi(&profile, NULL, 0) < 0 ||
                ns_wifi_find(db, profile.ssid) >= 0) {
                ns_secure_zero(&profile, sizeof(profile));
                (void)set_error(error, error_size,
                                "Wi-Fi database line %zu is malformed",
                                line_number);
                goto out;
            }
            db->profiles[db->count++] = profile;
            ns_secure_zero(&profile, sizeof(profile));
        } else {
            (void)set_error(error, error_size,
                            "Wi-Fi database line %zu is malformed",
                            line_number);
            goto out;
        }
        cursor = newline + 1;
    }
    if (!saw_default || validate_wifi_db(db, error, error_size) < 0)
        goto out;
    result = 0;

out:
    if (result < 0)
        ns_secure_zero(db, sizeof(*db));
    ns_secure_zero(data, length);
    free(data);
    return result;
}

int ns_hosts_load(NsStore *store, NsHostDb *db, char *error,
                  size_t error_size)
{
    char *data = NULL;
    size_t length = 0;
    size_t line_number = 0;
    char *cursor;
    char *end;
    int missing;
    int saw_default = 0;
    int result = -1;

    if (store == NULL || store->dir_fd < 0 || db == NULL)
        return set_error(error, error_size, "invalid host load request");
    memset(db, 0, sizeof(*db));
    if (read_database(store, NS_HOSTS_FILE, &data, &length, &missing, error,
                      error_size) < 0)
        return -1;
    if (missing)
        return 0;
    if (data[length - 1] != '\n') {
        (void)set_error(error, error_size,
                        "host database is truncated (missing final newline)");
        goto out;
    }
    cursor = data;
    end = data + length;
    while (cursor < end) {
        char *newline = memchr(cursor, '\n', (size_t)(end - cursor));
        char *fields[13];

        ++line_number;
        *newline = '\0';
        if (line_number == 1) {
            if (strcmp(cursor, "RG40XXV_NETSTREAM_HOSTS\t1") != 0) {
                (void)set_error(error, error_size,
                                "unsupported host database header");
                goto out;
            }
        } else if (line_number == 2 && strncmp(cursor, "D\t", 2) == 0) {
            if (saw_default || split_fields(cursor, fields, 2) < 0 ||
                strcmp(fields[0], "D") != 0 ||
                percent_decode(fields[1], db->default_name,
                               sizeof(db->default_name)) < 0) {
                (void)set_error(error, error_size,
                                "host database line %zu is malformed",
                                line_number);
                goto out;
            }
            saw_default = 1;
        } else if (line_number > 2 && saw_default &&
                   strncmp(cursor, "H\t", 2) == 0) {
            NsHost host;

            ns_host_defaults(&host);
            if (db->count >= NS_MAX_HOSTS ||
                split_fields(cursor, fields, 13) < 0 ||
                strcmp(fields[0], "H") != 0 ||
                percent_decode(fields[1], host.name, sizeof(host.name)) < 0 ||
                percent_decode(fields[2], host.address,
                               sizeof(host.address)) < 0 ||
                ns_parse_bool(fields[3], &host.paired) < 0 ||
                parse_u64(fields[4], 0, UINT64_C(4102444800),
                          &host.last_used) < 0 ||
                parse_u32(fields[5], 0, UINT32_MAX,
                          &host.resolution.width) < 0 ||
                parse_u32(fields[6], 0, UINT32_MAX,
                          &host.resolution.height) < 0 ||
                ns_parse_bool(fields[7], &host.resolution.custom) < 0 ||
                parse_u32(fields[8], 0, UINT32_MAX, &host.fps) < 0 ||
                parse_u32(fields[9], 0, UINT32_MAX,
                          &host.bitrate_kbps) < 0 ||
                parse_u32(fields[10], 0, UINT32_MAX,
                          &host.packet_size) < 0 ||
                ns_parse_codec(fields[11], &host.codec) < 0 ||
                ns_parse_aspect(fields[12], &host.aspect) < 0 ||
                ns_validate_host(&host, NULL, 0) < 0 ||
                ns_host_find(db, host.name) >= 0) {
                (void)set_error(error, error_size,
                                "host database line %zu is malformed",
                                line_number);
                goto out;
            }
            db->hosts[db->count++] = host;
        } else {
            (void)set_error(error, error_size,
                            "host database line %zu is malformed",
                            line_number);
            goto out;
        }
        cursor = newline + 1;
    }
    if (!saw_default || validate_host_db(db, error, error_size) < 0)
        goto out;
    result = 0;

out:
    if (result < 0)
        memset(db, 0, sizeof(*db));
    free(data);
    return result;
}

static int read_secret_fd(int fd, char *secret, size_t capacity, char *error,
                          size_t error_size)
{
    unsigned char buffer[512];
    size_t length = 0;
    int result = -1;

    if (secret == NULL || capacity < 2)
        return set_error(error, error_size, "invalid secret destination");
    secret[0] = '\0';
    for (;;) {
        ssize_t got;

        if (length == sizeof(buffer)) {
            (void)set_error(error, error_size, "secret input is too long");
            goto out;
        }
        got = read(fd, buffer + length, sizeof(buffer) - length);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            (void)set_error(error, error_size, "cannot read secret input: %s",
                            strerror(errno));
            goto out;
        }
        if (got == 0)
            break;
        length += (size_t)got;
    }
    if (length > 0 && buffer[length - 1] == '\n') {
        --length;
        if (length > 0 && buffer[length - 1] == '\r')
            --length;
    }
    if (length >= capacity) {
        (void)set_error(error, error_size, "secret input is too long");
        goto out;
    }
    if (memchr(buffer, '\0', length) != NULL ||
        memchr(buffer, '\n', length) != NULL ||
        memchr(buffer, '\r', length) != NULL) {
        (void)set_error(error, error_size,
                        "secret input contains a forbidden line break or NUL");
        goto out;
    }
    memcpy(secret, buffer, length);
    secret[length] = '\0';
    result = 0;

out:
    ns_secure_zero(buffer, sizeof(buffer));
    if (result < 0)
        ns_secure_zero(secret, capacity);
    return result;
}

int ns_read_secret_stdin(char *secret, size_t capacity, char *error,
                         size_t error_size)
{
    if (isatty(STDIN_FILENO)) {
        if (secret != NULL && capacity > 0)
            ns_secure_zero(secret, capacity);
        return set_error(error, error_size,
                         "refusing an echo-capable terminal; provide the secret through a pipe or restricted file");
    }
    return read_secret_fd(STDIN_FILENO, secret, capacity, error, error_size);
}

int ns_read_secret_file(const char *path, char *secret, size_t capacity,
                        char *error, size_t error_size)
{
    struct stat st;
    int fd;
    int result;

    if (path == NULL || path[0] != '/')
        return set_error(error, error_size,
                         "secret file must use an absolute path");
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return set_error(error, error_size, "cannot open secret file: %s",
                         strerror(errno));
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) ||
        st.st_uid != geteuid() || st.st_nlink != 1 ||
        ((st.st_mode & 07777) != 0600 && (st.st_mode & 07777) != 0400)) {
        close(fd);
        return set_error(error, error_size,
                         "secret file must be owned by this user, non-linked, and mode 0600 or 0400");
    }
    if (st.st_size < 0 || (uintmax_t)st.st_size > sizeof((unsigned char[512]){0})) {
        close(fd);
        return set_error(error, error_size, "secret file is too large");
    }
    result = read_secret_fd(fd, secret, capacity, error, error_size);
    close(fd);
    return result;
}

static int read_vendor_file(const char *path, char **data, size_t *length,
                            int *insecure, char *error, size_t error_size)
{
    struct stat st;
    int fd;
    char *buffer;

    if (path == NULL || path[0] != '/')
        return set_error(error, error_size,
                         "vendor Wi-Fi file must use an absolute path");
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return set_error(error, error_size,
                         "cannot open vendor Wi-Fi file: %s", strerror(errno));
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1) {
        close(fd);
        return set_error(error, error_size,
                         "vendor Wi-Fi source is not a safe regular file");
    }
    if (st.st_size <= 0 || (uintmax_t)st.st_size > NS_VENDOR_MAX_BYTES) {
        close(fd);
        return set_error(error, error_size,
                         "vendor Wi-Fi source has an invalid size");
    }
    *insecure = (st.st_mode & 0077) != 0;
    buffer = malloc((size_t)st.st_size + 1);
    if (buffer == NULL) {
        close(fd);
        return set_error(error, error_size, "out of memory");
    }
    if (read_all_fd(fd, buffer, (size_t)st.st_size) < 0) {
        int saved = errno;

        close(fd);
        ns_secure_zero(buffer, (size_t)st.st_size + 1);
        free(buffer);
        return set_error(error, error_size,
                         "cannot read vendor Wi-Fi source: %s",
                         strerror(saved));
    }
    close(fd);
    buffer[st.st_size] = '\0';
    *data = buffer;
    *length = (size_t)st.st_size;
    return 0;
}

int ns_wifi_import_vendor(NsWifiDb *db, const char *path, int make_default,
                          size_t *imported, int *source_was_insecure,
                          char *error, size_t error_size)
{
    NsWifiDb working;
    char *data = NULL;
    size_t length = 0;
    char *cursor;
    char *end;
    size_t count = 0;
    char first_ssid[NS_SSID_MAX_BYTES + 1] = {0};
    int insecure = 0;
    int result = -1;

    if (db == NULL || imported == NULL || source_was_insecure == NULL)
        return set_error(error, error_size, "invalid vendor import request");
    *imported = 0;
    *source_was_insecure = 0;
    if (validate_wifi_db(db, error, error_size) < 0)
        return -1;
    if (read_vendor_file(path, &data, &length, &insecure, error, error_size) < 0)
        return -1;
    working = *db;
    cursor = data;
    end = data + length;
    while (cursor < end) {
        char *line_end = memchr(cursor, '\n', (size_t)(end - cursor));
        char *separator;
        size_t line_length = line_end == NULL ? (size_t)(end - cursor)
                                               : (size_t)(line_end - cursor);
        NsWifiProfile profile = {0};
        size_t ssid_length;
        size_t secret_length;

        if (line_length > 0 && cursor[line_length - 1] == '\r')
            --line_length;
        if (line_length == 0) {
            cursor = line_end == NULL ? end : line_end + 1;
            continue;
        }
        if (line_length < 5 || memchr(cursor, '\0', line_length) != NULL ||
            cursor[0] != 'S' || cursor[1] != ':') {
            (void)set_error(error, error_size,
                            "vendor Wi-Fi source contains a malformed record");
            goto out;
        }
        separator = memmem(cursor + 2, line_length - 2, "\tP:", 3);
        if (separator == NULL ||
            memmem(separator + 3,
                   line_length - (size_t)(separator + 3 - cursor), "\tP:",
                   3) != NULL) {
            (void)set_error(error, error_size,
                            "vendor Wi-Fi source contains a malformed record");
            goto out;
        }
        ssid_length = (size_t)(separator - (cursor + 2));
        secret_length = line_length - (size_t)(separator + 3 - cursor);
        if (ssid_length == 0 || ssid_length > NS_SSID_MAX_BYTES ||
            secret_length > NS_WIFI_SECRET_MAX_BYTES) {
            (void)set_error(error, error_size,
                            "vendor Wi-Fi source contains an invalid credential length");
            goto out;
        }
        memcpy(profile.ssid, cursor + 2, ssid_length);
        profile.ssid[ssid_length] = '\0';
        memcpy(profile.secret, separator + 3, secret_length);
        profile.secret[secret_length] = '\0';
        profile.security = secret_length == 0 ? NS_WIFI_OPEN : NS_WIFI_WPA2_PSK;
        if (ns_validate_wifi(&profile, NULL, 0) < 0 ||
            ns_wifi_upsert(&working, &profile, error, error_size) < 0) {
            ns_secure_zero(&profile, sizeof(profile));
            (void)set_error(error, error_size,
                            "vendor Wi-Fi source contains an invalid profile");
            goto out;
        }
        if (count == 0)
            (void)snprintf(first_ssid, sizeof(first_ssid), "%s", profile.ssid);
        ++count;
        ns_secure_zero(&profile, sizeof(profile));
        cursor = line_end == NULL ? end : line_end + 1;
    }
    if (count == 0) {
        (void)set_error(error, error_size,
                        "vendor Wi-Fi source contains no profiles");
        goto out;
    }
    if ((make_default || working.default_ssid[0] == '\0') &&
        ns_wifi_select(&working, first_ssid, error, error_size) < 0)
        goto out;
    *db = working;
    ns_secure_zero(&working, sizeof(working));
    *imported = count;
    *source_was_insecure = insecure;
    result = 0;

out:
    ns_secure_zero(&working, sizeof(working));
    ns_secure_zero(first_ssid, sizeof(first_ssid));
    ns_secure_zero(data, length);
    free(data);
    return result;
}
