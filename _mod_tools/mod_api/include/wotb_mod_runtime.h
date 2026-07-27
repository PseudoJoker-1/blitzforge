#pragma once

#include <stdint.h>

#include "wotb_mod_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void(WOTBMOD_CALL* WotbModRuntimeLogSink)(
    WotbModLogLevel level,
    const char* message,
    void* user_data);

typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeHookCreate)(
    void* user_data,
    void* target,
    void* detour,
    void** original);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeHookOperation)(
    void* user_data,
    void* target);

/*
 * The runtime deliberately does not choose a hooking library. The loader
 * architecture supplies an adapter (for MinHook, PolyHook, or another engine).
 */
typedef struct WotbModRuntimeHookBackend {
    uint32_t struct_size;
    void* user_data;
    WotbModRuntimeHookCreate create;
    WotbModRuntimeHookOperation enable;
    WotbModRuntimeHookOperation disable;
    WotbModRuntimeHookOperation remove;
} WotbModRuntimeHookBackend;

typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeResourceLoad)(
    void* user_data,
    const WotbModResourceLoadRequest* request,
    void** out_native_resource);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeResourceOperation)(
    void* user_data,
    void* native_resource);
typedef void(WOTBMOD_CALL* WotbModRuntimeResourceRegistryChanged)(
    void* user_data,
    uint64_t generation);

/*
 * The loader owns DAVA C++ types and supplies this bridge. The public mod ABI
 * remains compiler-neutral and exposes only typed, opaque resource handles.
 */
typedef struct WotbModRuntimeResourceBackend {
    uint32_t struct_size;
    void* user_data;
    WotbModRuntimeResourceLoad load;
    WotbModRuntimeResourceOperation reload;
    WotbModRuntimeResourceOperation release;
    WotbModRuntimeResourceRegistryChanged registry_changed;
} WotbModRuntimeResourceBackend;

typedef struct WotbModRuntimeOptions {
    uint32_t struct_size;
    const char* game_directory;
    const char* mods_directory;
    void* game_module;
    WotbModRuntimeLogSink log_sink;
    void* log_user_data;
    const WotbModRuntimeHookBackend* hook_backend;
    const WotbModRuntimeResourceBackend* resource_backend;
} WotbModRuntimeOptions;

WotbModResult WOTBMOD_CALL WotbModRuntime_Initialize(
    const WotbModRuntimeOptions* options);
WotbModResult WOTBMOD_CALL WotbModRuntime_LoadAll(void);
void WOTBMOD_CALL WotbModRuntime_DispatchFrame(
    void* swap_chain,
    void* device,
    void* device_context,
    uint32_t back_buffer_width,
    uint32_t back_buffer_height,
    double delta_seconds);
void WOTBMOD_CALL WotbModRuntime_Shutdown(void);
const WotbModHostApi* WOTBMOD_CALL WotbModRuntime_GetHostApi(void);

/*
 * Called by the loader's reviewed file-resolution integration. It returns a
 * loose or .dvpl file from the highest-priority enabled mod mount. NOT_FOUND
 * means the original game resolver should continue unchanged.
 */
WotbModResult WOTBMOD_CALL WotbModRuntime_ResolveResourcePath(
    const char* requested_path,
    char* buffer,
    uint32_t* inout_size);
uint64_t WOTBMOD_CALL WotbModRuntime_GetResourceGeneration(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
