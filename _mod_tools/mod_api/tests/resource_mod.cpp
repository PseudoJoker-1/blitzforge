#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stddef.h>
#include <string.h>

#include "../include/wotb_mod_api.h"

static WotbModResourceMountId g_lowMount = 0;
static WotbModResourceMountId g_highMount = 0;
static const DWORD kAssertionException = 0xE0000001u;

static void Require(bool condition) {
    if (!condition) {
        RaiseException(kAssertionException, 0, 0, nullptr);
    }
}

static WotbModResult Mount(
    const WotbModHostApi* host,
    WotbModHandle mod,
    const char* source,
    int32_t priority,
    WotbModResourceMountId* outMount) {
    WotbModResourceMountInfo mount = {};
    mount.struct_size = sizeof(mount);
    mount.virtual_root = "~res:/Tests/Priority/";
    mount.source_directory = source;
    mount.priority = priority;
    mount.flags = WOTBMOD_RESOURCE_MOUNT_SEARCH_DVPL;
    return host->resource_mount(mod, &mount, outMount);
}

static void WOTBMOD_CALL OnEnable(
    const WotbModHostApi* host,
    WotbModHandle mod) {
    Require(
        host->struct_size >=
            offsetof(WotbModHostApi, resource_unmount) +
                sizeof(host->resource_unmount));

    WotbModResourceMountId invalidMount = 0;
    Require(
        Mount(host, mod, "../escape", 1000, &invalidMount) ==
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Require(Mount(host, mod, "low", 10, &g_lowMount) == WOTBMOD_OK);
    Require(Mount(host, mod, "high", 20, &g_highMount) == WOTBMOD_OK);
}

static void WOTBMOD_CALL OnDisable(
    const WotbModHostApi* host,
    WotbModHandle mod) {
    if (g_highMount) {
        host->resource_unmount(mod, g_highMount);
        g_highMount = 0;
    }
    if (g_lowMount) {
        host->resource_unmount(mod, g_lowMount);
        g_lowMount = 0;
    }
}

WOTBMOD_ENTRY {
    (void)mod;
    if (!host || !out_info) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = sizeof(*out_info);
    out_info->abi_version = WOTBMOD_ABI_VERSION;
    out_info->id = "test.resources";
    out_info->name = "Resource Overlay Test";
    out_info->version = "1.0.0";
    out_info->author = "SDK tests";
    out_info->on_enable = &OnEnable;
    out_info->on_disable = &OnDisable;
    return WOTBMOD_OK;
}
