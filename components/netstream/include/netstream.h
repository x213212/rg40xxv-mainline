#ifndef RG40XXV_NETSTREAM_H
#define RG40XXV_NETSTREAM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NS_MAX_WIFI_PROFILES 64
#define NS_MAX_HOSTS 128
#define NS_SSID_MAX_BYTES 32
#define NS_WIFI_SECRET_MAX_BYTES 64
#define NS_HOST_NAME_MAX_BYTES 96
#define NS_HOST_ADDRESS_MAX_BYTES 253
#define NS_ERROR_MAX 256

typedef enum {
    NS_WIFI_OPEN = 0,
    NS_WIFI_WPA2_PSK = 1,
    NS_WIFI_WPA3_SAE = 2
} NsWifiSecurity;

typedef enum {
    NS_CODEC_H264 = 0,
    NS_CODEC_H265 = 1,
    NS_CODEC_AV1 = 2
} NsCodec;

typedef enum {
    NS_ASPECT_FIT = 0,
    NS_ASPECT_FILL = 1,
    NS_ASPECT_STRETCH = 2
} NsAspect;

typedef struct {
    char ssid[NS_SSID_MAX_BYTES + 1];
    NsWifiSecurity security;
    int hidden;
    int priority;
    char secret[NS_WIFI_SECRET_MAX_BYTES + 1];
} NsWifiProfile;

typedef struct {
    NsWifiProfile profiles[NS_MAX_WIFI_PROFILES];
    size_t count;
    char default_ssid[NS_SSID_MAX_BYTES + 1];
} NsWifiDb;

typedef struct {
    uint32_t width;
    uint32_t height;
    int custom;
} NsResolution;

typedef struct {
    char name[NS_HOST_NAME_MAX_BYTES + 1];
    char address[NS_HOST_ADDRESS_MAX_BYTES + 1];
    int paired;
    uint64_t last_used;
    NsResolution resolution;
    uint32_t fps;
    uint32_t bitrate_kbps;
    uint32_t packet_size;
    NsCodec codec;
    NsAspect aspect;
} NsHost;

typedef struct {
    NsHost hosts[NS_MAX_HOSTS];
    size_t count;
    char default_name[NS_HOST_NAME_MAX_BYTES + 1];
} NsHostDb;

typedef struct {
    int dir_fd;
    int lock_fd;
} NsStore;

void ns_secure_zero(void *ptr, size_t len);

const char *ns_wifi_security_name(NsWifiSecurity value);
const char *ns_codec_name(NsCodec value);
const char *ns_aspect_name(NsAspect value);
int ns_parse_wifi_security(const char *text, NsWifiSecurity *out);
int ns_parse_codec(const char *text, NsCodec *out);
int ns_parse_aspect(const char *text, NsAspect *out);
int ns_parse_bool(const char *text, int *out);

int ns_validate_utf8_text(const char *text, size_t min_bytes,
                          size_t max_bytes);
int ns_validate_wifi(const NsWifiProfile *profile, char *error,
                     size_t error_size);
void ns_host_defaults(NsHost *host);
int ns_validate_host(const NsHost *host, char *error, size_t error_size);

int ns_store_open(NsStore *store, const char *state_dir, char *error,
                  size_t error_size);
void ns_store_close(NsStore *store);
int ns_wifi_load(NsStore *store, NsWifiDb *db, char *error,
                 size_t error_size);
int ns_wifi_save(NsStore *store, const NsWifiDb *db, char *error,
                 size_t error_size);
int ns_hosts_load(NsStore *store, NsHostDb *db, char *error,
                  size_t error_size);
int ns_hosts_save(NsStore *store, const NsHostDb *db, char *error,
                  size_t error_size);

int ns_wifi_find(const NsWifiDb *db, const char *ssid);
int ns_wifi_upsert(NsWifiDb *db, const NsWifiProfile *profile, char *error,
                   size_t error_size);
int ns_wifi_forget(NsWifiDb *db, const char *ssid, char *error,
                   size_t error_size);
int ns_wifi_select(NsWifiDb *db, const char *ssid, char *error,
                   size_t error_size);

int ns_host_find(const NsHostDb *db, const char *name);
int ns_host_upsert(NsHostDb *db, const NsHost *host, char *error,
                   size_t error_size);
int ns_host_delete(NsHostDb *db, const char *name, char *error,
                   size_t error_size);
int ns_host_select(NsHostDb *db, const char *name, char *error,
                   size_t error_size);

int ns_read_secret_stdin(char *secret, size_t capacity, char *error,
                         size_t error_size);
int ns_read_secret_file(const char *path, char *secret, size_t capacity,
                        char *error, size_t error_size);

/* Imports S:<ssid>\tP:<password> records. No credential is ever printed. */
int ns_wifi_import_vendor(NsWifiDb *db, const char *path, int make_default,
                          size_t *imported, int *source_was_insecure,
                          char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
