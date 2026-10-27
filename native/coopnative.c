// CoopNative: native (C) hooks for the Code Vein co-op project, loaded by Ultimate ASI Loader (winmm.dll proxy).
// Probe stage: inline MinHook detours. GetRescueSignalState (exec thunk) logs state changes; the session state-machine
// handler logs every internal event with a backtrace and can veto event 0x1F (only when explicitly enabled, see below).
// A command file lets Lua ask for memory reads (vtable lookups) that UE4SS Lua cannot do itself.
// The exe has no ASLR (DYNAMIC_BASE off), so UFunction exec thunk addresses from the UE4SS object dump are fixed.
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <share.h>
#include <string.h>
#include "MinHook.h"

#define ADDR_EXEC_GET_RESCUE_SIGNAL_STATE 0x140A04EC0ULL

static FILE *g_log;
static CRITICAL_SECTION g_log_lock;

static void logf_(const char *fmt, ...) {
    SYSTEMTIME t; GetLocalTime(&t);
    EnterCriticalSection(&g_log_lock);
    if (g_log) {
        fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
        fputc('\n', g_log); fflush(g_log);
    }
    LeaveCriticalSection(&g_log_lock);
}

// UE 4.18 exec thunk: static void execX(UObject* Context, FFrame& Stack, void* const Result)
typedef void (*exec_fn)(void *context, void *stack, void *result);
static exec_fn g_orig_get_state;
static volatile LONG g_calls;
static volatile LONG g_last_state = -1;

static void detour_get_state(void *context, void *stack, void *result) {
    g_orig_get_state(context, stack, result);
    InterlockedIncrement(&g_calls);
    if (result) InterlockedExchange(&g_last_state, *(uint8_t *)result);
}

// UAppSessionManager state-machine handler (body moved by Denuvo; original-location stub 0x140839A30).
// It maps an internal session event code (0..0x22) to ERescueSignalState at this+0xE08; events 0x1F and 0x20
// set 4 = EndOfCooperativeMode (Ghidra, 2026-10-26). Called indirectly (no direct calls to the stub).
#define ADDR_SESSION_STATE_HANDLER 0x146EA3AC0ULL
#define OFF_RESCUE_SIGNAL_STATE 0xE08
typedef void (*state_fn)(uint8_t *self, uint32_t a2, uint32_t event);
static state_fn g_orig_state_handler;

// The handler above is only the "on enter state" notification (veto there did not hold, 2-PC test 2026-10-26): the state
// machine stores the new internal state at this+0xD90 before notifying. It only logs now.
static void detour_state_handler(uint8_t *self, uint32_t a2, uint32_t event) {
    void *caller = __builtin_return_address(0);
    void *frames[12];
    USHORT n = RtlCaptureStackBackTrace(1, 12, frames, NULL);
    uint8_t before = self[OFF_RESCUE_SIGNAL_STATE];
    g_orig_state_handler(self, a2, event);
    char bt[12 * 20] = "";
    for (USHORT i = 0; i < n; i++) {
        char one[20];
        snprintf(one, sizeof one, " %llX", (unsigned long long)(uintptr_t)frames[i]);
        strcat_s(bt, sizeof bt, one);
    }
    logf_("SESSION EVENT 0x%02X (a2=%u) state %u -> %u, caller %llX, backtrace:%s",
          event, a2, before, self[OFF_RESCUE_SIGNAL_STATE], (unsigned long long)(uintptr_t)caller, bt);
}

// Veto experiment (off by default): AppSessionManager "EndCooperativeMode" (stub 0x140832D70, body 0x146E92EF0; only direct
// caller 0x146966279, a player-death handler). With no other players it switches the internal state (+0xD90) to 0x1F at once;
// otherwise it sets the request flag +0xDC0, which 0x14081C940 turns into 0x1F shortly after. Skipping the whole function while
// CoopNative.ini has veto_endcoop=1 AND CoopNative.flags reports a spectate window (boss fight, one player dead, one alive)
// keeps both the request and the state change from happening. Needed on host and guest (each ends co-op on its own side).
#define ADDR_END_COOP_BODY 0x146E92EF0ULL
typedef void (*endcoop_fn)(void *sm);
static endcoop_fn g_orig_end_coop;
static volatile LONG g_veto_enabled, g_spectate_window;

static void detour_end_coop(void *sm) {
    uintptr_t caller = (uintptr_t)__builtin_return_address(0);
    if (g_veto_enabled && g_spectate_window) {
        logf_("EndCooperativeMode from 0x%llX  *** VETOED (spectate window) ***", (unsigned long long)caller);
        return;
    }
    logf_("EndCooperativeMode from 0x%llX (allowed)", (unsigned long long)caller);
    g_orig_end_coop(sm);
}

// R1 (rest at a Mistle in co-op), off unless CoopNative.ini has rest_in_coop=1. The Mistle's IsActionButtonActive override
// (vtable 0x142D09658 +0x660 -> stub 0x1406AF330 -> body 0x1469781B0) greys the prompt out when IsOnlineMultiplay() is true
// (Ghidra 2026-10-26). We answer "false" to IsOnlineMultiplay only for calls coming from inside that function.
#define ADDR_IS_ONLINE_MULTIPLAY_BODY 0x146E7BC90ULL   // impl stub 0x140828C80
#define MISTLE_PROMPT_BEGIN 0x1469781B0ULL
#define MISTLE_PROMPT_END   0x1469782E0ULL               // past the last return seen in the decompile
typedef uint8_t (*multiplay_fn)(void *ctx);
static multiplay_fn g_orig_multiplay;
static volatile LONG g_rest_in_coop, g_rest_overrides;

// The rest action: the Mistle's ProcessInputButtonEvent (vtable +0x618 -> stub 0x1406BB510 -> body 0x146A1CB70) starts with
// "if ((state != Opened || !MultiPlayersCheck()) && pressed) { rest }" (Ghidra 2026-10-26); the call is near the start.
#define MISTLE_INPUT_BEGIN 0x146A1CB70ULL
#define MISTLE_INPUT_END   0x146A1CC00ULL
static int from_mistle_prompt(uintptr_t caller) {
    return (caller >= MISTLE_PROMPT_BEGIN && caller < MISTLE_PROMPT_END) ||
           (caller >= MISTLE_INPUT_BEGIN && caller < MISTLE_INPUT_END);
}

// While rest_in_coop is on, log every distinct caller (outside the prompt) that gets "true" from one of the three
// co-op checks, so the check behind the rest action itself shows up when the player presses E.
#define MAX_CALLERS 64
static volatile LONG64 g_seen[3][MAX_CALLERS];
static void note_caller(int which, uintptr_t caller) {
    static const char *names[3] = {"IsOnlineMultiplay", "MultiPlayersCheck", "PromptBlock1"};
    for (int i = 0; i < MAX_CALLERS; i++) {
        LONG64 v = g_seen[which][i];
        if (v == (LONG64)caller) return;
        if (v == 0 && InterlockedCompareExchange64(&g_seen[which][i], (LONG64)caller, 0) == 0) {
            logf_("new caller of %s returning true: 0x%llX", names[which], (unsigned long long)caller);
            return;
        }
    }
}

static uint8_t detour_multiplay(void *ctx) {
    uint8_t r = g_orig_multiplay(ctx);
    uintptr_t caller = (uintptr_t)__builtin_return_address(0);
    if (r && g_rest_in_coop) {
        if (from_mistle_prompt(caller)) { InterlockedIncrement(&g_rest_overrides); return 0; }
        note_caller(0, caller);
    }
    return r;
}

// Two more co-op checks in the same prompt (2-PC test 2026-10-26: overriding IsOnlineMultiplay alone was not enough):
// 0x140828970 (body 0x146E7AEF0) returns true when a counter difference on some game object is > 1 or another count > 0
// (reads like "more than one player"); the prompt calls it at the end, and through 0x140828AC0 (body 0x146E7B3C0:
// "0x140828970() && !0x1408285D0(x)") at the start. Both answer false only for calls from the Mistle prompt.
#define ADDR_MULTI_PLAYERS_BODY 0x146E7AEF0ULL
#define ADDR_PROMPT_BLOCK1_BODY 0x146E7B3C0ULL
typedef uint8_t (*check_fn)(void *a);
static check_fn g_orig_multi_players, g_orig_prompt_block1;

static uint8_t detour_multi_players(void *a) {
    uint8_t r = g_orig_multi_players(a);
    uintptr_t caller = (uintptr_t)__builtin_return_address(0);
    if (r && g_rest_in_coop) {
        if (from_mistle_prompt(caller)) { InterlockedIncrement(&g_rest_overrides); return 0; }
        note_caller(1, caller);
    }
    return r;
}

static uint8_t detour_prompt_block1(void *a) {
    uint8_t r = g_orig_prompt_block1(a);
    uintptr_t caller = (uintptr_t)__builtin_return_address(0);
    if (r && g_rest_in_coop) {
        if (from_mistle_prompt(caller)) { InterlockedIncrement(&g_rest_overrides); return 0; }
        note_caller(2, caller);
    }
    return r;
}

// ---- small file channel next to the DLL: CoopNative.cmd (one-shot commands), .ini (settings), .flags (from Lua) ----
static char g_dir[MAX_PATH];

static int read_small_file(const char *name, char *buf, size_t size) {
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s%s", g_dir, name);
    FILE *f = _fsopen(path, "r", _SH_DENYNO);
    if (!f) return 0;
    size_t n = fread(buf, 1, size - 1, f);
    fclose(f);
    buf[n] = 0;
    return 1;
}

static int readable(uintptr_t a, size_t len) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!a || !VirtualQuery((void *)a, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return a + len <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

static void log_code_at(const char *what, uintptr_t fn) {
    if (!readable(fn, 8)) { logf_("%s 0x%llX: not readable", what, (unsigned long long)fn); return; }
    const uint8_t *p = (const uint8_t *)fn;
    uintptr_t target = p[0] == 0xE9 ? fn + 5 + *(const int32_t *)(p + 1) : 0;
    logf_("%s 0x%llX: bytes %02X %02X %02X %02X %02X %02X %02X %02X%s%llX", what, (unsigned long long)fn,
          p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], target ? "  (jmp -> 0x" : "", (unsigned long long)target);
}

// "vfunc <object hex> <offset hex>": object's vtable and the entry at offset;  "peek <addr hex> <len>": hex dump (<=64)
static void run_commands(void) {
    char buf[256];
    if (!read_small_file("CoopNative.cmd", buf, sizeof buf)) return;
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%sCoopNative.cmd", g_dir);
    DeleteFileA(path);
    unsigned long long a = 0, b = 0;
    if (sscanf(buf, "vfunc %llx %llx", &a, &b) == 2) {
        if (!readable((uintptr_t)a, 8)) { logf_("vfunc: object 0x%llX not readable", a); return; }
        uintptr_t vt = *(uintptr_t *)(uintptr_t)a;
        if (!readable(vt + (uintptr_t)b, 8)) { logf_("vfunc: vtable 0x%llX + 0x%llX not readable", (unsigned long long)vt, b); return; }
        uintptr_t fn = *(uintptr_t *)(vt + (uintptr_t)b);
        logf_("vfunc: object 0x%llX vtable 0x%llX [+0x%llX] = 0x%llX", a, (unsigned long long)vt, b, (unsigned long long)fn);
        log_code_at("vfunc target", fn);
    } else if (sscanf(buf, "peek %llx %llu", &a, &b) == 2) {
        if (b > 64) b = 64;
        if (!readable((uintptr_t)a, (size_t)b)) { logf_("peek: 0x%llX not readable", a); return; }
        char hex[64 * 3 + 1] = "";
        for (unsigned i = 0; i < b; i++) snprintf(hex + i * 3, 4, "%02X ", ((uint8_t *)(uintptr_t)a)[i]);
        logf_("peek 0x%llX: %s", a, hex);
    } else {
        logf_("unknown command: %s", buf);
    }
}

static void read_settings(void) {
    char buf[256];
    int have_ini = read_small_file("CoopNative.ini", buf, sizeof buf);
    LONG veto = have_ini && strstr(buf, "veto_endcoop=1") != NULL;
    if (InterlockedExchange(&g_veto_enabled, veto) != veto) logf_("setting veto_endcoop = %ld", veto);
    LONG rest = have_ini && strstr(buf, "rest_in_coop=1") != NULL;
    if (InterlockedExchange(&g_rest_in_coop, rest) != rest) logf_("setting rest_in_coop = %ld", rest);
    LONG win = read_small_file("CoopNative.flags", buf, sizeof buf) && buf[0] == '1';
    if (InterlockedExchange(&g_spectate_window, win) != win) logf_("spectate window (from Lua) = %ld", win);
}

// Returns 1 hooked, 0 not executable yet (retry later), -1 refused for good. Every address is hard-coded for Code Vein
// 1.53.93354 (exe SHA-256 8fa550fe...); the first 16 bytes must match what was measured there, or the hook is skipped, so a
// different build (e.g. on another player's PC) cannot get a patch at the wrong place.
static int install(const char *name, uint64_t addr, const char *expect16, void *detour, void **orig) {
    const uint8_t *p = (const uint8_t *)(uintptr_t)addr;
    MEMORY_BASIC_INFORMATION mbi;
    const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || !(mbi.Protect & exec)) {
        logf_("%s: 0x%llX not executable yet (state 0x%lX, protect 0x%lX)", name, (unsigned long long)addr,
              (unsigned long)mbi.State, (unsigned long)mbi.Protect);
        return 0;
    }
    logf_("%s: first bytes %02X %02X %02X %02X %02X %02X %02X %02X", name, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
    if (!readable(addr, 16) || memcmp(p, expect16, 16) != 0) {
        logf_("%s: bytes at 0x%llX do not match this build's code, NOT hooking (different game version?)", name,
              (unsigned long long)addr);
        return -1;
    }
    MH_STATUS s = MH_CreateHook((LPVOID)p, detour, orig);
    if (s == MH_OK) s = MH_EnableHook((LPVOID)p);
    logf_("%s: hook at 0x%llX -> %s", name, (unsigned long long)addr, MH_StatusToString(s));
    return s == MH_OK ? 1 : -1;
}

static DWORD WINAPI worker(LPVOID unused) {
    (void)unused;
    Sleep(2000);   // let the engine finish its own startup first
    MH_STATUS s = MH_Initialize();
    logf_("MH_Initialize -> %s", MH_StatusToString(s));
    if (s != MH_OK) return 0;
    install("GetRescueSignalState", ADDR_EXEC_GET_RESCUE_SIGNAL_STATE,
            "\xE9\x1B\x56\xCB\x06\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC", (void *)detour_get_state, (void **)&g_orig_get_state);
    install("IsOnlineMultiplay", ADDR_IS_ONLINE_MULTIPLAY_BODY,
            "\x40\x53\x48\x83\xEC\x20\x48\x89\xCB\xE8\x12\x79\x9A\xF9\x85\xC0", (void *)detour_multiplay, (void **)&g_orig_multiplay);
    install("MultiPlayersCheck", ADDR_MULTI_PLAYERS_BODY,
            "\x40\x53\x48\x83\xEC\x20\xE8\x25\x88\x9A\xF9\x48\x89\xC3\x48\x85", (void *)detour_multi_players, (void **)&g_orig_multi_players);
    install("PromptBlock1", ADDR_PROMPT_BLOCK1_BODY,
            "\x40\x53\x48\x83\xEC\x20\x48\x89\xCB\xE8\xA2\xD5\x9A\xF9\x84\xC0", (void *)detour_prompt_block1, (void **)&g_orig_prompt_block1);
    install("EndCooperativeMode", ADDR_END_COOP_BODY,
            "\x40\x53\x48\x83\xEC\x20\x48\x89\xCB\xE8\x82\x03\x99\xF9\x48\x85", (void *)detour_end_coop, (void **)&g_orig_end_coop);
    // the Denuvo-moved .code region may become executable only after startup: retry every 2 s for 2 minutes
    int handler_ok = 0, tries = 0;
    LONG shown = -2;
    for (unsigned tick = 0;; tick++) {
        Sleep(200);
        read_settings();
        run_commands();
        if (tick % 5) continue;   // the rest once per second
        if (!handler_ok && tries < 60 && (++tries % 2) == 1)
            handler_ok = install("SessionStateHandler", ADDR_SESSION_STATE_HANDLER,
                                 "\x40\x55\x53\x57\x41\x54\x41\x55\x41\x56\x48\x8D\x6C\x24\xD1\x48",
                                 (void *)detour_state_handler, (void **)&g_orig_state_handler) != 0;
        LONG n = InterlockedExchange(&g_calls, 0), st = g_last_state;
        if (st != shown) { logf_("GetRescueSignalState now %ld (%ld calls in the last second)", st, n); shown = st; }
        LONG ro = InterlockedExchange(&g_rest_overrides, 0);
        if (ro) logf_("rest_in_coop: answered IsOnlineMultiplay=false to the Mistle prompt %ld times in the last second", ro);
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_log_lock);
        DWORD n = GetModuleFileNameA(inst, g_dir, MAX_PATH);
        while (n > 0 && g_dir[n - 1] != '\\') n--;
        g_dir[n] = 0;
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%sCoopNative.log", g_dir);
        g_log = _fsopen(path, "w", _SH_DENYWR);
        logf_("CoopNative loaded (probes: GetRescueSignalState, SessionStateHandler; EndCooperativeMode veto and Mistle rest are off unless enabled)");
        HANDLE h = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        if (h) CloseHandle(h);
    }
    return TRUE;
}
