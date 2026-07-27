# WoT Blitz Mod API

Clean, UI-independent SDK and runtime core for a Geode-style native mod
loader. This project does not depend on or integrate with the old mod under
`proxy_dll`.

## What is included

- A versioned, C-compatible ABI in `include/wotb_mod_api.h`.
- A static runtime core that discovers and loads `mods\*.dll`.
- Lifecycle callbacks: load, enable, frame, disable, and unload.
- Per-mod logging, data directories, and isolated INI configuration.
- Optional host-owned hook operations through an architecture-supplied adapter.
- RVA, loaded-export, and byte-pattern lookup.
- Mod enumeration and enable/disable controls for a separate UI layer.
- Priority-based loose/DVPL resource mounts scoped to each enabled mod.
- Typed UI package/control, YAML, scene, texture, and generic resource loads
  through a loader-owned DAVA bridge.
- Automatic release of loaded resources and removal of mounts when a mod is
  disabled, faults, or unloads.
- Callback crash isolation: an SEH fault disables only the failing mod and
  removes hooks owned by it.
- A minimal example mod and a standalone smoke host.

There is no named-anchor registry. It was removed together with the aim/ESP/
wallhack build it was written for: a curated table of camera, entity, vehicle
and reload-timer addresses is a cheat toolkit regardless of which API serves
it.

`resolve_rva`, `find_pattern` and `hook_create` remain, because a native mod
loader cannot work without them — and they are enough to rebuild everything
that was removed. The review process, not the API surface, is what keeps
prohibited mods out.

## Build and verify

Run:

```bat
build.cmd
```

The command builds the x86 static runtime and example DLL, checks that the
public header compiles as C, then runs a standalone
lifecycle/ABI/fault-isolation smoke test. Outputs:

- `build\wotb_mod_runtime.lib`
- `build\hello_mod.dll`
- `build\smoke_host.exe`

## Embed the runtime in your loader

Link `wotb_mod_runtime.lib`, include `wotb_mod_runtime.h`, and initialize it
from a normal worker thread after the game module is mapped:

```cpp
WotbModRuntimeOptions options = {};
options.struct_size = sizeof(options);
options.game_directory = gameDirectory;
options.mods_directory = modsDirectory; // optional; defaults to <game>\mods
options.game_module = GetModuleHandleA(nullptr);
options.log_sink = MyLogSink;
options.hook_backend = &myHookAdapter;   // optional
options.resource_backend = &myDavaResources; // optional typed UI/scene bridge

WotbModRuntime_Initialize(&options);
WotbModRuntime_LoadAll();
```

Call `WotbModRuntime_DispatchFrame` from your render/frame integration point.
The API accepts opaque D3D pointers and does not draw or initialize any UI:

```cpp
WotbModRuntime_DispatchFrame(
    swapChain,
    device,
    immediateContext,
    backBufferWidth,
    backBufferHeight,
    deltaSeconds);
```

Call `WotbModRuntime_Shutdown` from a safe worker/control thread before your
loader unloads. Do not call it from `DllMain`; Windows loader-lock rules make
plugin callbacks and `FreeLibrary` unsafe there.

## Hook adapter boundary

The runtime intentionally does not choose MinHook, PolyHook, or another hook
engine. Your architecture supplies four operations:

```cpp
WotbModRuntimeHookBackend hooks = {};
hooks.struct_size = sizeof(hooks);
hooks.user_data = myHookEngine;
hooks.create = HookCreate;
hooks.enable = HookEnable;
hooks.disable = HookDisable;
hooks.remove = HookRemove;
```

The runtime tracks hook ownership per mod. A mod cannot enable, disable, or
remove another mod's hook through the API. Disabling, faulting, or unloading a
mod removes all hooks still owned by that mod.

## Client resources, UI packages, and custom models

The runtime contains the cross-mod overlay registry and the public stable ABI.
Your loader supplies the small DAVA-specific bridge:

```cpp
WotbModRuntimeResourceBackend resources = {};
resources.struct_size = sizeof(resources);
resources.user_data = myDavaBridge;
resources.load = LoadDavaResource;
resources.reload = ReloadDavaResource; // optional
resources.release = ReleaseDavaResource;
resources.registry_changed = OnResourceRegistryChanged; // optional
options.resource_backend = &resources;
```

The verified loader integration points are documented in
[`../re_anchors.md`](../re_anchors.md) under **Client resources**. The normal
file hook calls `WotbModRuntime_ResolveResourcePath(requestedPath, ...)`.
`WOTBMOD_ERROR_NOT_FOUND` means continue through the original DAVA resolver.
This covers loose files and `.dvpl` fallbacks without exposing DAVA C++ types
or raw game addresses to a mod DLL.

A mod mounts only a directory below its own data directory:

```cpp
WotbModResourceMountInfo mount = {};
mount.struct_size = sizeof(mount);
mount.virtual_root = "~res:/Mods/author.example/";
mount.source_directory = "resources";
mount.priority = 100;
mount.flags = WOTBMOD_RESOURCE_MOUNT_SEARCH_DVPL;

WotbModResourceMountId mountId = 0;
host->resource_mount(mod, &mount, &mountId);
```

With this layout:

```text
mods/data/<module-name>/resources/
  UI/MyScreen.yaml
  3d/MyModel.sc2
  3d/MyModel/texture.tex
```

the game paths are:

```text
~res:/Mods/author.example/UI/MyScreen.yaml
~res:/Mods/author.example/3d/MyModel.sc2
```

The same resolver handles `.yaml.dvpl`, `.sc2.dvpl`, referenced materials, and
textures. Explicit engine objects use `resource_load`; set the request type to
`WOTBMOD_RESOURCE_UI_PACKAGE`, `WOTBMOD_RESOURCE_UI_CONTROL`, or
`WOTBMOD_RESOURCE_SCENE`. `object_name` is used for a named UI control.
Returned handles are opaque and ownership-checked by the runtime.

## Writing a mod

Include `wotb_mod_api.h` and export exactly one entry point:

```cpp
WOTBMOD_ENTRY {
    if (!host || !out_info ||
        WOTBMOD_ABI_MAJOR(host->abi_version) !=
            WOTBMOD_ABI_MAJOR(WOTBMOD_ABI_VERSION)) {
        return WOTBMOD_ERROR_UNSUPPORTED_ABI;
    }

    *out_info = {};
    out_info->struct_size = sizeof(*out_info);
    out_info->abi_version = WOTBMOD_ABI_VERSION;
    out_info->id = "author.mod-id";
    out_info->name = "My Mod";
    out_info->version = "1.0.0";
    out_info->author = "Author";
    out_info->on_enable = OnEnable;
    out_info->on_disable = OnDisable;
    out_info->on_unload = OnUnload;
    out_info->on_frame = OnFrame;
    return WOTBMOD_OK;
}
```

See `examples\hello_mod\hello_mod.cpp` for logging, config, frame usage, a
resource mount, and a typed UI-package load.

## ABI rules

- Major ABI mismatch means incompatible.
- Public structs begin with `struct_size` and only grow by appending fields.
- Public data uses fixed-width integers, pointers, callbacks, and fixed buffers.
- Do not pass STL objects, C++ exceptions, or allocations across the ABI.
- `WotbModLoad` and initial `on_enable` run on the loader's worker thread.
- `on_frame` runs on the thread that calls `DispatchFrame`, normally render.
- ABI 2.1 has resource-level reload when the loader bridge supports it. DLL
  hot reload is not provided; replacing a DLL still requires a
  controlled runtime shutdown and reload.
