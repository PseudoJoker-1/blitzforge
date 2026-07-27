#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WOTBMOD_RUNTIME_BUILD
#include "../include/wotb_mod_runtime.h"

namespace {

const uint32_t kModRecordMagic = 0x4D425457u; /* WTBM */
const uint32_t kMaxMods = 128u;
const uint32_t kMaxHooksPerMod = 64u;
const uint32_t kMaxResourceMounts = 256u;
const uint32_t kMaxResourceMountsPerMod = 32u;
const uint32_t kMaxResourcesPerMod = 64u;

struct HookRecord {
    void* target;
    LONG active;
};

struct ModRecord;

struct ResourceMountRecord {
    ModRecord* owner;
    WotbModResourceMountId id;
    int32_t priority;
    uint32_t flags;
    LONG active;
    char virtual_root[WOTBMOD_MAX_RESOURCE_PATH];
    char source_directory[WOTBMOD_MAX_RESOURCE_PATH];
};

struct ResourceHandleRecord {
    void* native_resource;
    WotbModResourceType type;
    LONG active;
};

struct ModCallbacks {
    WotbModEnableCallback on_enable;
    WotbModDisableCallback on_disable;
    WotbModUnloadCallback on_unload;
    WotbModFrameCallback on_frame;
};

struct ModRecord {
    uint32_t magic;
    HMODULE module;
    char module_name[MAX_PATH];
    char module_path[MAX_PATH];
    char data_path[MAX_PATH];
    char config_path[MAX_PATH];
    char settings_key[MAX_PATH];
    WotbModPublicInfo public_info;
    ModCallbacks callbacks;
    HookRecord hooks[kMaxHooksPerMod];
    ResourceHandleRecord resources[kMaxResourcesPerMod];
    LONG hook_count;
    volatile LONG enabled;
    volatile LONG state;
    volatile LONG fault_count;
};

static ModRecord g_mods[kMaxMods] = {};
static volatile LONG g_modCount = 0;
static volatile LONG g_initialized = 0;
static volatile LONG g_loadedAll = 0;
static volatile LONG64 g_frameIndex = 0;
static HMODULE g_gameModule = nullptr;
static char g_gameDirectory[MAX_PATH] = {};
static char g_modsDirectory[MAX_PATH] = {};
static char g_dataDirectory[MAX_PATH] = {};
static char g_configDirectory[MAX_PATH] = {};
static char g_settingsPath[MAX_PATH] = {};
static LARGE_INTEGER g_qpcFrequency = {};
static LARGE_INTEGER g_lastFrameTime = {};
static WotbModRuntimeLogSink g_logSink = nullptr;
static void* g_logUserData = nullptr;
static WotbModRuntimeHookBackend g_hookBackend = {};
static WotbModRuntimeResourceBackend g_resourceBackend = {};
static ResourceMountRecord g_resourceMounts[kMaxResourceMounts] = {};
static SRWLOCK g_resourceMountLock = SRWLOCK_INIT;
static volatile LONG64 g_nextResourceMountId = 0;
static volatile LONG64 g_resourceGeneration = 0;

extern WotbModHostApi g_hostApi;

static void CopyString(char* destination, size_t capacity, const char* source) {
    if (!destination || capacity == 0) return;
    if (!source) source = "";
#if defined(_MSC_VER)
    strncpy_s(destination, capacity, source, _TRUNCATE);
#else
    strncpy(destination, source, capacity - 1);
    destination[capacity - 1] = '\0';
#endif
}

static bool JoinPath(
    char* destination,
    size_t capacity,
    const char* left,
    const char* right) {
    if (!destination || capacity == 0 || !left || !right) return false;
    const size_t leftLength = strlen(left);
    const char separator =
        leftLength > 0 && (left[leftLength - 1] == '\\' || left[leftLength - 1] == '/')
            ? '\0'
            : '\\';
    int written = 0;
    if (separator) {
        written = _snprintf_s(
            destination, capacity, _TRUNCATE, "%s\\%s", left, right);
    } else {
        written = _snprintf_s(
            destination, capacity, _TRUNCATE, "%s%s", left, right);
    }
    return written >= 0;
}

static bool NormalizeAbsolutePath(
    const char* input,
    char* output,
    size_t outputCapacity) {
    if (!input || !input[0] || !output || outputCapacity == 0) return false;
    DWORD length = GetFullPathNameA(
        input, (DWORD)outputCapacity, output, nullptr);
    if (length == 0 || length >= outputCapacity) return false;

    size_t current = strlen(output);
    while (current > 3 &&
           (output[current - 1] == '\\' || output[current - 1] == '/')) {
        output[--current] = '\0';
    }
    return true;
}

static bool DeriveExecutableDirectory(char* output, size_t outputCapacity) {
    if (!output || outputCapacity == 0) return false;
    DWORD length = GetModuleFileNameA(nullptr, output, (DWORD)outputCapacity);
    if (length == 0 || length >= outputCapacity) return false;
    char* slash = strrchr(output, '\\');
    if (!slash) slash = strrchr(output, '/');
    if (!slash) return false;
    *slash = '\0';
    return true;
}

static bool EnsureDirectory(const char* path) {
    if (!path || !path[0]) return false;
    if (CreateDirectoryA(path, nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

static const char* LogLevelName(WotbModLogLevel level) {
    switch (level) {
        case WOTBMOD_LOG_TRACE: return "trace";
        case WOTBMOD_LOG_WARNING: return "warning";
        case WOTBMOD_LOG_ERROR: return "error";
        case WOTBMOD_LOG_INFO:
        default: return "info";
    }
}

static void RuntimeLogV(
    WotbModLogLevel level,
    const char* format,
    va_list arguments) {
    char message[1400] = {};
    _vsnprintf_s(message, sizeof(message), _TRUNCATE, format, arguments);
    if (g_logSink) {
        g_logSink(level, message, g_logUserData);
        return;
    }

    char debugLine[1500] = {};
    _snprintf_s(
        debugLine,
        sizeof(debugLine),
        _TRUNCATE,
        "[wotb-mod-api/%s] %s\n",
        LogLevelName(level),
        message);
    OutputDebugStringA(debugLine);
}

static void RuntimeLog(WotbModLogLevel level, const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    RuntimeLogV(level, format, arguments);
    va_end(arguments);
}

static bool StartsWithInsensitive(const char* value, const char* prefix) {
    if (!value || !prefix) return false;
    const size_t prefixLength = strlen(prefix);
    return _strnicmp(value, prefix, prefixLength) == 0;
}

static bool CopyForwardSlashPath(
    const char* input,
    char* output,
    size_t outputCapacity) {
    if (!input || !output || outputCapacity == 0) return false;
    const size_t inputLength = strlen(input);
    if (inputLength >= outputCapacity) return false;
    for (size_t index = 0; index <= inputLength; ++index) {
        const char c = input[index];
        output[index] = c == '\\' ? '/' : c;
    }
    return true;
}

static bool IsSafeRelativeDirectory(const char* path) {
    if (!path || !path[0]) return false;
    if (path[0] == '\\' || path[0] == '/' || strchr(path, ':')) return false;

    const char* segment = path;
    while (*segment) {
        while (*segment == '\\' || *segment == '/') ++segment;
        const char* end = segment;
        while (*end && *end != '\\' && *end != '/') ++end;
        const size_t length = (size_t)(end - segment);
        if (length == 2 && segment[0] == '.' && segment[1] == '.') {
            return false;
        }
        segment = end;
    }
    return true;
}

static bool IsPathWithinDirectory(
    const char* directory,
    const char* candidate) {
    if (!directory || !candidate) return false;
    const size_t directoryLength = strlen(directory);
    if (_strnicmp(directory, candidate, directoryLength) != 0) return false;
    const char boundary = candidate[directoryLength];
    return boundary == '\0' || boundary == '\\' || boundary == '/';
}

static bool CanonicalizeResourcePath(
    const char* input,
    bool asRoot,
    char* output,
    size_t outputCapacity) {
    if (!input || !input[0] || !output || outputCapacity < 7) return false;

    char normalized[WOTBMOD_MAX_RESOURCE_PATH] = {};
    if (!CopyForwardSlashPath(input, normalized, sizeof(normalized))) {
        return false;
    }

    const char* relative = normalized;
    if (StartsWithInsensitive(relative, "~res:/")) {
        relative += 6;
    } else if (StartsWithInsensitive(relative, "res:/")) {
        relative += 5;
    } else if (StartsWithInsensitive(relative, "Data/")) {
        relative += 5;
    } else {
        char gameData[WOTBMOD_MAX_RESOURCE_PATH] = {};
        char gameDirectory[WOTBMOD_MAX_RESOURCE_PATH] = {};
        if (g_gameDirectory[0] &&
            CopyForwardSlashPath(
                g_gameDirectory, gameDirectory, sizeof(gameDirectory))) {
            const int written = _snprintf_s(
                gameData,
                sizeof(gameData),
                _TRUNCATE,
                "%s/Data/",
                gameDirectory);
            if (written >= 0 && StartsWithInsensitive(relative, gameData)) {
                relative += strlen(gameData);
            } else if (strchr(relative, ':')) {
                return false;
            }
        } else if (strchr(relative, ':')) {
            return false;
        }
    }

    while (*relative == '/') ++relative;
    CopyString(output, outputCapacity, "~res:/");
    size_t used = strlen(output);
    bool wroteSegment = false;

    while (*relative) {
        while (*relative == '/') ++relative;
        if (!*relative) break;
        const char* end = strchr(relative, '/');
        if (!end) end = relative + strlen(relative);
        const size_t segmentLength = (size_t)(end - relative);
        if ((segmentLength == 1 && relative[0] == '.') ||
            (segmentLength == 2 &&
             relative[0] == '.' &&
             relative[1] == '.') ||
            memchr(relative, ':', segmentLength)) {
            return false;
        }

        if (wroteSegment) {
            if (used + 1 >= outputCapacity) return false;
            output[used++] = '/';
        }
        if (used + segmentLength >= outputCapacity) return false;
        memcpy(output + used, relative, segmentLength);
        used += segmentLength;
        output[used] = '\0';
        wroteSegment = true;
        relative = end;
    }

    if (asRoot && output[used - 1] != '/') {
        if (used + 1 >= outputCapacity) return false;
        output[used++] = '/';
        output[used] = '\0';
    }
    return true;
}

static bool IsRegularFile(const char* path) {
    if (!path || !path[0]) return false;
    const DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static bool IsDirectory(const char* path) {
    if (!path || !path[0]) return false;
    const DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static void NotifyResourceRegistryChanged() {
    const uint64_t generation =
        (uint64_t)InterlockedIncrement64(&g_resourceGeneration);
    if (!g_resourceBackend.registry_changed) return;
    __try {
        g_resourceBackend.registry_changed(
            g_resourceBackend.user_data, generation);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "resource registry callback faulted with SEH 0x%08lX",
            GetExceptionCode());
    }
}

static ModRecord* RecordFromHandle(WotbModHandle handle) {
    if (!handle) return nullptr;
    ModRecord* record = static_cast<ModRecord*>(handle);
    if (record < &g_mods[0] || record >= &g_mods[kMaxMods]) return nullptr;
    return record->magic == kModRecordMagic ? record : nullptr;
}

static void SyncPublicState(ModRecord* record) {
    if (!record) return;
    record->public_info.state = (uint32_t)record->state;
    record->public_info.enabled = record->enabled ? 1u : 0u;
    record->public_info.fault_count = (uint32_t)record->fault_count;
}

static const char* RecordLogId(const ModRecord* record) {
    if (!record) return "host";
    if (record->public_info.id[0]) return record->public_info.id;
    if (record->settings_key[0]) return record->settings_key;
    return "unknown";
}

static void WOTBMOD_CALL HostLog(
    WotbModHandle handle,
    WotbModLogLevel level,
    const char* message) {
    ModRecord* record = RecordFromHandle(handle);
    RuntimeLog(
        level,
        "[%s] %s",
        RecordLogId(record),
        message ? message : "");
}

static WotbModResult CopyPathToCaller(
    const char* path,
    char* buffer,
    uint32_t* inoutSize) {
    if (!path || !inoutSize) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    const uint32_t required = (uint32_t)strlen(path) + 1u;
    const uint32_t capacity = *inoutSize;
    *inoutSize = required;
    if (!buffer || capacity < required) return WOTBMOD_ERROR_BUFFER_TOO_SMALL;
    memcpy(buffer, path, required);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL HostGetPath(
    WotbModHandle handle,
    WotbModPath path,
    char* buffer,
    uint32_t* inoutSize) {
    ModRecord* record = RecordFromHandle(handle);
    switch (path) {
        case WOTBMOD_PATH_GAME:
            return CopyPathToCaller(g_gameDirectory, buffer, inoutSize);
        case WOTBMOD_PATH_MODS:
            return CopyPathToCaller(g_modsDirectory, buffer, inoutSize);
        case WOTBMOD_PATH_MODULE:
            if (!record) return WOTBMOD_ERROR_INVALID_ARGUMENT;
            return CopyPathToCaller(record->module_path, buffer, inoutSize);
        case WOTBMOD_PATH_DATA:
            if (!record) return WOTBMOD_ERROR_INVALID_ARGUMENT;
            return CopyPathToCaller(record->data_path, buffer, inoutSize);
        case WOTBMOD_PATH_CONFIG:
            if (!record) return WOTBMOD_ERROR_INVALID_ARGUMENT;
            return CopyPathToCaller(record->config_path, buffer, inoutSize);
        default:
            return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
}

static size_t GetModuleImageSize(HMODULE module) {
    if (!module) return 0;
    __try {
        const IMAGE_DOS_HEADER* dos =
            reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            reinterpret_cast<const uint8_t*>(module) + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        return nt->OptionalHeader.SizeOfImage;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static void* WOTBMOD_CALL HostGetGameModule() {
    return g_gameModule;
}

static void* WOTBMOD_CALL HostResolveRva(uint32_t rva) {
    const size_t imageSize = GetModuleImageSize(g_gameModule);
    if (!g_gameModule || imageSize == 0 || rva >= imageSize) return nullptr;
    return reinterpret_cast<uint8_t*>(g_gameModule) + rva;
}

static void* WOTBMOD_CALL HostGetProcAddress(
    const char* loadedModuleName,
    const char* exportName) {
    if (!exportName || !exportName[0]) return nullptr;
    HMODULE module = loadedModuleName && loadedModuleName[0]
                         ? GetModuleHandleA(loadedModuleName)
                         : g_gameModule;
    if (!module) return nullptr;
    return reinterpret_cast<void*>(GetProcAddress(module, exportName));
}

static bool IsReadableProtection(DWORD protection) {
    if ((protection & PAGE_GUARD) || (protection & PAGE_NOACCESS)) return false;
    const DWORD baseProtection = protection & 0xFFu;
    return baseProtection == PAGE_READONLY ||
           baseProtection == PAGE_READWRITE ||
           baseProtection == PAGE_WRITECOPY ||
           baseProtection == PAGE_EXECUTE ||
           baseProtection == PAGE_EXECUTE_READ ||
           baseProtection == PAGE_EXECUTE_READWRITE ||
           baseProtection == PAGE_EXECUTE_WRITECOPY;
}

static bool PatternMatches(
    const uint8_t* candidate,
    const uint8_t* pattern,
    const char* mask,
    size_t length) {
    __try {
        for (size_t index = 0; index < length; ++index) {
            if (mask[index] == 'x' && candidate[index] != pattern[index]) {
                return false;
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void* WOTBMOD_CALL HostFindPattern(
    const char* loadedModuleName,
    const uint8_t* pattern,
    const char* mask) {
    if (!pattern || !mask || !mask[0]) return nullptr;
    HMODULE module = loadedModuleName && loadedModuleName[0]
                         ? GetModuleHandleA(loadedModuleName)
                         : g_gameModule;
    const size_t imageSize = GetModuleImageSize(module);
    const size_t patternLength = strlen(mask);
    if (!module || imageSize == 0 || patternLength > imageSize) return nullptr;

    uint8_t* imageStart = reinterpret_cast<uint8_t*>(module);
    uint8_t* imageEnd = imageStart + imageSize;
    uint8_t* cursor = imageStart;
    while (cursor < imageEnd) {
        MEMORY_BASIC_INFORMATION memory = {};
        if (!VirtualQuery(cursor, &memory, sizeof(memory))) break;

        uint8_t* regionStart =
            static_cast<uint8_t*>(memory.BaseAddress) > imageStart
                ? static_cast<uint8_t*>(memory.BaseAddress)
                : imageStart;
        uint8_t* rawRegionEnd =
            static_cast<uint8_t*>(memory.BaseAddress) + memory.RegionSize;
        uint8_t* regionEnd = rawRegionEnd < imageEnd ? rawRegionEnd : imageEnd;

        if (memory.State == MEM_COMMIT &&
            IsReadableProtection(memory.Protect) &&
            regionEnd > regionStart &&
            (size_t)(regionEnd - regionStart) >= patternLength) {
            uint8_t* last = regionEnd - patternLength;
            for (uint8_t* candidate = regionStart; candidate <= last; ++candidate) {
                if (PatternMatches(candidate, pattern, mask, patternLength)) {
                    return candidate;
                }
            }
        }

        if (rawRegionEnd <= cursor) break;
        cursor = rawRegionEnd;
    }
    return nullptr;
}

static int FindOwnedHook(const ModRecord* record, void* target) {
    if (!record || !target) return -1;
    const LONG count = record->hook_count;
    for (LONG index = 0; index < count && index < (LONG)kMaxHooksPerMod; ++index) {
        if (record->hooks[index].active &&
            record->hooks[index].target == target) {
            return (int)index;
        }
    }
    return -1;
}

static void RemoveOwnedHooks(ModRecord* record) {
    if (!record) return;
    for (LONG index = record->hook_count - 1; index >= 0; --index) {
        HookRecord* hook = &record->hooks[index];
        if (!hook->active || !hook->target) continue;
        if (g_hookBackend.disable) {
            g_hookBackend.disable(g_hookBackend.user_data, hook->target);
        }
        if (g_hookBackend.remove) {
            g_hookBackend.remove(g_hookBackend.user_data, hook->target);
        }
        hook->active = 0;
        hook->target = nullptr;
    }
    record->hook_count = 0;
}

static bool HookBackendReady() {
    return g_hookBackend.create &&
           g_hookBackend.enable &&
           g_hookBackend.disable &&
           g_hookBackend.remove;
}

static WotbModResult WOTBMOD_CALL HostHookCreate(
    WotbModHandle handle,
    void* target,
    void* detour,
    void** original) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record || !target || !detour || !original) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (!HookBackendReady()) return WOTBMOD_ERROR_PLATFORM;
    if (FindOwnedHook(record, target) >= 0) return WOTBMOD_ERROR_ALREADY_EXISTS;
    if (record->hook_count >= (LONG)kMaxHooksPerMod) {
        return WOTBMOD_ERROR_LIMIT_REACHED;
    }

    WotbModResult result = g_hookBackend.create(
        g_hookBackend.user_data, target, detour, original);
    if (result != WOTBMOD_OK) return result;

    HookRecord* hook = &record->hooks[record->hook_count++];
    hook->target = target;
    hook->active = 1;
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL HostHookEnable(
    WotbModHandle handle,
    void* target) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record || !target) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    if (!HookBackendReady()) return WOTBMOD_ERROR_PLATFORM;
    if (FindOwnedHook(record, target) < 0) return WOTBMOD_ERROR_ACCESS_DENIED;
    return g_hookBackend.enable(g_hookBackend.user_data, target);
}

static WotbModResult WOTBMOD_CALL HostHookDisable(
    WotbModHandle handle,
    void* target) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record || !target) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    if (!HookBackendReady()) return WOTBMOD_ERROR_PLATFORM;
    if (FindOwnedHook(record, target) < 0) return WOTBMOD_ERROR_ACCESS_DENIED;
    return g_hookBackend.disable(g_hookBackend.user_data, target);
}

static WotbModResult WOTBMOD_CALL HostHookRemove(
    WotbModHandle handle,
    void* target) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record || !target) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    if (!HookBackendReady()) return WOTBMOD_ERROR_PLATFORM;
    const int hookIndex = FindOwnedHook(record, target);
    if (hookIndex < 0) return WOTBMOD_ERROR_ACCESS_DENIED;

    WotbModResult result =
        g_hookBackend.remove(g_hookBackend.user_data, target);
    if (result == WOTBMOD_OK) {
        record->hooks[hookIndex].active = 0;
        record->hooks[hookIndex].target = nullptr;
    }
    return result;
}

static bool ResourceBackendReady() {
    return g_resourceBackend.load && g_resourceBackend.release;
}

static WotbModResult CallResourceBackendOperation(
    WotbModRuntimeResourceOperation operation,
    void* nativeResource) {
    if (!operation || !nativeResource) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    WotbModResult result = WOTBMOD_ERROR_PLATFORM;
    __try {
        result = operation(g_resourceBackend.user_data, nativeResource);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "resource backend operation faulted with SEH 0x%08lX",
            GetExceptionCode());
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    return result;
}

static bool BuildMountedCandidate(
    const ResourceMountRecord* mount,
    const char* canonicalPath,
    char* output,
    size_t outputCapacity) {
    if (!mount || !canonicalPath || !output || outputCapacity == 0) {
        return false;
    }
    const size_t rootLength = strlen(mount->virtual_root);
    if (_strnicmp(canonicalPath, mount->virtual_root, rootLength) != 0) {
        return false;
    }

    const char* suffix = canonicalPath + rootLength;
    if (!suffix[0]) return false;
    char relative[WOTBMOD_MAX_RESOURCE_PATH] = {};
    if (!CopyForwardSlashPath(suffix, relative, sizeof(relative))) return false;
    for (char* current = relative; *current; ++current) {
        if (*current == '/') *current = '\\';
    }

    const int written = _snprintf_s(
        output,
        outputCapacity,
        _TRUNCATE,
        "%s\\%s",
        mount->source_directory,
        relative);
    if (written < 0) return false;
    if (IsRegularFile(output)) return true;

    if ((mount->flags & WOTBMOD_RESOURCE_MOUNT_SEARCH_DVPL) == 0) {
        return false;
    }

    const size_t outputLength = strlen(output);
    if (outputLength >= 5 &&
        _stricmp(output + outputLength - 5, ".dvpl") == 0) {
        output[outputLength - 5] = '\0';
        return IsRegularFile(output);
    }
    if (outputLength + 5 >= outputCapacity) return false;
    strcat_s(output, outputCapacity, ".dvpl");
    return IsRegularFile(output);
}

static WotbModResult ResolveResourcePathInternal(
    const char* requestedPath,
    char* buffer,
    uint32_t* inoutSize) {
    if (!requestedPath || !inoutSize) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }

    char canonical[WOTBMOD_MAX_RESOURCE_PATH] = {};
    if (!CanonicalizeResourcePath(
            requestedPath, false, canonical, sizeof(canonical))) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }

    char bestPath[WOTBMOD_MAX_RESOURCE_PATH] = {};
    int32_t bestPriority = INT32_MIN;
    WotbModResourceMountId bestId = 0;
    AcquireSRWLockShared(&g_resourceMountLock);
    for (uint32_t index = 0; index < kMaxResourceMounts; ++index) {
        const ResourceMountRecord* mount = &g_resourceMounts[index];
        if (!mount->active ||
            !mount->owner ||
            !mount->owner->enabled ||
            mount->owner->state != WOTBMOD_STATE_ENABLED) {
            continue;
        }

        char candidate[WOTBMOD_MAX_RESOURCE_PATH] = {};
        if (!BuildMountedCandidate(
                mount, canonical, candidate, sizeof(candidate))) {
            continue;
        }
        if (mount->priority > bestPriority ||
            (mount->priority == bestPriority && mount->id > bestId)) {
            bestPriority = mount->priority;
            bestId = mount->id;
            CopyString(bestPath, sizeof(bestPath), candidate);
        }
    }
    ReleaseSRWLockShared(&g_resourceMountLock);

    return bestPath[0]
               ? CopyPathToCaller(bestPath, buffer, inoutSize)
               : WOTBMOD_ERROR_NOT_FOUND;
}

static WotbModResult WOTBMOD_CALL HostResourceMount(
    WotbModHandle handle,
    const WotbModResourceMountInfo* mountInfo,
    WotbModResourceMountId* outMountId) {
    ModRecord* record = RecordFromHandle(handle);
    const size_t requiredSize =
        offsetof(WotbModResourceMountInfo, flags) +
        sizeof(mountInfo->flags);
    if (!record ||
        !record->enabled ||
        !mountInfo ||
        mountInfo->struct_size < requiredSize ||
        !mountInfo->virtual_root ||
        !mountInfo->source_directory ||
        !outMountId ||
        (mountInfo->flags & ~WOTBMOD_RESOURCE_MOUNT_SEARCH_DVPL) != 0) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }

    char virtualRoot[WOTBMOD_MAX_RESOURCE_PATH] = {};
    if (!CanonicalizeResourcePath(
            mountInfo->virtual_root,
            true,
            virtualRoot,
            sizeof(virtualRoot)) ||
        !IsSafeRelativeDirectory(mountInfo->source_directory)) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }

    char joined[WOTBMOD_MAX_RESOURCE_PATH] = {};
    char sourceDirectory[WOTBMOD_MAX_RESOURCE_PATH] = {};
    if (!JoinPath(
            joined,
            sizeof(joined),
            record->data_path,
            mountInfo->source_directory) ||
        !NormalizeAbsolutePath(
            joined, sourceDirectory, sizeof(sourceDirectory)) ||
        !IsPathWithinDirectory(record->data_path, sourceDirectory)) {
        return WOTBMOD_ERROR_ACCESS_DENIED;
    }
    if (!IsDirectory(sourceDirectory)) return WOTBMOD_ERROR_NOT_FOUND;

    ResourceMountRecord* available = nullptr;
    uint32_t ownedCount = 0;
    AcquireSRWLockExclusive(&g_resourceMountLock);
    for (uint32_t index = 0; index < kMaxResourceMounts; ++index) {
        ResourceMountRecord* current = &g_resourceMounts[index];
        if (current->active && current->owner == record) ++ownedCount;
        if (!current->active && !available) available = current;
    }
    if (!available || ownedCount >= kMaxResourceMountsPerMod) {
        ReleaseSRWLockExclusive(&g_resourceMountLock);
        return WOTBMOD_ERROR_LIMIT_REACHED;
    }

    ZeroMemory(available, sizeof(*available));
    available->owner = record;
    available->id =
        (WotbModResourceMountId)InterlockedIncrement64(
            &g_nextResourceMountId);
    available->priority = mountInfo->priority;
    available->flags = mountInfo->flags;
    CopyString(
        available->virtual_root,
        sizeof(available->virtual_root),
        virtualRoot);
    CopyString(
        available->source_directory,
        sizeof(available->source_directory),
        sourceDirectory);
    InterlockedExchange(&available->active, 1);
    *outMountId = available->id;
    ReleaseSRWLockExclusive(&g_resourceMountLock);

    NotifyResourceRegistryChanged();
    RuntimeLog(
        WOTBMOD_LOG_INFO,
        "[%s] mounted %s -> %s (priority=%ld)",
        RecordLogId(record),
        virtualRoot,
        sourceDirectory,
        (long)mountInfo->priority);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL HostResourceUnmount(
    WotbModHandle handle,
    WotbModResourceMountId mountId) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record || mountId == 0) return WOTBMOD_ERROR_INVALID_ARGUMENT;

    bool removed = false;
    bool denied = false;
    AcquireSRWLockExclusive(&g_resourceMountLock);
    for (uint32_t index = 0; index < kMaxResourceMounts; ++index) {
        ResourceMountRecord* mount = &g_resourceMounts[index];
        if (!mount->active || mount->id != mountId) continue;
        if (mount->owner != record) {
            denied = true;
            break;
        }
        ZeroMemory(mount, sizeof(*mount));
        removed = true;
        break;
    }
    ReleaseSRWLockExclusive(&g_resourceMountLock);
    if (denied) return WOTBMOD_ERROR_ACCESS_DENIED;
    if (!removed) return WOTBMOD_ERROR_NOT_FOUND;
    NotifyResourceRegistryChanged();
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL HostResourceResolve(
    WotbModHandle handle,
    const char* virtualPath,
    char* buffer,
    uint32_t* inoutSize) {
    if (!RecordFromHandle(handle)) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    return ResolveResourcePathInternal(virtualPath, buffer, inoutSize);
}

static ResourceHandleRecord* ResourceFromHandle(
    ModRecord* record,
    WotbModResourceHandle handle) {
    if (!record || !handle) return nullptr;
    const uintptr_t value = reinterpret_cast<uintptr_t>(handle);
    const uintptr_t begin =
        reinterpret_cast<uintptr_t>(&record->resources[0]);
    const uintptr_t end =
        reinterpret_cast<uintptr_t>(&record->resources[kMaxResourcesPerMod]);
    if (value < begin ||
        value >= end ||
        ((value - begin) % sizeof(ResourceHandleRecord)) != 0) {
        return nullptr;
    }
    return reinterpret_cast<ResourceHandleRecord*>(value);
}

static WotbModResult WOTBMOD_CALL HostResourceLoad(
    WotbModHandle handle,
    const WotbModResourceLoadRequest* request,
    WotbModResourceHandle* outResource) {
    ModRecord* record = RecordFromHandle(handle);
    const size_t requiredSize =
        offsetof(WotbModResourceLoadRequest, flags) +
        sizeof(request->flags);
    if (!record ||
        !record->enabled ||
        !request ||
        request->struct_size < requiredSize ||
        !request->virtual_path ||
        !outResource ||
        request->type < WOTBMOD_RESOURCE_GENERIC ||
        request->type > WOTBMOD_RESOURCE_TEXTURE) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (!ResourceBackendReady()) return WOTBMOD_ERROR_PLATFORM;

    ResourceHandleRecord* slot = nullptr;
    for (uint32_t index = 0; index < kMaxResourcesPerMod; ++index) {
        if (InterlockedCompareExchange(
                &record->resources[index].active, -1, 0) == 0) {
            slot = &record->resources[index];
            break;
        }
    }
    if (!slot) return WOTBMOD_ERROR_LIMIT_REACHED;

    char canonical[WOTBMOD_MAX_RESOURCE_PATH] = {};
    if (!CanonicalizeResourcePath(
            request->virtual_path, false, canonical, sizeof(canonical))) {
        InterlockedExchange(&slot->active, 0);
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }

    WotbModResourceLoadRequest normalized = *request;
    normalized.struct_size = sizeof(normalized);
    normalized.virtual_path = canonical;
    void* nativeResource = nullptr;
    WotbModResult result = WOTBMOD_ERROR_PLATFORM;
    __try {
        result = g_resourceBackend.load(
            g_resourceBackend.user_data,
            &normalized,
            &nativeResource);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "[%s] resource load backend faulted with SEH 0x%08lX",
            RecordLogId(record),
            GetExceptionCode());
        result = WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    if (result != WOTBMOD_OK || !nativeResource) {
        slot->native_resource = nullptr;
        InterlockedExchange(&slot->active, 0);
        return result == WOTBMOD_OK ? WOTBMOD_ERROR_PLATFORM : result;
    }

    slot->native_resource = nativeResource;
    slot->type = request->type;
    InterlockedExchange(&slot->active, 1);
    *outResource = slot;
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL HostResourceReload(
    WotbModHandle handle,
    WotbModResourceHandle resourceHandle) {
    ModRecord* record = RecordFromHandle(handle);
    ResourceHandleRecord* resource =
        ResourceFromHandle(record, resourceHandle);
    if (!resource) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    if (resource->active != 1) return WOTBMOD_ERROR_NOT_FOUND;
    if (!g_resourceBackend.reload) return WOTBMOD_ERROR_PLATFORM;
    return CallResourceBackendOperation(
        g_resourceBackend.reload, resource->native_resource);
}

static WotbModResult WOTBMOD_CALL HostResourceRelease(
    WotbModHandle handle,
    WotbModResourceHandle resourceHandle) {
    ModRecord* record = RecordFromHandle(handle);
    ResourceHandleRecord* resource =
        ResourceFromHandle(record, resourceHandle);
    if (!resource) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    if (InterlockedCompareExchange(&resource->active, -1, 1) != 1) {
        return WOTBMOD_ERROR_NOT_FOUND;
    }

    void* nativeResource = resource->native_resource;
    resource->native_resource = nullptr;
    const WotbModResult result = CallResourceBackendOperation(
        g_resourceBackend.release, nativeResource);
    InterlockedExchange(&resource->active, 0);
    return result;
}

static void RemoveOwnedResources(ModRecord* record) {
    if (!record) return;
    for (uint32_t index = 0; index < kMaxResourcesPerMod; ++index) {
        ResourceHandleRecord* resource = &record->resources[index];
        if (InterlockedCompareExchange(&resource->active, -1, 1) != 1) {
            continue;
        }
        void* nativeResource = resource->native_resource;
        resource->native_resource = nullptr;
        if (nativeResource && g_resourceBackend.release) {
            CallResourceBackendOperation(
                g_resourceBackend.release, nativeResource);
        }
        InterlockedExchange(&resource->active, 0);
    }

    bool removedMount = false;
    AcquireSRWLockExclusive(&g_resourceMountLock);
    for (uint32_t index = 0; index < kMaxResourceMounts; ++index) {
        ResourceMountRecord* mount = &g_resourceMounts[index];
        if (mount->active && mount->owner == record) {
            ZeroMemory(mount, sizeof(*mount));
            removedMount = true;
        }
    }
    ReleaseSRWLockExclusive(&g_resourceMountLock);
    if (removedMount) NotifyResourceRegistryChanged();
}

static int32_t WOTBMOD_CALL HostConfigGetInt(
    WotbModHandle handle,
    const char* section,
    const char* key,
    int32_t defaultValue) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record || !section || !key) return defaultValue;
    return (int32_t)GetPrivateProfileIntA(
        section, key, defaultValue, record->config_path);
}

static WotbModResult WOTBMOD_CALL HostConfigSetInt(
    WotbModHandle handle,
    const char* section,
    const char* key,
    int32_t value) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record || !section || !key) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    char text[32] = {};
    _snprintf_s(text, sizeof(text), _TRUNCATE, "%ld", (long)value);
    return WritePrivateProfileStringA(
               section, key, text, record->config_path)
               ? WOTBMOD_OK
               : WOTBMOD_ERROR_PLATFORM;
}

static WotbModResult WOTBMOD_CALL HostConfigGetString(
    WotbModHandle handle,
    const char* section,
    const char* key,
    const char* defaultValue,
    char* buffer,
    uint32_t bufferSize) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record || !section || !key || !buffer || bufferSize == 0) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    DWORD copied = GetPrivateProfileStringA(
        section,
        key,
        defaultValue ? defaultValue : "",
        buffer,
        bufferSize,
        record->config_path);
    return copied < bufferSize - 1u
               ? WOTBMOD_OK
               : WOTBMOD_ERROR_BUFFER_TOO_SMALL;
}

static WotbModResult WOTBMOD_CALL HostConfigSetString(
    WotbModHandle handle,
    const char* section,
    const char* key,
    const char* value) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record || !section || !key || !value) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    return WritePrivateProfileStringA(
               section, key, value, record->config_path)
               ? WOTBMOD_OK
               : WOTBMOD_ERROR_PLATFORM;
}

static uint32_t WOTBMOD_CALL HostGetModCount() {
    LONG count = g_modCount;
    return count > 0 ? (uint32_t)count : 0u;
}

static WotbModResult WOTBMOD_CALL HostGetModInfo(
    uint32_t index,
    WotbModPublicInfo* outInfo) {
    const LONG count = g_modCount;
    if (!outInfo || index >= (uint32_t)count) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    const uint32_t callerSize = outInfo->struct_size;
    if (callerSize < sizeof(uint32_t)) return WOTBMOD_ERROR_INVALID_ARGUMENT;

    ModRecord* record = &g_mods[index];
    SyncPublicState(record);
    WotbModPublicInfo snapshot = record->public_info;
    snapshot.struct_size = sizeof(snapshot);
    const uint32_t copySize =
        callerSize < sizeof(snapshot) ? callerSize : (uint32_t)sizeof(snapshot);
    memcpy(outInfo, &snapshot, copySize);
    return callerSize < sizeof(snapshot)
               ? WOTBMOD_ERROR_BUFFER_TOO_SMALL
               : WOTBMOD_OK;
}

static void SaveEnabledState(const ModRecord* record, bool enabled) {
    if (!record || !record->settings_key[0]) return;
    WritePrivateProfileStringA(
        "mods",
        record->settings_key,
        enabled ? "1" : "0",
        g_settingsPath);
}

static void MarkFaulted(
    ModRecord* record,
    const char* callbackName,
    DWORD exceptionCode) {
    if (!record) return;
    InterlockedIncrement(&record->fault_count);
    InterlockedExchange(&record->enabled, 0);
    InterlockedExchange(&record->state, WOTBMOD_STATE_FAULTED);
    SyncPublicState(record);
    RuntimeLog(
        WOTBMOD_LOG_ERROR,
        "[%s] callback %s faulted with SEH 0x%08lX; mod disabled",
        RecordLogId(record),
        callbackName ? callbackName : "unknown",
        exceptionCode);
    RemoveOwnedHooks(record);
    RemoveOwnedResources(record);
}

static bool CallEnable(ModRecord* record) {
    if (!record || !record->callbacks.on_enable) return true;
    __try {
        record->callbacks.on_enable(&g_hostApi, record);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        MarkFaulted(record, "on_enable", GetExceptionCode());
        return false;
    }
}

static bool CallDisable(ModRecord* record) {
    if (!record || !record->callbacks.on_disable) return true;
    __try {
        record->callbacks.on_disable(&g_hostApi, record);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        MarkFaulted(record, "on_disable", GetExceptionCode());
        return false;
    }
}

static bool CallUnload(ModRecord* record) {
    if (!record || !record->callbacks.on_unload) return true;
    __try {
        record->callbacks.on_unload(&g_hostApi, record);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        MarkFaulted(record, "on_unload", GetExceptionCode());
        return false;
    }
}

static bool CallFrame(
    ModRecord* record,
    const WotbModFrameInfo* frame) {
    if (!record || !record->callbacks.on_frame) return true;
    __try {
        record->callbacks.on_frame(&g_hostApi, record, frame);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        MarkFaulted(record, "on_frame", GetExceptionCode());
        return false;
    }
}

static WotbModResult SetRecordEnabled(ModRecord* record, bool enabled) {
    if (!record) return WOTBMOD_ERROR_NOT_FOUND;
    if (record->state == WOTBMOD_STATE_FAULTED) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }

    if (enabled) {
        if (record->enabled) return WOTBMOD_OK;
        InterlockedExchange(&record->enabled, 1);
        InterlockedExchange(&record->state, WOTBMOD_STATE_ENABLED);
        SyncPublicState(record);
        if (!CallEnable(record)) return WOTBMOD_ERROR_CALLBACK_FAULT;
        SaveEnabledState(record, true);
        RuntimeLog(WOTBMOD_LOG_INFO, "[%s] enabled", RecordLogId(record));
        return WOTBMOD_OK;
    }

    if (!record->enabled) return WOTBMOD_OK;
    InterlockedExchange(&record->enabled, 0);
    InterlockedExchange(&record->state, WOTBMOD_STATE_DISABLED);
    SyncPublicState(record);
    const bool callbackOk = CallDisable(record);
    RemoveOwnedHooks(record);
    RemoveOwnedResources(record);
    if (record->state != WOTBMOD_STATE_FAULTED) {
        InterlockedExchange(&record->state, WOTBMOD_STATE_DISABLED);
        SyncPublicState(record);
    }
    SaveEnabledState(record, false);
    RuntimeLog(WOTBMOD_LOG_INFO, "[%s] disabled", RecordLogId(record));
    return callbackOk ? WOTBMOD_OK : WOTBMOD_ERROR_CALLBACK_FAULT;
}

static WotbModResult WOTBMOD_CALL HostSetModEnabled(
    const char* id,
    int32_t enabled) {
    if (!id || !id[0]) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    const LONG count = g_modCount;
    for (LONG index = 0; index < count; ++index) {
        if (_stricmp(g_mods[index].public_info.id, id) == 0) {
            return SetRecordEnabled(&g_mods[index], enabled != 0);
        }
    }
    return WOTBMOD_ERROR_NOT_FOUND;
}

WotbModHostApi g_hostApi = {
    sizeof(WotbModHostApi),
    WOTBMOD_ABI_VERSION,
    WOTBMOD_HOST_VERSION,
    0u,
    &HostLog,
    &HostGetPath,
    &HostGetGameModule,
    &HostResolveRva,
    &HostGetProcAddress,
    &HostFindPattern,
    &HostHookCreate,
    &HostHookEnable,
    &HostHookDisable,
    &HostHookRemove,
    &HostConfigGetInt,
    &HostConfigSetInt,
    &HostConfigGetString,
    &HostConfigSetString,
    &HostGetModCount,
    &HostGetModInfo,
    &HostSetModEnabled,
    &HostResourceMount,
    &HostResourceUnmount,
    &HostResourceResolve,
    &HostResourceLoad,
    &HostResourceReload,
    &HostResourceRelease};

static bool IsValidModId(const char* id) {
    if (!id || !id[0]) return false;
    const size_t length = strlen(id);
    if (length >= WOTBMOD_MAX_ID) return false;
    for (size_t index = 0; index < length; ++index) {
        const char c = id[index];
        const bool valid =
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '-';
        if (!valid) return false;
    }
    return true;
}

static bool IsDuplicateModId(const char* id) {
    const LONG count = g_modCount;
    for (LONG index = 0; index < count; ++index) {
        if (_stricmp(g_mods[index].public_info.id, id) == 0) return true;
    }
    return false;
}

static void GetFileStem(
    const char* fileName,
    char* output,
    size_t outputCapacity) {
    CopyString(output, outputCapacity, fileName);
    char* dot = strrchr(output, '.');
    if (dot) *dot = '\0';
}

static bool CallModEntry(
    WotbModLoadFn entry,
    ModRecord* record,
    WotbModInfo* outInfo,
    WotbModResult* outResult) {
    __try {
        *outResult = entry(&g_hostApi, record, outInfo);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "[%s] WotbModLoad faulted with SEH 0x%08lX",
            RecordLogId(record),
            GetExceptionCode());
        return false;
    }
}

static void ResetFailedRecord(ModRecord* record) {
    if (!record) return;
    RemoveOwnedHooks(record);
    RemoveOwnedResources(record);
    if (record->module) FreeLibrary(record->module);
    ZeroMemory(record, sizeof(*record));
}

static WotbModResult LoadOneMod(const char* path) {
    LONG slot = g_modCount;
    if (slot < 0 || slot >= (LONG)kMaxMods) {
        return WOTBMOD_ERROR_LIMIT_REACHED;
    }

    ModRecord* record = &g_mods[slot];
    ZeroMemory(record, sizeof(*record));
    record->magic = kModRecordMagic;
    record->state = WOTBMOD_STATE_DISCOVERED;
    CopyString(record->module_path, sizeof(record->module_path), path);
    const char* fileName = strrchr(path, '\\');
    fileName = fileName ? fileName + 1 : path;
    CopyString(record->module_name, sizeof(record->module_name), fileName);
    GetFileStem(
        fileName, record->settings_key, sizeof(record->settings_key));
    JoinPath(
        record->data_path,
        sizeof(record->data_path),
        g_dataDirectory,
        record->settings_key);
    EnsureDirectory(record->data_path);

    char configName[MAX_PATH] = {};
    _snprintf_s(
        configName,
        sizeof(configName),
        _TRUNCATE,
        "%s.ini",
        record->settings_key);
    JoinPath(
        record->config_path,
        sizeof(record->config_path),
        g_configDirectory,
        configName);

    record->module = LoadLibraryExA(
        path,
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
            LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!record->module && GetLastError() == ERROR_INVALID_PARAMETER) {
        record->module = LoadLibraryA(path);
    }
    if (!record->module) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "failed to load %s (win32=%lu)",
            path,
            GetLastError());
        ResetFailedRecord(record);
        return WOTBMOD_ERROR_PLATFORM;
    }

    WotbModLoadFn entry = reinterpret_cast<WotbModLoadFn>(
        GetProcAddress(record->module, WOTBMOD_ENTRY_NAME));
    if (!entry) {
        RuntimeLog(
            WOTBMOD_LOG_WARNING,
            "%s does not export %s; skipped",
            fileName,
            WOTBMOD_ENTRY_NAME);
        ResetFailedRecord(record);
        return WOTBMOD_ERROR_NOT_FOUND;
    }

    WotbModInfo info = {};
    info.struct_size = sizeof(info);
    info.abi_version = WOTBMOD_ABI_VERSION;
    WotbModResult entryResult = WOTBMOD_ERROR_PLATFORM;
    if (!CallModEntry(entry, record, &info, &entryResult)) {
        ResetFailedRecord(record);
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    if (entryResult != WOTBMOD_OK) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "[%s] WotbModLoad returned %d",
            RecordLogId(record),
            (int)entryResult);
        ResetFailedRecord(record);
        return entryResult;
    }

    const size_t requiredInfoSize =
        offsetof(WotbModInfo, on_frame) + sizeof(info.on_frame);
    if (info.struct_size < requiredInfoSize ||
        WOTBMOD_ABI_MAJOR(info.abi_version) !=
            WOTBMOD_ABI_MAJOR(WOTBMOD_ABI_VERSION)) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "%s uses unsupported ABI 0x%08X or descriptor size %u",
            fileName,
            info.abi_version,
            info.struct_size);
        ResetFailedRecord(record);
        return WOTBMOD_ERROR_UNSUPPORTED_ABI;
    }
    if (!IsValidModId(info.id) || IsDuplicateModId(info.id)) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "%s has invalid or duplicate mod id '%s'",
            fileName,
            info.id ? info.id : "");
        ResetFailedRecord(record);
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }

    record->public_info.struct_size = sizeof(record->public_info);
    record->public_info.abi_version = info.abi_version;
    record->public_info.flags = info.flags;
    CopyString(
        record->public_info.id,
        sizeof(record->public_info.id),
        info.id);
    CopyString(
        record->public_info.name,
        sizeof(record->public_info.name),
        info.name ? info.name : info.id);
    CopyString(
        record->public_info.version,
        sizeof(record->public_info.version),
        info.version ? info.version : "0.0.0");
    CopyString(
        record->public_info.author,
        sizeof(record->public_info.author),
        info.author ? info.author : "");
    CopyString(
        record->public_info.description,
        sizeof(record->public_info.description),
        info.description ? info.description : "");
    CopyString(
        record->public_info.module_path,
        sizeof(record->public_info.module_path),
        path);
    record->callbacks.on_enable = info.on_enable;
    record->callbacks.on_disable = info.on_disable;
    record->callbacks.on_unload = info.on_unload;
    record->callbacks.on_frame = info.on_frame;
    InterlockedExchange(&record->state, WOTBMOD_STATE_LOADED);

    const bool enabledByDefault =
        GetPrivateProfileIntA(
            "mods", record->settings_key, 1, g_settingsPath) != 0;
    InterlockedIncrement(&g_modCount);
    if (enabledByDefault) {
        WotbModResult enableResult = SetRecordEnabled(record, true);
        if (enableResult != WOTBMOD_OK) return enableResult;
    } else {
        InterlockedExchange(&record->enabled, 0);
        InterlockedExchange(&record->state, WOTBMOD_STATE_DISABLED);
        RemoveOwnedHooks(record);
        SyncPublicState(record);
    }

    RuntimeLog(
        WOTBMOD_LOG_INFO,
        "[%s] loaded %s v%s by %s",
        record->public_info.id,
        record->public_info.name,
        record->public_info.version,
        record->public_info.author[0] ? record->public_info.author : "unknown");
    return WOTBMOD_OK;
}

static int __cdecl ComparePaths(const void* left, const void* right) {
    return _stricmp(
        static_cast<const char*>(left),
        static_cast<const char*>(right));
}

static double ResolveDeltaSeconds(double suppliedDeltaSeconds) {
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    if (suppliedDeltaSeconds > 0.0) {
        g_lastFrameTime = now;
        return suppliedDeltaSeconds;
    }
    if (g_lastFrameTime.QuadPart == 0 || g_qpcFrequency.QuadPart == 0) {
        g_lastFrameTime = now;
        return 0.0;
    }
    const double delta =
        (double)(now.QuadPart - g_lastFrameTime.QuadPart) /
        (double)g_qpcFrequency.QuadPart;
    g_lastFrameTime = now;
    return delta;
}

} /* namespace */

extern "C" WotbModResult WOTBMOD_CALL WotbModRuntime_Initialize(
    const WotbModRuntimeOptions* options) {
    if (!options ||
        options->struct_size <
            offsetof(WotbModRuntimeOptions, hook_backend) +
                sizeof(options->hook_backend)) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (InterlockedCompareExchange(&g_initialized, 1, 0) != 0) {
        return WOTBMOD_ERROR_ALREADY_EXISTS;
    }

    g_logSink = options->log_sink;
    g_logUserData = options->log_user_data;
    g_gameModule = static_cast<HMODULE>(
        options->game_module ? options->game_module : GetModuleHandleA(nullptr));

    if (options->game_directory && options->game_directory[0]) {
        if (!NormalizeAbsolutePath(
                options->game_directory,
                g_gameDirectory,
                sizeof(g_gameDirectory))) {
            InterlockedExchange(&g_initialized, 0);
            return WOTBMOD_ERROR_INVALID_ARGUMENT;
        }
    } else if (!DeriveExecutableDirectory(
                   g_gameDirectory, sizeof(g_gameDirectory))) {
        InterlockedExchange(&g_initialized, 0);
        return WOTBMOD_ERROR_PLATFORM;
    }

    if (options->mods_directory && options->mods_directory[0]) {
        if (!NormalizeAbsolutePath(
                options->mods_directory,
                g_modsDirectory,
                sizeof(g_modsDirectory))) {
            InterlockedExchange(&g_initialized, 0);
            return WOTBMOD_ERROR_INVALID_ARGUMENT;
        }
    } else if (!JoinPath(
                   g_modsDirectory,
                   sizeof(g_modsDirectory),
                   g_gameDirectory,
                   "mods")) {
        InterlockedExchange(&g_initialized, 0);
        return WOTBMOD_ERROR_PLATFORM;
    }

    if (!EnsureDirectory(g_modsDirectory) ||
        !JoinPath(
            g_dataDirectory,
            sizeof(g_dataDirectory),
            g_modsDirectory,
            "data") ||
        !EnsureDirectory(g_dataDirectory) ||
        !JoinPath(
            g_configDirectory,
            sizeof(g_configDirectory),
            g_modsDirectory,
            "config") ||
        !EnsureDirectory(g_configDirectory) ||
        !JoinPath(
            g_settingsPath,
            sizeof(g_settingsPath),
            g_modsDirectory,
            "mods.ini")) {
        InterlockedExchange(&g_initialized, 0);
        return WOTBMOD_ERROR_PLATFORM;
    }

    ZeroMemory(&g_hookBackend, sizeof(g_hookBackend));
    if (options->hook_backend &&
        options->hook_backend->struct_size >= sizeof(g_hookBackend)) {
        g_hookBackend = *options->hook_backend;
    }
    ZeroMemory(&g_resourceBackend, sizeof(g_resourceBackend));
    const size_t resourceBackendEnd =
        offsetof(WotbModRuntimeOptions, resource_backend) +
        sizeof(options->resource_backend);
    if (options->struct_size >= resourceBackendEnd &&
        options->resource_backend &&
        options->resource_backend->struct_size >=
            sizeof(g_resourceBackend)) {
        g_resourceBackend = *options->resource_backend;
    }
    QueryPerformanceFrequency(&g_qpcFrequency);
    QueryPerformanceCounter(&g_lastFrameTime);
    RuntimeLog(
        WOTBMOD_LOG_INFO,
        "runtime initialized game=%s mods=%s hooks=%s resource-load=%s",
        g_gameDirectory,
        g_modsDirectory,
        HookBackendReady() ? "available" : "unavailable",
        ResourceBackendReady() ? "available" : "unavailable");
    return WOTBMOD_OK;
}

extern "C" WotbModResult WOTBMOD_CALL WotbModRuntime_LoadAll() {
    if (!g_initialized) return WOTBMOD_ERROR_DISABLED;
    if (InterlockedCompareExchange(&g_loadedAll, 1, 0) != 0) {
        return WOTBMOD_ERROR_ALREADY_EXISTS;
    }

    char searchPath[MAX_PATH] = {};
    if (!JoinPath(
            searchPath, sizeof(searchPath), g_modsDirectory, "*.dll")) {
        return WOTBMOD_ERROR_PLATFORM;
    }

    char paths[kMaxMods][MAX_PATH] = {};
    uint32_t pathCount = 0;
    WIN32_FIND_DATAA findData = {};
    HANDLE find = FindFirstFileA(searchPath, &findData);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                continue;
            }
            if (pathCount >= kMaxMods) {
                RuntimeLog(
                    WOTBMOD_LOG_WARNING,
                    "mod limit reached; remaining DLLs were skipped");
                break;
            }
            if (JoinPath(
                    paths[pathCount],
                    sizeof(paths[pathCount]),
                    g_modsDirectory,
                    findData.cFileName)) {
                ++pathCount;
            }
        } while (FindNextFileA(find, &findData));
        FindClose(find);
    } else if (GetLastError() != ERROR_FILE_NOT_FOUND) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "failed to enumerate %s (win32=%lu)",
            searchPath,
            GetLastError());
        return WOTBMOD_ERROR_PLATFORM;
    }

    qsort(paths, pathCount, sizeof(paths[0]), &ComparePaths);
    uint32_t failures = 0;
    for (uint32_t index = 0; index < pathCount; ++index) {
        if (LoadOneMod(paths[index]) != WOTBMOD_OK) ++failures;
    }
    RuntimeLog(
        failures ? WOTBMOD_LOG_WARNING : WOTBMOD_LOG_INFO,
        "load complete discovered=%u loaded=%u failed=%u",
        pathCount,
        (uint32_t)g_modCount,
        failures);
    return failures ? WOTBMOD_ERROR_PLATFORM : WOTBMOD_OK;
}

extern "C" void WOTBMOD_CALL WotbModRuntime_DispatchFrame(
    void* swapChain,
    void* device,
    void* deviceContext,
    uint32_t backBufferWidth,
    uint32_t backBufferHeight,
    double deltaSeconds) {
    if (!g_initialized) return;

    WotbModFrameInfo frame = {};
    frame.struct_size = sizeof(frame);
    frame.frame_index =
        (uint64_t)InterlockedIncrement64(&g_frameIndex);
    frame.delta_seconds = ResolveDeltaSeconds(deltaSeconds);
    frame.swap_chain = swapChain;
    frame.device = device;
    frame.device_context = deviceContext;
    frame.back_buffer_width = backBufferWidth;
    frame.back_buffer_height = backBufferHeight;

    const LONG count = g_modCount;
    for (LONG index = 0; index < count; ++index) {
        ModRecord* record = &g_mods[index];
        if (record->enabled &&
            record->state == WOTBMOD_STATE_ENABLED) {
            CallFrame(record, &frame);
        }
    }
}

extern "C" void WOTBMOD_CALL WotbModRuntime_Shutdown() {
    if (InterlockedCompareExchange(&g_initialized, 0, 1) != 1) return;

    const LONG count = g_modCount;
    for (LONG index = count - 1; index >= 0; --index) {
        ModRecord* record = &g_mods[index];
        if (record->enabled) {
            InterlockedExchange(&record->enabled, 0);
            CallDisable(record);
        }
        RemoveOwnedHooks(record);
        RemoveOwnedResources(record);
        CallUnload(record);
        HMODULE module = record->module;
        record->module = nullptr;
        record->magic = 0;
        if (module) FreeLibrary(module);
    }

    ZeroMemory(g_mods, sizeof(g_mods));
    InterlockedExchange(&g_modCount, 0);
    InterlockedExchange(&g_loadedAll, 0);
    InterlockedExchange64(&g_frameIndex, 0);
    g_gameModule = nullptr;
    g_logSink = nullptr;
    g_logUserData = nullptr;
    ZeroMemory(&g_hookBackend, sizeof(g_hookBackend));
    ZeroMemory(&g_resourceBackend, sizeof(g_resourceBackend));
    ZeroMemory(g_resourceMounts, sizeof(g_resourceMounts));
    InterlockedExchange64(&g_nextResourceMountId, 0);
    InterlockedExchange64(&g_resourceGeneration, 0);
}

extern "C" const WotbModHostApi* WOTBMOD_CALL
WotbModRuntime_GetHostApi() {
    return &g_hostApi;
}

extern "C" WotbModResult WOTBMOD_CALL
WotbModRuntime_ResolveResourcePath(
    const char* requestedPath,
    char* buffer,
    uint32_t* inoutSize) {
    if (!g_initialized) return WOTBMOD_ERROR_DISABLED;
    return ResolveResourcePathInternal(requestedPath, buffer, inoutSize);
}

extern "C" uint64_t WOTBMOD_CALL
WotbModRuntime_GetResourceGeneration() {
    return (uint64_t)InterlockedCompareExchange64(
        &g_resourceGeneration, 0, 0);
}

extern "C" const WotbModHostApi* WOTBMOD_CALL
WotbModApi_GetHost() {
    return &g_hostApi;
}

extern "C" uint32_t WOTBMOD_CALL
WotbModApi_GetVersion() {
    return WOTBMOD_ABI_VERSION;
}
