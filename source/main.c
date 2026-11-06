#include <switch.h>

#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#define DEFAULT_NRO "sdmc:/switch/.overlays/ovlmenu.ovl"
#define HEAP_CONFIG_PATH "/config/nx-ovlloader/heap_size.bin"
#define EXIT_FLAG_PATH   "/config/nx-ovlloader/exit_flag.bin"
#define RELOAD_FLAG_PATH "/config/nx-ovlloader/reload_flag.bin"

const char g_noticeText[] =
    "nx-ovlloader " VERSION "\0"
    "What gives an idea power? Its origin? Its truth? Its reach? No. Its ability to collapse, reassemble, and still mean the same thing.";

static char g_argv[1024];
static char g_nextArgv[1024];
static char g_nextNroPath[512];
u64  g_nroAddr = 0;
static u64  g_nroSize = 0;
static NroHeader g_nroHeader;

static u64 g_appletHeapSize = 0;
static u64 g_appletHeapReservationSize = 0;

static u128 g_userIdStorage;

static u8 g_savedTls[0x100];

// Minimize fs resource usage
u32 __nx_fs_num_sessions = 1;
u32 __nx_fsdev_direntry_cache_size = 1;
bool __nx_fsdev_support_cwd = false;

// Used by trampoline.s
Result g_lastRet = 0;

extern void* __stack_top;
#define STACK_SIZE 0x10000

// Cache file system handle globally - performance improvement
static FsFileSystem g_sdmc;
static bool g_sdmc_initialized = false;

// Atomic loading flag to prevent concurrent loads
static _Atomic bool g_loading = false;

// Rotating address strategy: increment within 64GB-256GB range
// Provides predictable fast mapping while respecting 38-bit address space limit (256GB)
static u64 s_nextMapAddr = 0x1000000000ull;
static const u64 ADDR_LIMIT = 0x4000000000ull;  // 256GB limit

// Cache HOS version - firmware version doesn't change without reboot
static u64 s_hosVersion = 0;

#define M EntryFlag_IsMandatory

static ConfigEntry entries[] = {
    { EntryType_MainThreadHandle,     0, {0, 0} },
    { EntryType_ProcessHandle,        0, {0, 0} },
    { EntryType_AppletType,           0, {AppletType_LibraryApplet, 0} },
    { EntryType_OverrideHeap,         M, {0, 0} },
    { EntryType_Argv,                 0, {0, 0} },
    { EntryType_NextLoadPath,         0, {0, 0} },
    { EntryType_LastLoadResult,       0, {0, 0} },
    { EntryType_SyscallAvailableHint, 0, {0xffffffffffffffff, 0x9fc1fff0007ffff} },
    { EntryType_RandomSeed,           0, {0, 0} },
    { EntryType_UserIdStorage,        0, {(u64)(uintptr_t)&g_userIdStorage, 0} },
    { EntryType_HosVersion,           0, {0, 0} },
    { EntryType_EndOfList,            0, {(u64)(uintptr_t)g_noticeText, sizeof(g_noticeText)} }
};

void __libnx_initheap(void) {
    static char g_innerheap[0x8000];

    extern char* fake_heap_start;
    extern char* fake_heap_end;

    fake_heap_start = &g_innerheap[0];
    fake_heap_end   = &g_innerheap[sizeof g_innerheap];
}

// Fast inline heap size validator
static inline bool isValidHeapSize(u64 size) {
    constexpr u64 twoMB = 0x200000;
    return size != twoMB && size % twoMB == 0;
}

void __appInit(void) {
    Result rc;

    rc = smInitialize();
    if (R_FAILED(rc))
        fatalThrow(MAKERESULT(Module_HomebrewLoader, 1));

    rc = fsInitialize();
    if (R_FAILED(rc))
        fatalThrow(MAKERESULT(Module_HomebrewLoader, 2));

    // Read persistent heap size config early
    FsFileSystem tempSdmc;
    rc = fsOpenSdCardFileSystem(&tempSdmc);
    if (R_SUCCEEDED(rc)) {
        FsFile fd;
        rc = fsFsOpenFile(&tempSdmc, HEAP_CONFIG_PATH, FsOpenMode_Read, &fd);
        if (R_SUCCEEDED(rc)) {
            u64 bytes_read, savedHeapSize;
            rc = fsFileRead(&fd, 0, &savedHeapSize, sizeof(savedHeapSize), 
                           FsReadOption_None, &bytes_read);
            fsFileClose(&fd);
            
            if (R_SUCCEEDED(rc) && bytes_read == sizeof(savedHeapSize) && 
                isValidHeapSize(savedHeapSize)) {
                g_appletHeapSize = savedHeapSize;
            }
        }
        fsFsClose(&tempSdmc);
    }

    rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw;
        rc = setsysGetFirmwareVersion(&fw);
        if (R_SUCCEEDED(rc)) {
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
            s_hosVersion = hosversionGet();
        }
        
        // Default values if no valid config found
        if (g_appletHeapSize == 0) {
            if (hosversionAtLeast(23, 0, 1)) {
                g_appletHeapSize = 0x600000;  // 6MB for HOS 23.0.1+
            } else if (hosversionAtLeast(21, 0, 0)) {
                g_appletHeapSize = 0x400000;  // 4MB for HOS 21.0.0+
            } else if (hosversionAtLeast(20, 0, 0)) {
                g_appletHeapSize = 0x600000;  // 6MB for HOS 20.0.0+
            } else {
                g_appletHeapSize = 0x600000;  // 6MB for older versions
            }
        }
        
        g_appletHeapReservationSize = 0x00;
        setsysExit();
    }

    smExit();
}

void __wrap_exit(void) {
    // Open/init SD card if needed
    if (!g_sdmc_initialized) {
        Result rc = fsOpenSdCardFileSystem(&g_sdmc);
        if (R_SUCCEEDED(rc)) {
            g_sdmc_initialized = true;
        }
    }
    
    Result rc;
    
    // Reinitialize SM
    rc = smInitialize();
    
    if (R_FAILED(rc)) {
        if (g_sdmc_initialized) {
            fsFsClose(&g_sdmc);
            g_sdmc_initialized = false;
        }
        svcExitProcess();
        __builtin_unreachable();
    }
    
    // Try pmshell init
    rc = pmshellInitialize();
    
    if (R_SUCCEEDED(rc)) {
        NcmProgramLocation programLocation = {
            .program_id = 0x420000000007E51BULL,
            .storageID = NcmStorageId_None,
        };
        
        u64 pid = 0;
        rc = pmshellLaunchProgram(0, &programLocation, &pid);
        
        if (R_SUCCEEDED(rc) && pid != 0) {
            // CRITICAL: Give PM time to actually spawn the process
            // before we exit and clean up our handles
            svcSleepThread(500000000ULL); // 500ms
        }
        
        pmshellExit();
    }
    
    smExit();
    
    // Clean up
    if (g_sdmc_initialized) {
        fsFsClose(&g_sdmc);
        g_sdmc_initialized = false;
    }
    
    svcExitProcess();
    __builtin_unreachable();
}

static void*  g_heapAddr;
static size_t g_heapSize;

static void setupHbHeap(void) {
    void* addr = NULL;
    Result rc = svcSetHeapSize(&addr, g_appletHeapSize);
    
    if (R_FAILED(rc) || addr==NULL)
        fatalThrow(MAKERESULT(Module_HomebrewLoader, 9));
    
    g_heapAddr = addr;
    g_heapSize = g_appletHeapSize;
}

static Handle g_procHandle;

static void procHandleReceiveThread(void* arg) {
    Handle session = (Handle)(uintptr_t)arg;
    Result rc;

    void* base = armGetTls();
    hipcMakeRequestInline(base);

    s32 idx;
    rc = svcReplyAndReceive(&idx, &session, 1, INVALID_HANDLE, UINT64_MAX);
    if (R_FAILED(rc))
        fatalThrow(MAKERESULT(Module_HomebrewLoader, 15));

    HipcParsedRequest r = hipcParseRequest(base);
    if (r.meta.num_copy_handles != 1)
        fatalThrow(MAKERESULT(Module_HomebrewLoader, 17));

    g_procHandle = r.data.copy_handles[0];
    svcCloseHandle(session);
}

static void getOwnProcessHandle(void) {
    Result rc;

    Handle server_handle, client_handle;
    rc = svcCreateSession(&server_handle, &client_handle, 0, 0);
    if (R_FAILED(rc))
        fatalThrow(MAKERESULT(Module_HomebrewLoader, 12));

    Thread t;
    rc = threadCreate(&t, &procHandleReceiveThread, (void*)(uintptr_t)server_handle, NULL, 0x1000, 0x20, 0);
    if (R_FAILED(rc))
        fatalThrow(MAKERESULT(Module_HomebrewLoader, 10));

    rc = threadStart(&t);
    if (R_FAILED(rc))
        fatalThrow(MAKERESULT(Module_HomebrewLoader, 13));

    hipcMakeRequestInline(armGetTls(),
        .num_copy_handles = 1,
    ).copy_handles[0] = CUR_PROCESS_HANDLE;

    svcSendSyncRequest(client_handle);
    svcCloseHandle(client_handle);

    threadWaitForExit(&t);
    threadClose(&t);
}

// Fast exit check - called from trampoline between overlays
// Returns true if user requested complete exit
bool checkExitRequested(void) {
    // Early init check - fast path
    if (!g_sdmc_initialized) {
        Result rc = fsOpenSdCardFileSystem(&g_sdmc);
        if (R_FAILED(rc)) return false;
        g_sdmc_initialized = true;
    }
    
    FsFile fd;
    Result rc = fsFsOpenFile(&g_sdmc, EXIT_FLAG_PATH, FsOpenMode_Read, &fd);
    if (R_FAILED(rc)) {
        return false; // No flag file = don't exit
    }
    
    // Flag exists - delete it and signal exit
    fsFileClose(&fd);
    fsFsDeleteFile(&g_sdmc, EXIT_FLAG_PATH);
    
    return true; // Exit requested!
}

// Reload flag check - called from trampoline between overlays
// Returns true if a manual reload was requested (spawns nx-ovlreloader, same as heap change)
bool checkReloadRequested(void) {
    if (!g_sdmc_initialized) {
        Result rc = fsOpenSdCardFileSystem(&g_sdmc);
        if (R_FAILED(rc)) return false;
        g_sdmc_initialized = true;
    }

    FsFile fd;
    Result rc = fsFsOpenFile(&g_sdmc, RELOAD_FLAG_PATH, FsOpenMode_Read, &fd);
    if (R_FAILED(rc)) {
        return false; // No flag file = no reload
    }

    // Flag exists - delete it and signal reload
    fsFileClose(&fd);
    fsFsDeleteFile(&g_sdmc, RELOAD_FLAG_PATH);

    return true; // Reload requested!
}

// Fast heap change check - called from trampoline between overlays
// Returns true if loader needs to restart with new heap size
bool checkHeapSizeChange(void) {
    // Early init check - fast path
    if (!g_sdmc_initialized) {
        Result rc = fsOpenSdCardFileSystem(&g_sdmc);
        if (R_FAILED(rc)) return false;
        g_sdmc_initialized = true;
    }
    
    FsFile fd;
    Result rc = fsFsOpenFile(&g_sdmc, HEAP_CONFIG_PATH, FsOpenMode_Read, &fd);
    if (R_FAILED(rc)) {
        return false; // No config = no change
    }
    
    u64 bytes_read, configHeapSize;
    rc = fsFileRead(&fd, 0, &configHeapSize, sizeof(configHeapSize), 
                    FsReadOption_None, &bytes_read);
    fsFileClose(&fd);
    
    // Fast validation and comparison
    if (R_SUCCEEDED(rc) && bytes_read == sizeof(configHeapSize) && 
        isValidHeapSize(configHeapSize) && configHeapSize != g_appletHeapSize) {
        return true; // Heap size changed!
    }
    
    return false;
}

void loadNro(void) {
    // Atomically check-and-set loading flag to prevent concurrent loads
    if (__atomic_test_and_set(&g_loading, __ATOMIC_SEQ_CST)) {
        // Another loadNro() is already running, exit gracefully
        svcExitProcess();
        __builtin_unreachable();
    }

    NroHeader* header = NULL;
    size_t rw_size;
    Result rc;

    __builtin_memcpy((u8*)armGetTls() + 0x100, g_savedTls, 0x100);

    if (g_nroSize > 0) {
        // Unmap previous NRO
        header = &g_nroHeader;
        rw_size = (header->segments[2].size + header->bss_size + 0xFFF) & ~0xFFF;

        // .text
        rc = svcUnmapProcessCodeMemory(
            g_procHandle, g_nroAddr + header->segments[0].file_off, ((u64) g_heapAddr) + header->segments[0].file_off, header->segments[0].size);
        if (R_FAILED(rc))
            fatalThrow(MAKERESULT(Module_HomebrewLoader, 24));

        // .rodata
        rc = svcUnmapProcessCodeMemory(
            g_procHandle, g_nroAddr + header->segments[1].file_off, ((u64) g_heapAddr) + header->segments[1].file_off, header->segments[1].size);
        if (R_FAILED(rc))
            fatalThrow(MAKERESULT(Module_HomebrewLoader, 25));

        // .data + .bss
        rc = svcUnmapProcessCodeMemory(
            g_procHandle, g_nroAddr + header->segments[2].file_off, ((u64) g_heapAddr) + header->segments[2].file_off, rw_size);
        if (R_FAILED(rc))
            fatalThrow(MAKERESULT(Module_HomebrewLoader, 26));

        g_nroAddr = g_nroSize = 0;
    }

    if (!g_nextNroPath[0])
        __builtin_memcpy(g_nextNroPath, DEFAULT_NRO, sizeof(DEFAULT_NRO));

    if (!g_nextArgv[0])
        __builtin_memcpy(g_nextArgv,    DEFAULT_NRO, sizeof(DEFAULT_NRO));

    __builtin_memcpy(g_argv, g_nextArgv, sizeof g_argv);

    uint8_t *nrobuf = (uint8_t*) g_heapAddr;

    header = (NroHeader*) (nrobuf + sizeof(NroStart));
    
    // Cache filesystem handle instead of reopening every time
    if (!g_sdmc_initialized) {
        rc = fsOpenSdCardFileSystem(&g_sdmc);
        if (R_FAILED(rc))
            fatalThrow(MAKERESULT(Module_HomebrewLoader, 404));
        g_sdmc_initialized = true;
    }

    FsFile fd;
    rc = fsFsOpenFile(&g_sdmc, g_nextNroPath + 5, FsOpenMode_Read, &fd);
    if (R_FAILED(rc)) {
        // Clear flag before exiting on error
        __atomic_clear(&g_loading, __ATOMIC_SEQ_CST);
        exit(1);
    }

    // Reset NRO path to load hbmenu by default next time.
    g_nextNroPath[0] = '\0';

    // Read file in one go
    u64 bytes_read;
    rc = fsFileRead(&fd, 0, nrobuf, g_heapSize, FsReadOption_None, &bytes_read);
    fsFileClose(&fd);
    
    if (R_FAILED(rc) || bytes_read == 0)
        fatalThrow(MAKERESULT(Module_HomebrewLoader, 4));

    if (header->magic != NROHEADER_MAGIC)
        fatalThrow(MAKERESULT(Module_HomebrewLoader, 5));


    // Copy header to elsewhere because we're going to unmap it next.
    g_nroHeader = *header;
    header = &g_nroHeader;

    const size_t total_size = ((header->size + header->bss_size + 0xFFF) & ~0xFFF);

    rw_size = ((header->segments[2].size + header->bss_size + 0xFFF) & ~0xFFF);

    // Validate segments
    for (int i = 0; i < 3; i++) {
        const u32 offset = header->segments[i].file_off;
        const u32 size = header->segments[i].size;
        
        // Check 1: offset must be within file
        if (offset >= header->size) {
            fatalThrow(MAKERESULT(Module_HomebrewLoader, 6));
        }
        
        // Check 2: size must not exceed remaining space (overflow-safe)
        if (size > header->size - offset) {
            fatalThrow(MAKERESULT(Module_HomebrewLoader, 6));
        }
    }

    // Rotating window strategy: increment address within a fixed 512MB window
    // This gives us fresh addresses (fast) while preventing unbounded growth (no crash)
    u64 map_addr = s_nextMapAddr;
    
    rc = svcMapProcessCodeMemory(g_procHandle, map_addr, (u64)nrobuf, total_size);
    if (R_SUCCEEDED(rc)) {
        // Success! Advance to next address, wrapping within the window
        s_nextMapAddr = (map_addr + total_size + 0x4000000) & ~0x1FFFFFull;    // Align to 2MB
        
        // Wrap around if we exceed the window
        if (s_nextMapAddr >= ADDR_LIMIT) {
            s_nextMapAddr = 0x1000000000ull;  // Wrap
        }
    } else {
        // Mapping failed (window wrapped and old addresses not reclaimed yet)
        // Fall back to random search
        do {
            map_addr = randomGet64() & 0xFFFFFF000ull;
            rc = svcMapProcessCodeMemory(g_procHandle, map_addr, (u64)nrobuf, total_size);
        } while (R_FAILED(rc) && (rc == 0xDC01 || rc == 0xD401));
        
        if (R_FAILED(rc))
            fatalThrow(MAKERESULT(Module_HomebrewLoader, 18));
        
        // Don't update s_nextMapAddr - keep trying the window on next load
    }

    // .text
    rc = svcSetProcessMemoryPermission(
        g_procHandle, map_addr + header->segments[0].file_off, header->segments[0].size, Perm_R | Perm_X);
    if (R_FAILED(rc))
        fatalThrow(MAKERESULT(Module_HomebrewLoader, 19));

    // .rodata
    rc = svcSetProcessMemoryPermission(
        g_procHandle, map_addr + header->segments[1].file_off, header->segments[1].size, Perm_R);
    if (R_FAILED(rc))
        fatalThrow(MAKERESULT(Module_HomebrewLoader, 20));

    // .data + .bss
    rc = svcSetProcessMemoryPermission(
        g_procHandle, map_addr + header->segments[2].file_off, rw_size, Perm_Rw);
    if (R_FAILED(rc))
        fatalThrow(MAKERESULT(Module_HomebrewLoader, 21));

    const u64 nro_size = header->segments[2].file_off + rw_size;
    const u64 nro_heap_start = ((u64) g_heapAddr) + nro_size;

    // Fill entry values
    entries[0].Value[0] = envGetMainThreadHandle();
    entries[1].Value[0] = g_procHandle;
    entries[3].Value[0] = nro_heap_start;
    entries[3].Value[1] = g_heapSize + (u64) g_heapAddr - (u64) nro_heap_start;
    entries[4].Value[1] = (u64) &g_argv[0];
    entries[5].Value[0] = (u64) &g_nextNroPath[0];
    entries[5].Value[1] = (u64) &g_nextArgv[0];
    entries[6].Value[0] = g_lastRet;
    entries[8].Value[0] = randomGet64();
    entries[8].Value[1] = randomGet64();
    entries[10].Value[0] = s_hosVersion; // Use cached version

    g_nroAddr = map_addr;
    g_nroSize = nro_size;

    __builtin_memset(__stack_top - STACK_SIZE, 0, STACK_SIZE);

    // Clear the flag right before jumping to new overlay
    __atomic_clear(&g_loading, __ATOMIC_SEQ_CST);

    extern NX_NORETURN void nroEntrypointTrampoline(u64 entries_ptr, u64 handle, u64 entrypoint);
    nroEntrypointTrampoline((u64) entries, -1, map_addr);
}

int main(int argc, char **argv) {
    if (hosversionBefore(9,0,0))
        exit(1);

    __builtin_memcpy(g_savedTls, (u8*)armGetTls() + 0x100, 0x100);

    setupHbHeap();
    getOwnProcessHandle();
    loadNro();

    fatalThrow(MAKERESULT(Module_HomebrewLoader, 8));
    return 0;
}
