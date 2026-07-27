#pragma once

/*
 * WoT Blitz Mod API v1
 *
 * This header is the binary contract between a loader host and third-party
 * mod DLLs. Keep it C-compatible: do not place STL types, C++
 * classes, exceptions, or compiler-owned allocations in public structures.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define WOTBMOD_EXTERN_C extern "C"
#else
#define WOTBMOD_EXTERN_C extern
#endif

#if defined(_MSC_VER)
#define WOTBMOD_CALL __cdecl
#define WOTBMOD_EXPORT WOTBMOD_EXTERN_C __declspec(dllexport)
#else
#define WOTBMOD_CALL
#define WOTBMOD_EXPORT WOTBMOD_EXTERN_C
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ABI 2 removed the anchor calls from WotbModHostApi. A mod built against
 * ABI 1 expects a larger host table, so the major bump makes the loader
 * reject it rather than let it read past the end.
 */
#define WOTBMOD_ABI_VERSION_2 0x00020000u
#define WOTBMOD_ABI_VERSION_2_1 0x00020001u
#define WOTBMOD_ABI_VERSION WOTBMOD_ABI_VERSION_2_1
#define WOTBMOD_HOST_VERSION 0x00010001u
#define WOTBMOD_ABI_MAJOR(version) ((uint32_t)(version) >> 16)
#define WOTBMOD_ABI_MINOR(version) ((uint32_t)(version) & 0xFFFFu)

#if defined(_MSC_VER) && defined(WOTBMOD_RUNTIME_BUILD)
#define WOTBMOD_HOST_EXPORT __declspec(dllexport)
#else
#define WOTBMOD_HOST_EXPORT
#endif

#define WOTBMOD_ENTRY_NAME "WotbModLoad"
#define WOTBMOD_MAX_ID 64u
#define WOTBMOD_MAX_NAME 96u
#define WOTBMOD_MAX_VERSION 32u
#define WOTBMOD_MAX_AUTHOR 96u
#define WOTBMOD_MAX_DESCRIPTION 256u
#define WOTBMOD_MAX_PATH 260u
#define WOTBMOD_MAX_RESOURCE_PATH 1024u

typedef void* WotbModHandle;
typedef uint64_t WotbModResourceMountId;
typedef void* WotbModResourceHandle;

typedef enum WotbModResult {
    WOTBMOD_OK = 0,
    WOTBMOD_ERROR_INVALID_ARGUMENT = 1,
    WOTBMOD_ERROR_UNSUPPORTED_ABI = 2,
    WOTBMOD_ERROR_NOT_FOUND = 3,
    WOTBMOD_ERROR_ALREADY_EXISTS = 4,
    WOTBMOD_ERROR_PLATFORM = 5,
    WOTBMOD_ERROR_ACCESS_DENIED = 6,
    WOTBMOD_ERROR_BUFFER_TOO_SMALL = 7,
    WOTBMOD_ERROR_DISABLED = 8,
    WOTBMOD_ERROR_LIMIT_REACHED = 9,
    WOTBMOD_ERROR_CALLBACK_FAULT = 10
} WotbModResult;

typedef enum WotbModLogLevel {
    WOTBMOD_LOG_TRACE = 0,
    WOTBMOD_LOG_INFO = 1,
    WOTBMOD_LOG_WARNING = 2,
    WOTBMOD_LOG_ERROR = 3
} WotbModLogLevel;

typedef enum WotbModPath {
    WOTBMOD_PATH_GAME = 0,
    WOTBMOD_PATH_MODS = 1,
    WOTBMOD_PATH_MODULE = 2,
    WOTBMOD_PATH_DATA = 3,
    WOTBMOD_PATH_CONFIG = 4
} WotbModPath;

typedef enum WotbModState {
    WOTBMOD_STATE_DISCOVERED = 0,
    WOTBMOD_STATE_LOADED = 1,
    WOTBMOD_STATE_ENABLED = 2,
    WOTBMOD_STATE_DISABLED = 3,
    WOTBMOD_STATE_FAULTED = 4
} WotbModState;

typedef enum WotbModResourceType {
    WOTBMOD_RESOURCE_GENERIC = 0,
    WOTBMOD_RESOURCE_YAML_DOCUMENT = 1,
    WOTBMOD_RESOURCE_UI_PACKAGE = 2,
    WOTBMOD_RESOURCE_UI_CONTROL = 3,
    WOTBMOD_RESOURCE_SCENE = 4,
    WOTBMOD_RESOURCE_TEXTURE = 5
} WotbModResourceType;

typedef enum WotbModResourceMountFlags {
    WOTBMOD_RESOURCE_MOUNT_NONE = 0,
    /*
     * If the exact loose file is absent, also try the same path with ".dvpl".
     * This matches the packaged UI, YAML, texture, and .sc2 layout used by the
     * client while still allowing unpacked files during development.
     */
    WOTBMOD_RESOURCE_MOUNT_SEARCH_DVPL = 1u << 0
} WotbModResourceMountFlags;

/*
 * source_directory is relative to the calling mod's WOTBMOD_PATH_DATA
 * directory. Absolute paths and ".." traversal are rejected.
 *
 * virtual_root accepts "~res:/...", "res:/...", "Data/...", or a path
 * relative to the DAVA resource root. The runtime normalizes it to ~res:/.
 */
typedef struct WotbModResourceMountInfo {
    uint32_t struct_size;
    const char* virtual_root;
    const char* source_directory;
    int32_t priority;
    uint32_t flags;
} WotbModResourceMountInfo;

/*
 * object_name is optional. A resource bridge may use it for a named control
 * inside a UI package; scene and YAML loads normally leave it null.
 */
typedef struct WotbModResourceLoadRequest {
    uint32_t struct_size;
    WotbModResourceType type;
    const char* virtual_path;
    const char* object_name;
    uint32_t flags;
} WotbModResourceLoadRequest;

typedef struct WotbModFrameInfo {
    uint32_t struct_size;
    uint64_t frame_index;
    double delta_seconds;
    void* swap_chain;      /* IDXGISwapChain*, kept opaque in the ABI */
    void* device;          /* ID3D11Device* */
    void* device_context;  /* ID3D11DeviceContext* */
    uint32_t back_buffer_width;
    uint32_t back_buffer_height;
} WotbModFrameInfo;

struct WotbModHostApi;

typedef void(WOTBMOD_CALL* WotbModEnableCallback)(
    const struct WotbModHostApi* host,
    WotbModHandle mod);
typedef void(WOTBMOD_CALL* WotbModDisableCallback)(
    const struct WotbModHostApi* host,
    WotbModHandle mod);
typedef void(WOTBMOD_CALL* WotbModUnloadCallback)(
    const struct WotbModHostApi* host,
    WotbModHandle mod);
typedef void(WOTBMOD_CALL* WotbModFrameCallback)(
    const struct WotbModHostApi* host,
    WotbModHandle mod,
    const WotbModFrameInfo* frame);

/*
 * A mod fills this descriptor from WotbModLoad. String storage must remain
 * valid until on_unload, although the v1 host copies every string immediately.
 */
typedef struct WotbModInfo {
    uint32_t struct_size;
    uint32_t abi_version;
    const char* id;
    const char* name;
    const char* version;
    const char* author;
    const char* description;
    uint32_t flags;
    WotbModEnableCallback on_enable;
    WotbModDisableCallback on_disable;
    WotbModUnloadCallback on_unload;
    WotbModFrameCallback on_frame;
} WotbModInfo;

/*
 * Fixed-size copy used by launchers and UI layers. Callers must set
 * struct_size before passing this structure to get_mod_info.
 */
typedef struct WotbModPublicInfo {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t state;
    uint32_t enabled;
    uint32_t fault_count;
    uint32_t flags;
    char id[WOTBMOD_MAX_ID];
    char name[WOTBMOD_MAX_NAME];
    char version[WOTBMOD_MAX_VERSION];
    char author[WOTBMOD_MAX_AUTHOR];
    char description[WOTBMOD_MAX_DESCRIPTION];
    char module_path[WOTBMOD_MAX_PATH];
} WotbModPublicInfo;

typedef struct WotbModHostApi {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t host_version;
    uint32_t reserved;

    void(WOTBMOD_CALL* log)(
        WotbModHandle mod,
        WotbModLogLevel level,
        const char* message);

    /*
     * Buffer contract: set *inout_size to buffer capacity. On success it is
     * the byte count including the trailing NUL. With a null/short buffer,
     * WOTBMOD_ERROR_BUFFER_TOO_SMALL is returned and the required size is set.
     */
    WotbModResult(WOTBMOD_CALL* get_path)(
        WotbModHandle mod,
        WotbModPath path,
        char* buffer,
        uint32_t* inout_size);

    void*(WOTBMOD_CALL* get_game_module)(void);
    void*(WOTBMOD_CALL* resolve_rva)(uint32_t rva);
    void*(WOTBMOD_CALL* get_proc_address)(
        const char* loaded_module_name,
        const char* export_name);

    /*
     * mask uses 'x' for an exact byte and '?' for a wildcard. A null module
     * name scans the main executable image.
     */
    void*(WOTBMOD_CALL* find_pattern)(
        const char* loaded_module_name,
        const uint8_t* pattern,
        const char* mask);

    WotbModResult(WOTBMOD_CALL* hook_create)(
        WotbModHandle mod,
        void* target,
        void* detour,
        void** original);
    WotbModResult(WOTBMOD_CALL* hook_enable)(
        WotbModHandle mod,
        void* target);
    WotbModResult(WOTBMOD_CALL* hook_disable)(
        WotbModHandle mod,
        void* target);
    WotbModResult(WOTBMOD_CALL* hook_remove)(
        WotbModHandle mod,
        void* target);

    int32_t(WOTBMOD_CALL* config_get_int)(
        WotbModHandle mod,
        const char* section,
        const char* key,
        int32_t default_value);
    WotbModResult(WOTBMOD_CALL* config_set_int)(
        WotbModHandle mod,
        const char* section,
        const char* key,
        int32_t value);
    WotbModResult(WOTBMOD_CALL* config_get_string)(
        WotbModHandle mod,
        const char* section,
        const char* key,
        const char* default_value,
        char* buffer,
        uint32_t buffer_size);
    WotbModResult(WOTBMOD_CALL* config_set_string)(
        WotbModHandle mod,
        const char* section,
        const char* key,
        const char* value);

    uint32_t(WOTBMOD_CALL* get_mod_count)(void);
    WotbModResult(WOTBMOD_CALL* get_mod_info)(
        uint32_t index,
        WotbModPublicInfo* out_info);
    WotbModResult(WOTBMOD_CALL* set_mod_enabled)(
        const char* id,
        int32_t enabled);

    /*
     * Resource overlays are resolved by priority and are active only while
     * their owner mod is enabled. Mount ids and loaded resource handles are
     * owned by the registering mod and cannot be released by another mod.
     */
    WotbModResult(WOTBMOD_CALL* resource_mount)(
        WotbModHandle mod,
        const WotbModResourceMountInfo* mount,
        WotbModResourceMountId* out_mount_id);
    WotbModResult(WOTBMOD_CALL* resource_unmount)(
        WotbModHandle mod,
        WotbModResourceMountId mount_id);
    WotbModResult(WOTBMOD_CALL* resource_resolve)(
        WotbModHandle mod,
        const char* virtual_path,
        char* buffer,
        uint32_t* inout_size);
    WotbModResult(WOTBMOD_CALL* resource_load)(
        WotbModHandle mod,
        const WotbModResourceLoadRequest* request,
        WotbModResourceHandle* out_resource);
    WotbModResult(WOTBMOD_CALL* resource_reload)(
        WotbModHandle mod,
        WotbModResourceHandle resource);
    WotbModResult(WOTBMOD_CALL* resource_release)(
        WotbModHandle mod,
        WotbModResourceHandle resource);

    /*
     * There is deliberately no named raw-address registry here. Loader code
     * wires the reviewed resource bridge to the verified DAVA functions;
     * third-party mods receive only typed resource operations.
     */
} WotbModHostApi;

typedef WotbModResult(WOTBMOD_CALL* WotbModLoadFn)(
    const WotbModHostApi* host,
    WotbModHandle mod,
    WotbModInfo* out_info);

/*
 * Optional exports a loader may provide. Mods normally use the host pointer
 * passed to WotbModLoad; these exports are intended for the loader's own UI.
 */
WOTBMOD_HOST_EXPORT const WotbModHostApi* WOTBMOD_CALL
WotbModApi_GetHost(void);
WOTBMOD_HOST_EXPORT uint32_t WOTBMOD_CALL WotbModApi_GetVersion(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#define WOTBMOD_DEFINE_ENTRY(function_name)                                      \
    WOTBMOD_EXPORT WotbModResult WOTBMOD_CALL function_name(                    \
        const WotbModHostApi* host, WotbModHandle mod, WotbModInfo* out_info)

#define WOTBMOD_ENTRY WOTBMOD_DEFINE_ENTRY(WotbModLoad)

#ifdef __cplusplus
#include <stdarg.h>
#include <stdio.h>

static inline void wotbmod_logf(
    const WotbModHostApi* host,
    WotbModHandle mod,
    WotbModLogLevel level,
    const char* format,
    ...) {
    if (!host || !host->log || !format) return;
    char message[1024];
    va_list args;
    va_start(args, format);
#if defined(_MSC_VER)
    _vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
#else
    vsnprintf(message, sizeof(message), format, args);
#endif
    va_end(args);
    host->log(mod, level, message);
}
#endif
