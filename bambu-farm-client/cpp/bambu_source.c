#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <dlfcn.h>
#include <limits.h>

typedef char tchar;
typedef void *Bambu_Tunnel;
typedef void (*Logger)(void *context, int level, tchar const *msg);

typedef struct Bambu_StreamInfo {
    int type;
    int sub_type;
    int format_type;
    int format_size;
    int max_frame_size;
    unsigned char const *format_buffer;
} Bambu_StreamInfo;

typedef struct Bambu_Sample {
    int itrack;
    int size;
    int flags;
    unsigned char const *buffer;
    unsigned long long decode_time;
} Bambu_Sample;

typedef int (*Bambu_Create_Fn)(Bambu_Tunnel *tunnel, char const *path);
typedef void (*Bambu_SetLogger_Fn)(Bambu_Tunnel tunnel, Logger logger, void *context);
typedef int (*Bambu_Open_Fn)(Bambu_Tunnel tunnel);
typedef int (*Bambu_StartStream_Fn)(Bambu_Tunnel tunnel, bool video);
typedef int (*Bambu_StartStreamEx_Fn)(Bambu_Tunnel tunnel, int type);
typedef int (*Bambu_GetStreamCount_Fn)(Bambu_Tunnel tunnel);
typedef int (*Bambu_GetStreamInfo_Fn)(Bambu_Tunnel tunnel, int index, Bambu_StreamInfo *info);
typedef unsigned long (*Bambu_GetDuration_Fn)(Bambu_Tunnel tunnel);
typedef int (*Bambu_Seek_Fn)(Bambu_Tunnel tunnel, unsigned long time);
typedef int (*Bambu_ReadSample_Fn)(Bambu_Tunnel tunnel, Bambu_Sample *sample);
typedef int (*Bambu_SendMessage_Fn)(Bambu_Tunnel tunnel, int ctrl, char const *data, int len);
typedef int (*Bambu_RecvMessage_Fn)(Bambu_Tunnel tunnel, int *ctrl, char *data, int *len);
typedef void (*Bambu_Close_Fn)(Bambu_Tunnel tunnel);
typedef void (*Bambu_Destroy_Fn)(Bambu_Tunnel tunnel);
typedef int (*Bambu_Init_Fn)(void);
typedef void (*Bambu_Deinit_Fn)(void);
typedef char const *(*Bambu_GetLastErrorMsg_Fn)(void);
typedef void (*Bambu_FreeLogMsg_Fn)(tchar const *msg);

static void *network_module = NULL;
static Bambu_Create_Fn fn_Bambu_Create = NULL;
static Bambu_SetLogger_Fn fn_Bambu_SetLogger = NULL;
static Bambu_Open_Fn fn_Bambu_Open = NULL;
static Bambu_StartStream_Fn fn_Bambu_StartStream = NULL;
static Bambu_StartStreamEx_Fn fn_Bambu_StartStreamEx = NULL;
static Bambu_GetStreamCount_Fn fn_Bambu_GetStreamCount = NULL;
static Bambu_GetStreamInfo_Fn fn_Bambu_GetStreamInfo = NULL;
static Bambu_GetDuration_Fn fn_Bambu_GetDuration = NULL;
static Bambu_Seek_Fn fn_Bambu_Seek = NULL;
static Bambu_ReadSample_Fn fn_Bambu_ReadSample = NULL;
static Bambu_SendMessage_Fn fn_Bambu_SendMessage = NULL;
static Bambu_RecvMessage_Fn fn_Bambu_RecvMessage = NULL;
static Bambu_Close_Fn fn_Bambu_Close = NULL;
static Bambu_Destroy_Fn fn_Bambu_Destroy = NULL;
static Bambu_Init_Fn fn_Bambu_Init = NULL;
static Bambu_Deinit_Fn fn_Bambu_Deinit = NULL;
static Bambu_GetLastErrorMsg_Fn fn_Bambu_GetLastErrorMsg = NULL;
static Bambu_FreeLogMsg_Fn fn_Bambu_FreeLogMsg = NULL;
static char last_error[512];

static void write_diag(const char *message) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    const char *path = "/tmp/bambu-oss-plugin-debug.log";
    char buffer[1024];

    if (xdg && *xdg) {
        snprintf(buffer, sizeof(buffer), "%s/BambuStudio/oss-plugin-debug.log", xdg);
        path = buffer;
    } else if (home && *home) {
        snprintf(buffer, sizeof(buffer), "%s/.config/BambuStudio/oss-plugin-debug.log", home);
        path = buffer;
    }

    FILE *f = fopen(path, "a");
    if (!f) {
        return;
    }
    fprintf(f, "%s\n", message);
    fclose(f);
}

static int load_network_module(void) {
    if (network_module) {
        return 0;
    }

    Dl_info info;
    if (dladdr((void *) &load_network_module, &info) == 0 || !info.dli_fname) {
        snprintf(last_error, sizeof(last_error), "dladdr failed");
        write_diag("libBambuSource: dladdr failed");
        return -1;
    }

    char path[PATH_MAX];
    strncpy(path, info.dli_fname, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    char *slash = strrchr(path, '/');
    if (!slash) {
        snprintf(last_error, sizeof(last_error), "invalid module path");
        write_diag("libBambuSource: invalid module path");
        return -1;
    }
    *slash = '\0';

    char network_path[PATH_MAX];
    snprintf(network_path, sizeof(network_path), "%s/libbambu_networking.so", path);
    network_module = dlopen(network_path, RTLD_NOW | RTLD_GLOBAL);
    if (!network_module) {
        snprintf(last_error, sizeof(last_error), "dlopen failed: %s", dlerror());
        write_diag(last_error);
        return -1;
    }

#define LOAD_SYM(name) do { \
    fn_##name = (name##_Fn) dlsym(network_module, #name); \
    if (!fn_##name) { \
        snprintf(last_error, sizeof(last_error), "missing symbol %s: %s", #name, dlerror()); \
        write_diag(last_error); \
        return -1; \
    } \
} while (0)

    LOAD_SYM(Bambu_Create);
    LOAD_SYM(Bambu_SetLogger);
    LOAD_SYM(Bambu_Open);
    LOAD_SYM(Bambu_StartStream);
    LOAD_SYM(Bambu_StartStreamEx);
    LOAD_SYM(Bambu_GetStreamCount);
    LOAD_SYM(Bambu_GetStreamInfo);
    LOAD_SYM(Bambu_GetDuration);
    LOAD_SYM(Bambu_Seek);
    LOAD_SYM(Bambu_ReadSample);
    LOAD_SYM(Bambu_SendMessage);
    LOAD_SYM(Bambu_RecvMessage);
    LOAD_SYM(Bambu_Close);
    LOAD_SYM(Bambu_Destroy);
    LOAD_SYM(Bambu_Init);
    LOAD_SYM(Bambu_Deinit);
    LOAD_SYM(Bambu_GetLastErrorMsg);
    LOAD_SYM(Bambu_FreeLogMsg);

#undef LOAD_SYM

    write_diag("libBambuSource: loaded libbambu_networking.so");
    return 0;
}

__attribute__((constructor))
static void bambu_source_init(void) {
    write_diag("libBambuSource constructor");
    load_network_module();
}

#define REQUIRE_LOADED() do { if (load_network_module() != 0) return -1; } while (0)
#define REQUIRE_LOADED_VOID() do { if (load_network_module() != 0) return; } while (0)

__attribute__((visibility("default"))) int Bambu_Create(Bambu_Tunnel *tunnel, char const *path) { REQUIRE_LOADED(); return fn_Bambu_Create(tunnel, path); }
__attribute__((visibility("default"))) void Bambu_SetLogger(Bambu_Tunnel tunnel, Logger logger, void *context) { REQUIRE_LOADED_VOID(); fn_Bambu_SetLogger(tunnel, logger, context); }
__attribute__((visibility("default"))) int Bambu_Open(Bambu_Tunnel tunnel) { REQUIRE_LOADED(); return fn_Bambu_Open(tunnel); }
__attribute__((visibility("default"))) int Bambu_StartStream(Bambu_Tunnel tunnel, bool video) { REQUIRE_LOADED(); return fn_Bambu_StartStream(tunnel, video); }
__attribute__((visibility("default"))) int Bambu_StartStreamEx(Bambu_Tunnel tunnel, int type) { REQUIRE_LOADED(); return fn_Bambu_StartStreamEx(tunnel, type); }
__attribute__((visibility("default"))) int Bambu_GetStreamCount(Bambu_Tunnel tunnel) { REQUIRE_LOADED(); return fn_Bambu_GetStreamCount(tunnel); }
__attribute__((visibility("default"))) int Bambu_GetStreamInfo(Bambu_Tunnel tunnel, int index, Bambu_StreamInfo *info) { REQUIRE_LOADED(); return fn_Bambu_GetStreamInfo(tunnel, index, info); }
__attribute__((visibility("default"))) unsigned long Bambu_GetDuration(Bambu_Tunnel tunnel) { if (load_network_module() != 0) return 0; return fn_Bambu_GetDuration(tunnel); }
__attribute__((visibility("default"))) int Bambu_Seek(Bambu_Tunnel tunnel, unsigned long time) { REQUIRE_LOADED(); return fn_Bambu_Seek(tunnel, time); }
__attribute__((visibility("default"))) int Bambu_ReadSample(Bambu_Tunnel tunnel, Bambu_Sample *sample) { REQUIRE_LOADED(); return fn_Bambu_ReadSample(tunnel, sample); }
__attribute__((visibility("default"))) int Bambu_SendMessage(Bambu_Tunnel tunnel, int ctrl, char const *data, int len) { REQUIRE_LOADED(); return fn_Bambu_SendMessage(tunnel, ctrl, data, len); }
__attribute__((visibility("default"))) int Bambu_RecvMessage(Bambu_Tunnel tunnel, int *ctrl, char *data, int *len) { REQUIRE_LOADED(); return fn_Bambu_RecvMessage(tunnel, ctrl, data, len); }
__attribute__((visibility("default"))) void Bambu_Close(Bambu_Tunnel tunnel) { REQUIRE_LOADED_VOID(); fn_Bambu_Close(tunnel); }
__attribute__((visibility("default"))) void Bambu_Destroy(Bambu_Tunnel tunnel) { REQUIRE_LOADED_VOID(); fn_Bambu_Destroy(tunnel); }
__attribute__((visibility("default"))) int Bambu_Init(void) { REQUIRE_LOADED(); return fn_Bambu_Init(); }
__attribute__((visibility("default"))) void Bambu_Deinit(void) { REQUIRE_LOADED_VOID(); fn_Bambu_Deinit(); }
__attribute__((visibility("default"))) char const *Bambu_GetLastErrorMsg(void) {
    if (load_network_module() != 0) {
        return last_error;
    }
    return fn_Bambu_GetLastErrorMsg();
}
__attribute__((visibility("default"))) void Bambu_FreeLogMsg(tchar const *msg) {
    if (load_network_module() != 0) {
        free((void *) msg);
        return;
    }
    fn_Bambu_FreeLogMsg(msg);
}
