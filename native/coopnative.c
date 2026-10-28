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

// Replay of a vetoed end (session 7): a guest held by the veto never retries leaving, so after a boss kill the host's outro waits
// for him forever. CoopProbe sends "replay" when the boss rewards arrive and its own character is dead; the vetoed call is then
// made for real here, on the game thread (Lua polls GetRescueSignalState from it), and only on the same session manager object.
typedef void (*endcoop_fn)(void *sm);
static void *volatile g_vetoed_sm;
static volatile LONG g_vetoed_kind, g_replay_pending;   // kind 1 = SwitchToEndCoop, 2 = EndCooperativeMode
static endcoop_fn g_orig_end_coop, g_orig_switch_end_coop;

// R4 stage 1 (vestige_menu=1): the Mistle menu WDT_CheckpointMenu_C disables its memory/restoration pieces whenever
// GetRescueSignalState(self) is not Idle. Answer Idle (0) only when the calling Blueprint frame's object (FFrame + 0x18) is of the
// menu's class (UObject + 0x10), whose address CoopProbe sends once ("menuclass <hex>"). A Lua hook on this function froze the game
// when the distress signal was sent (2026-10-27), hence native.
static volatile LONG g_vestige_enabled, g_vestige_overrides;
static volatile uintptr_t g_menu_class;
static int readable(uintptr_t a, size_t len);
static int from_menu(void *stack) {
    uintptr_t cls = g_menu_class;
    if (!cls || !stack || !readable((uintptr_t)stack + 0x18, 8)) return 0;
    uintptr_t obj = *(uintptr_t *)((uintptr_t)stack + 0x18);
    return obj && readable(obj + 0x10, 8) && *(uintptr_t *)(obj + 0x10) == cls;
}

// Guest level-up (R6, session 8). CheckPointUIComponent:LevelUpPlayer (thunk 0x140A842C0) -> 0x14099C350(context, UpLevel) fetches
// "the player character" with 0x141DF4530(context, 0) and sends it the level-up message (0x2c). On a guest that lands on his network
// copy: the haze goes, the level does not. So the host does it on the guest's character: the guest's game sends Server_AddExp(0) once
// per level (harmless), the host's CoopNative counts those from a component that is not its own and runs 0x14099C350 with the player
// lookup redirected to the guest's pawn (first address in CoopNative.protect) for the duration of the call, on the game thread.
#define ADDR_GET_PLAYER_CHAR 0x141DF4530ULL
#define ADDR_LEVEL_UP 0x14099C350ULL
#define ADDR_EXEC_SERVER_ADDEXP 0x140ADA400ULL
typedef void *(*getplayerchar_fn)(void *ctx, int index);
typedef uint8_t (*levelup_fn)(void *ctx, int uplevel);
static getplayerchar_fn g_orig_get_player_char;
static levelup_fn g_orig_level_up;
static volatile uintptr_t g_override_player;
static volatile DWORD g_override_tid;
static volatile uintptr_t g_levelup_ctx, g_levelup_pawn;
static volatile LONG g_levelup_n, g_levelup_pending, g_guest_levelups;
static volatile LONG g_levelup_relay;   // set from the ini in read_settings
static void *detour_get_player_char(void *ctx, int index) {
    if (g_override_player && index == 0 && GetCurrentThreadId() == g_override_tid) return (void *)g_override_player;
    return g_orig_get_player_char(ctx, index);
}
// The level-up check 0x140593A20(iface, N) also needs haze >= cost, and on the host the guest's copy has haze 0 (his haze lives on
// his PC; Server_AddExp on the host does not change the copy). During our relayed level-up (override active on this thread) the
// check is answered "yes"; his menu has already spent the haze on his side.
#define ADDR_LEVEL_CHECK 0x140593A20ULL
typedef uint8_t (*lvlcheck_fn)(void *iface, int n);
static lvlcheck_fn g_orig_level_check;
static uint8_t detour_level_check(void *iface, int n) {
    if (g_override_player && GetCurrentThreadId() == g_override_tid) {
        uint8_t real = g_orig_level_check(iface, n);
        if (!real) logf_("level check for the relayed level-up: real answer 0 (haze), answering 1");
        return 1;
    }
    return g_orig_level_check(iface, n);
}
static void run_levelup(void) {
    uintptr_t ctx = g_levelup_ctx, pawn = g_levelup_pawn;
    LONG n = InterlockedExchange(&g_levelup_n, 0);   // reset: the relay adds one per Server_AddExp(0)
    if (!ctx || n <= 0) return;
    if (pawn) { g_override_tid = GetCurrentThreadId(); g_override_player = pawn; }
    uint8_t r = ((levelup_fn)ADDR_LEVEL_UP)((void *)ctx, (int)n);
    g_override_player = 0;
    logf_("levelup: +%ld level(s) for %s0x%llX -> %u", n, pawn ? "pawn " : "the local player via ", (unsigned long long)(pawn ? pawn : ctx), r);
}
// Resync after the guest's Mistle menu: the join-time sender is stub 0x140465CA0 -> 0x145DC5E40(character) ("SyncAll": the
// character's ServerInitialDataSync via 0x1404634D0, which sends only while +0xB82 (bIsInitialSynced) is clear, then every component).
// CoopProbe clears the flags and sends "syncall <pawn>"; the call is made here on the game thread.
#define ADDR_SYNC_ALL 0x140465CA0ULL
static volatile uintptr_t g_syncall_pawn;
static volatile LONG g_syncall_pending;
static void run_syncall(void) {
    uintptr_t pawn = g_syncall_pawn;
    if (!pawn) return;
    ((void (*)(void *))ADDR_SYNC_ALL)((void *)pawn);
    logf_("syncall: initial data sync sent again for 0x%llX", (unsigned long long)pawn);
}
// Level-up check (session 8 research): the HumanContextIF level-up 0x1405A2E20(iface, N) refuses when its check 0x140593A20 fails:
// base = iface - 0x368; level = base->vt[0x90](), max = base->vt[0x98](); level + N <= max; cost = iface->vt[0x138](level + N)
// (float, != 0); exp = iface->vt[0x118]() >= cost. "lvlinfo <component> [component ...]" logs those values for each
// HumanContextComponent, on the game thread. The interface pointer comes from 0x140EBF6A0(component, HumanContextIF class 0x140AD6680()).
static volatile uintptr_t g_lvlinfo_comp[4];
static volatile LONG g_lvlinfo_pending;
typedef void *(*getiface_fn)(void *obj, void *cls);
typedef int (*int0_fn)(void *self);
typedef float (*fint_fn)(void *self, int x);
static void run_lvlinfo(void) {
    void *ifcls = ((void *(*)(void))0x140AD6680ULL)();
    for (int k = 0; k < 4; k++) {
        uintptr_t comp = g_lvlinfo_comp[k];
        if (!comp) continue;
        g_lvlinfo_comp[k] = 0;
        uintptr_t iface = (uintptr_t)((getiface_fn)0x140EBF6A0ULL)((void *)comp, ifcls);
        if (!iface || !readable(iface, 8)) { logf_("lvlinfo 0x%llX: no HumanContextIF", (unsigned long long)comp); continue; }
        uintptr_t base = iface - 0x368;
        uintptr_t ivt = *(uintptr_t *)iface, bvt = *(uintptr_t *)base;
        int level = ((int0_fn)*(uintptr_t *)(bvt + 0x90))((void *)base);
        int maxl = ((int0_fn)*(uintptr_t *)(bvt + 0x98))((void *)base);
        float cost = ((fint_fn)*(uintptr_t *)(ivt + 0x138))((void *)iface, level + 1);
        int exp = ((int0_fn)*(uintptr_t *)(ivt + 0x118))((void *)iface);
        logf_("lvlinfo 0x%llX (iface 0x%llX, base 0x%llX = comp+0x%llX): level %d, max %d, cost(level+1) %.0f, haze %d", (unsigned long long)comp,
              (unsigned long long)iface, (unsigned long long)base, (unsigned long long)(base - comp), level, maxl, cost, exp);
    }
}
static int read_small_file(const char *name, char *buf, size_t size);
static uintptr_t first_protected(void) {
    char buf[128];
    if (!read_small_file("CoopNative.protect", buf, sizeof buf)) return 0;
    return (uintptr_t)strtoull(buf, NULL, 16);
}

// End the death fade after a group respawn (session 8). The native trace showed a death's input lock belongs to the guest's own
// BP_FadeUIComponent: it calls RequestDeathFade + SetTemporaryInputMask at the death and releases the lock only after its own
// RequestUnfade. The exec thunk 0x140AC76F0 (body 0x1483E02A0) ends in a virtual call on the context: (*(iface) + 0x38)(iface, Caller,
// Usage, DownCount, Time), and the context in the trace was always the component + 0x120 (its FadeUIComponentIF part). Calling it on
// the interface's default object from Lua crashed the game. CoopProbe sends "unfade <component hex>" once the guest has his new pawn;
// the call is made here on the game thread with Caller = the component, Usage = DeathPC (102), DownCount = true, Time = 0.3 s.
typedef void (*unfade_fn)(void *iface, void *caller, uint8_t usage, uint8_t down_count, float time);
static volatile uintptr_t g_unfade_obj;
static volatile LONG g_unfade_pending;
static int readable(uintptr_t a, size_t len);
static void run_unfade(void) {
    uintptr_t obj = g_unfade_obj, iface = obj + 0x120;
    if (!obj || !readable(iface, 8)) { logf_("unfade: component 0x%llX not readable", (unsigned long long)obj); return; }
    uintptr_t vt = *(uintptr_t *)iface;
    if (!readable(vt + 0x38, 8)) { logf_("unfade: vtable 0x%llX not readable", (unsigned long long)vt); return; }
    uintptr_t fn = *(uintptr_t *)(vt + 0x38);
    logf_("unfade: RequestUnfade(DeathPC) on component 0x%llX (iface vtable 0x%llX, fn 0x%llX)", (unsigned long long)obj,
          (unsigned long long)vt, (unsigned long long)fn);
    ((unfade_fn)fn)((void *)iface, (void *)obj, 102, 1, 0.3f);
}

static void detour_get_state(void *context, void *stack, void *result) {
    if (g_replay_pending && context && context == g_vetoed_sm && InterlockedExchange(&g_replay_pending, 0)) {
        LONG kind = InterlockedExchange(&g_vetoed_kind, 0);
        g_vetoed_sm = NULL;
        logf_("replay: calling the vetoed %s for real", kind == 2 ? "EndCooperativeMode" : "SwitchToEndCoop");
        if (kind == 2) g_orig_end_coop(context); else g_orig_switch_end_coop(context);
    }
    if (g_unfade_pending && InterlockedExchange(&g_unfade_pending, 0)) run_unfade();
    if (g_levelup_pending && InterlockedExchange(&g_levelup_pending, 0)) run_levelup();
    if (g_syncall_pending && InterlockedExchange(&g_syncall_pending, 0)) run_syncall();
    if (g_lvlinfo_pending && InterlockedExchange(&g_lvlinfo_pending, 0)) run_lvlinfo();
    int menu = g_vestige_enabled && from_menu(stack);
    g_orig_get_state(context, stack, result);
    if (menu && result) { *(uint8_t *)result = 0; InterlockedIncrement(&g_vestige_overrides); }
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
static void note_rest_event(void *frame);
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
    if (event == 0x16 && n > 1) note_rest_event(frames[1]);
}

// Veto experiment (off by default): AppSessionManager "EndCooperativeMode" (stub 0x140832D70, body 0x146E92EF0; only direct
// caller 0x146966279, a player-death handler). With no other players it switches the internal state (+0xD90) to 0x1F at once;
// otherwise it sets the request flag +0xDC0, which 0x14081C940 turns into 0x1F shortly after. Skipping the whole function while
// CoopNative.ini has veto_endcoop=1 AND CoopNative.flags reports a spectate window (boss fight, one player dead, one alive)
// keeps both the request and the state change from happening. Needed on host and guest (each ends co-op on its own side).
#define ADDR_END_COOP_BODY 0x146E92EF0ULL
static volatile LONG g_veto_enabled, g_spectate_window;

// Session 7: a boss kill calls EndCooperativeMode ~2 s after MulticastGainRewards; the polled flag was 0.3 s stale and the veto
// stalled the host's outro on a black screen. Re-read the flag file at the moment of the decision instead.
static int read_small_file(const char *name, char *buf, size_t size);
static int window_now(void) {
    char buf[8];
    LONG win = read_small_file("CoopNative.flags", buf, sizeof buf) && buf[0] == '1';
    InterlockedExchange(&g_spectate_window, win);
    return win;
}

static void detour_end_coop(void *sm) {
    uintptr_t caller = (uintptr_t)__builtin_return_address(0);
    if (g_veto_enabled && window_now()) {
        logf_("EndCooperativeMode from 0x%llX  *** VETOED (spectate window) ***", (unsigned long long)caller);
        g_vetoed_sm = sm; InterlockedExchange(&g_vetoed_kind, 2);
        return;
    }
    logf_("EndCooperativeMode from 0x%llX (allowed)", (unsigned long long)caller);
    g_orig_end_coop(sm);
}

// Session 7 (2026-10-27): with the EndCooperativeMode veto on, both sides still ended co-op through 0x14081BDA0 (in .xtls, not
// Denuvo-moved), the small "switch the internal state to 0x1F" function that EndCooperativeMode has inlined. The guest reached it
// from a Blueprint-called native (return 0x14633CFA0, after getting the session manager); the host from the session monitor
// (return 0x146E653C0) once no other player was left. Veto it in the spectate window, except from the monitor: when the other
// player is really gone, the host must end co-op.
#define ADDR_SWITCH_END_COOP 0x14081BDA0ULL
#define CALLER_NO_PLAYERS_LEFT 0x146E653C0ULL

static void detour_switch_end_coop(void *sm) {
    uintptr_t caller = (uintptr_t)__builtin_return_address(0);
    if (g_veto_enabled && caller != CALLER_NO_PLAYERS_LEFT && window_now()) {
        logf_("SwitchToEndCoop from 0x%llX  *** VETOED (spectate window) ***", (unsigned long long)caller);
        g_vetoed_sm = sm; InterlockedExchange(&g_vetoed_kind, 1);
        return;
    }
    logf_("SwitchToEndCoop from 0x%llX (allowed)", (unsigned long long)caller);
    g_orig_switch_end_coop(sm);
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

// R6, the guest's own rest (guest_rest=1, guest only; session 8). With rest_in_coop his E press starts the vanilla rest, which asks
// the session to close the enemy-reset channel: CloseOnlineSession (stub 0x1408176A0 -> body 0x146E5C3D0(world context, bits)),
// called with 8 from the Mistle's ProcessInputButtonEvent. On the host that runs session events 0x16 -> 0x17 -> 0x18 and the rest goes
// on; on a guest only 0x16 -> 0x19 came and he sat at the Mistle for good. On the guest, answer "done" for that call without touching
// the session: the host's world is not reset by his rest, and his rest no longer waits for the session.
#define ADDR_CLOSE_SESSION 0x1408176A0ULL
typedef uint64_t (*closesession_fn)(void *ctx, uint8_t bits);
static closesession_fn g_orig_close_session;
static volatile LONG g_guest_rest;
static uint64_t detour_close_session(void *ctx, uint8_t bits) {
    uintptr_t caller = (uintptr_t)__builtin_return_address(0);
    if (g_guest_rest && bits == 8 && caller >= 0x146A1CB70ULL && caller < 0x146A1CB70ULL + 0x800) {
        logf_("CloseOnlineSession(8) from the Mistle input 0x%llX  *** SKIPPED (guest_rest) ***", (unsigned long long)caller);
        return 1;
    }
    return g_orig_close_session(ctx, bits);
}

// R6 v2: skipping the session part was not enough: the rest also changes the Mistle actor itself and asserted on the guest
// (CheckPointBase.cpp:850). With guest_rest=1 the guest's E press at an opened Mistle no longer runs the Mistle's
// ProcessInputButtonEvent (vtable +0x618 -> stub 0x1406BB510 -> body 0x146A1CB70(mistle, pressed, character)) at all; CoopNative
// writes CoopNative.restreq and CoopProbe opens his Mistle menu directly (BP_CheckpointUIComponent:Open, validated on the host).
#define ADDR_MISTLE_INPUT 0x1406BB510ULL
static char g_dir[MAX_PATH];   // defined (tentatively) again further down
typedef void (*mistleinput_fn)(void *mistle, uint8_t pressed, void *character);
static mistleinput_fn g_orig_mistle_input;
static void detour_mistle_input(void *mistle, uint8_t pressed, void *character) {
    if (g_guest_rest && pressed == 1 && mistle && readable((uintptr_t)mistle + 0x660, 1) && *((uint8_t *)mistle + 0x660) == 2) {
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%sCoopNative.restreq", g_dir);
        FILE *f = _fsopen(path, "w", _SH_DENYNO);
        if (f) { fputs("1", f); fclose(f); }
        logf_("Mistle input at 0x%llX  *** vanilla rest SKIPPED (guest_rest), menu requested ***", (unsigned long long)(uintptr_t)mistle);
        return;
    }
    g_orig_mistle_input(mistle, pressed, character);
}

// Enemy reset on a co-op rest (session 7: the host rested, enemies stayed). During those rests two more callers got "true" from
// IsOnlineMultiplay: 0x1465871A7 ("if multiplayer, an extra condition must hold, else skip") and 0x1462711CE (branches on
// multiplayer). Guess, to be tested: answer "false" to those two for 6 s after a rest starts (session event 0x16 raised from the
// Mistle's ProcessInputButtonEvent).
#define REST_RESET_CALLER1 0x1465871A7ULL
#define REST_RESET_CALLER2 0x1462711CEULL
#define REST_WINDOW_MS 120000   // the reset check comes when the player leaves the Mistle (session 8: 19 s after sitting down)
static volatile ULONGLONG g_rest_started;
static volatile LONG g_reset_overrides;
static int in_rest_window(void) { return g_rest_started && GetTickCount64() - g_rest_started < REST_WINDOW_MS; }
// Session event 0x16 raised from the Mistle's input function (frame 2 in session 7) = a rest just started.
static void note_rest_event(void *frame) {
    uintptr_t f = (uintptr_t)frame;
    if (!g_rest_in_coop || f < MISTLE_INPUT_BEGIN || f >= MISTLE_INPUT_BEGIN + 0x800) return;
    g_rest_started = GetTickCount64();
    logf_("rest_in_coop: rest started, enemy-reset overrides active for %d ms", REST_WINDOW_MS);
}

static uint8_t detour_multiplay(void *ctx) {
    uint8_t r = g_orig_multiplay(ctx);
    uintptr_t caller = (uintptr_t)__builtin_return_address(0);
    if (r && g_rest_in_coop) {
        if (from_mistle_prompt(caller)) { InterlockedIncrement(&g_rest_overrides); return 0; }
        if ((caller == REST_RESET_CALLER1 || caller == REST_RESET_CALLER2) && in_rest_window()) {
            InterlockedIncrement(&g_reset_overrides); return 0;
        }
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
    } else if (strncmp(buf, "lvlinfo ", 8) == 0) {
        unsigned long long c[4] = {0, 0, 0, 0};
        int n = sscanf(buf + 8, "%llx %llx %llx %llx", &c[0], &c[1], &c[2], &c[3]);
        for (int k = 0; k < 4 && k < n; k++) g_lvlinfo_comp[k] = (uintptr_t)c[k];
        InterlockedExchange(&g_lvlinfo_pending, 1);
        logf_("lvlinfo requested for %d component(s)", n);
    } else if (sscanf(buf, "syncall %llx", &a) == 1) {
        g_syncall_pawn = (uintptr_t)a; InterlockedExchange(&g_syncall_pending, 1);
        logf_("syncall requested for 0x%llX", a);
    } else if (strncmp(buf, "levelup ", 8) == 0) {
        unsigned long long ctx = 0, pawn = 0; int n = 0;
        if (sscanf(buf + 8, "%llx %d %llx", &ctx, &n, &pawn) >= 2) {
            g_levelup_ctx = (uintptr_t)ctx; g_levelup_n = n; g_levelup_pawn = (uintptr_t)pawn;
            InterlockedExchange(&g_levelup_pending, 1);
            logf_("levelup requested: ctx 0x%llX, +%d, pawn 0x%llX", ctx, n, pawn);
        }
    } else if (sscanf(buf, "unfade %llx", &a) == 1) {
        g_unfade_obj = (uintptr_t)a; InterlockedExchange(&g_unfade_pending, 1);
        logf_("unfade requested for component 0x%llX", a);
    } else if (sscanf(buf, "menuclass %llx", &a) == 1) {
        g_menu_class = (uintptr_t)a;
        logf_("vestige_menu: WDT_CheckpointMenu_C class = 0x%llX", a);
    } else if (strncmp(buf, "replay", 6) == 0) {
        if (g_vetoed_kind) { InterlockedExchange(&g_replay_pending, 1); logf_("replay requested (sm 0x%p)", g_vetoed_sm); }
        else logf_("replay requested, but nothing was vetoed");
    } else {
        logf_("unknown command: %s", buf);
    }
}

static void read_settings(void) {
    char buf[256];
    int have_ini = read_small_file("CoopNative.ini", buf, sizeof buf);
    LONG veto = have_ini && strstr(buf, "veto_endcoop=1") != NULL;
    if (InterlockedExchange(&g_veto_enabled, veto) != veto) logf_("setting veto_endcoop = %ld", veto);
    LONG vest = have_ini && strstr(buf, "vestige_menu=1") != NULL;
    if (InterlockedExchange(&g_vestige_enabled, vest) != vest) logf_("setting vestige_menu = %ld", vest);
    LONG relay = have_ini && strstr(buf, "levelup_relay=1") != NULL;
    if (InterlockedExchange(&g_levelup_relay, relay) != relay) logf_("setting levelup_relay = %ld", relay);
    LONG grest = have_ini && strstr(buf, "guest_rest=1") != NULL;
    if (InterlockedExchange(&g_guest_rest, grest) != grest) logf_("setting guest_rest = %ld", grest);
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


// Death trace (trace_death=1 in CoopNative.ini, read once at startup; log only). Session 8: a Lua trace of these UFunctions crashed
// the game at the title screen, so the exec thunks (UE4SS object dump [f:], fixed without ASLR) are detoured natively instead. Each
// detour logs the call with the calling object (FFrame + 0x18) and its class (UObject + 0x10), then passes through. Calls from
// Blueprints and Lua go through these thunks; native C++ callers do not.
#define TRACE_N 17
static const char *g_trace_name[TRACE_N] = {"CharacterHelper:TryExecuteDeath", "CharacterHelper:RequestTryExecuteDeath", "DeathComponent:Multicast_MigrationToDeath", "AIDataManager:OnPlayerDeath", "UIHelper:RequestDeathFade", "FadeUIComponentIF:RequestDeathFade", "FadeUIComponentIF:RequestUnfade", "FadeUIComponentIF:RequestLoadingFade", "FadeUIComponentIF:FinishCurrentFadeKind", "CharacterHelper:RequestPlayerRespawn", "CharacterHelper:RequestPlayerSpawn", "BaseInputController:ServerRequestGuestPlayerSpawn", "HumanCommonBase:RPCServer_RequestRespawn", "CheckPointUIComponent:RequestPlayerSpawnToLastCheckPoint", "BaseInputController:SetTemporaryInputMask", "BaseInputController:ClearTemporaryInputMask", "BaseInputController:ClearAllTemporaryInputMask"};
static exec_fn g_trace_orig[TRACE_N];
static void trace_note(int i, void *ctx, void *stack) {
    uintptr_t obj = 0, cls = 0;
    if (stack && readable((uintptr_t)stack + 0x18, 8)) obj = *(uintptr_t *)((uintptr_t)stack + 0x18);
    if (obj && readable(obj + 0x10, 8)) cls = *(uintptr_t *)(obj + 0x10);
    logf_("TRACE %s ctx=0x%llX caller=0x%llX class=0x%llX", g_trace_name[i], (unsigned long long)(uintptr_t)ctx,
          (unsigned long long)obj, (unsigned long long)cls);
    // Session 8: an enemy kill on the guest reached RequestDeathFade without TryExecuteDeath or MigrationToDeath on either PC:
    // log the native call chain for the death-fade requests to find the common death point.
    if (strstr(g_trace_name[i], "DeathFade") || strstr(g_trace_name[i], "TryExecuteDeath")) {
        void *frames[24];
        USHORT n = RtlCaptureStackBackTrace(1, 24, frames, NULL);
        char bt[24 * 20] = "";
        for (USHORT k = 0; k < n; k++) {
            char one[20];
            snprintf(one, sizeof one, " %llX", (unsigned long long)(uintptr_t)frames[k]);
            strcat_s(bt, sizeof bt, one);
        }
        logf_("TRACE   backtrace:%s", bt);
    }
}
#define TRACE_DETOUR(i) static void trace_det_##i(void *c, void *s, void *r) { trace_note(i, c, s); g_trace_orig[i](c, s, r); }

TRACE_DETOUR(0)
TRACE_DETOUR(1)
TRACE_DETOUR(2)
TRACE_DETOUR(3)
TRACE_DETOUR(4)
TRACE_DETOUR(5)
TRACE_DETOUR(6)
TRACE_DETOUR(7)
TRACE_DETOUR(8)
TRACE_DETOUR(9)
TRACE_DETOUR(10)
TRACE_DETOUR(11)
TRACE_DETOUR(12)
TRACE_DETOUR(13)
TRACE_DETOUR(14)
TRACE_DETOUR(15)
TRACE_DETOUR(16)
static void *g_trace_det[TRACE_N] = {(void *)trace_det_0, (void *)trace_det_1, (void *)trace_det_2, (void *)trace_det_3, (void *)trace_det_4, (void *)trace_det_5, (void *)trace_det_6, (void *)trace_det_7, (void *)trace_det_8, (void *)trace_det_9, (void *)trace_det_10, (void *)trace_det_11, (void *)trace_det_12, (void *)trace_det_13, (void *)trace_det_14, (void *)trace_det_15, (void *)trace_det_16};
static const uint64_t g_trace_addr[TRACE_N] = {0x140A7CA70ULL, 0x140A76D10ULL, 0x140AA3D40ULL, 0x1409E4700ULL, 0x140B6ECF0ULL, 0x140AC7230ULL, 0x140AC76F0ULL, 0x140AC7330ULL, 0x140B62460ULL, 0x140A76620ULL, 0x140A767A0ULL, 0x140A3EF80ULL, 0x140ADA3B0ULL, 0x140A84770ULL, 0x140A3F380ULL, 0x140A3BE80ULL, 0x140A3BDE0ULL};
static const char *g_trace_expect[TRACE_N] = {
    "\xE9\x7B\x55\x34\x07\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC",
    "\xE9\xDB\x2B\x33\x07\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC",
    "\xE9\xBB\x63\x64\x07\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC",
    "\xE9\x4B\xEB\xAF\x06\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC",
    "\xE9\xCB\x7F\x52\x08\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC",
    "\xE9\x8B\x83\x91\x07\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC",
    "\xE9\xAB\x8B\x91\x07\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC",
    "\xE9\x2B\x84\x91\x07\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC",
    "\x48\x8B\x42\x20\x45\x33\xC0\x48\x85\xC0\x41\x0F\x95\xC0\x4C\x03",
    "\xE9\xCB\x1C\x33\x07\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC",
    "\xE9\x6B\x25\x33\x07\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC",
    "\x40\x53\x48\x83\xEC\x20\x48\x8B\x42\x20\x45\x33\xC0\x48\x85\xC0",
    "\x40\x53\x48\x83\xEC\x20\x48\x8B\x42\x20\x45\x33\xC0\x48\x85\xC0",
    "\xE9\xEB\xAA\x3A\x07\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC",
    "\xE9\xCB\x18\x11\x07\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC",
    "\xE9\x3B\xEC\x01\x07\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC",
    "\xE9\x5B\xE9\x01\x07\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC"};

// Keep a guest "dying" instead of dead (keep_dying=1, host only; session 8). A final death on the host goes CharacterHelper::
// RequestTryExecuteDeath (thunk 0x140A76D10) -> 0x140508540(world context, dying character, ...), which sends "execute death" to that
// character's death component and returns whether it did; Multicast_MigrationToDeath follows and the dead player's own death sequence
// starts (death fade, input lock, leave). Patching that sequence after the fact failed, so answer "no" here for any character that is
// not the host's own while the host lives: the guest stays in the revivable dying state. CoopProbe writes the host's living pawn
// address to CoopNative.host ("0" = do not veto); it is read at the moment of the decision.
// Second test (same session): an enemy kill of the guest never reached this function: his own game ran the death through a
// component's death-processing function (stub 0x14056AC60, body 0x14628F790; arg 1 = the component, its +0xE8 = the dying
// character) when his dying countdown ran out. Enemies die through it too, so "everyone but the host" became an explicit list:
// CoopProbe writes the characters to protect to CoopNative.protect (hex addresses separated by spaces; host: the guests' pawns in its
// world, guest: his own pawn), and both functions veto only for those.
#define ADDR_TRY_EXECUTE_DEATH 0x140508540ULL
#define ADDR_DEATH_PROCESS 0x14056AC60ULL
typedef uint8_t (*trydeath_fn)(void *world, void *target, uint8_t a3, uint8_t a4, uint8_t a5);
typedef void (*deathproc_fn)(void *component, void *arg);
static trydeath_fn g_orig_try_death;
static deathproc_fn g_orig_death_process;
static volatile ULONGLONG g_last_death_veto_log, g_last_proc_veto_log;
static int is_protected(uintptr_t who) {
    char buf[128];
    if (!who || !read_small_file("CoopNative.protect", buf, sizeof buf)) return 0;
    for (char *p = buf; *p; ) {
        char *end;
        uintptr_t a = (uintptr_t)strtoull(p, &end, 16);
        if (end == p) break;
        if (a && a == who) return 1;
        p = end;
    }
    return 0;
}
// The moment a veto fires is the "true death" (the revive window ran out): note the character in CoopNative.truedead so
// CoopProbe starts spectating only then, and teleports the guest next to the host when he is revived on the host's rest.
static void note_true_death(uintptr_t who) {
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%sCoopNative.truedead", g_dir);
    FILE *f = _fsopen(path, "w", _SH_DENYNO);
    if (f) { fprintf(f, "%llX", (unsigned long long)who); fclose(f); }
}
static uint8_t detour_try_death(void *world, void *target, uint8_t a3, uint8_t a4, uint8_t a5) {
    if (is_protected((uintptr_t)target)) {
        ULONGLONG now = GetTickCount64();
        if (now - g_last_death_veto_log > 1000) {
            g_last_death_veto_log = now;
            logf_("TryExecuteDeath on 0x%llX (args %u %u %u)  *** VETOED (keep_dying) ***",
                  (unsigned long long)(uintptr_t)target, a3, a4, a5);
            note_true_death((uintptr_t)target);
        }
        return 0;
    }
    logf_("TryExecuteDeath on 0x%llX (args %u %u %u) (allowed)", (unsigned long long)(uintptr_t)target, a3, a4, a5);
    return g_orig_try_death(world, target, a3, a4, a5);
}
// The revive window running out also plays the character's dissolve (AppCharacter:ExecuteDisappearExpression, a Blueprint-callable
// UFunction, exec thunk 0x1409EE3F0); it runs apart from the death processing, so a guest revived after a true death stayed invisible
// on both PCs (session 8). Skip it for protected characters. The function has no parameters; when called from a Blueprint the thunk
// still does P_FINISH (Stack.Code += 1 if Code is set, FFrame + 0x20), which the skip must do too or the Blueprint VM desyncs.
#define ADDR_EXEC_DISAPPEAR 0x1409EE3F0ULL
static exec_fn g_orig_disappear;
static void detour_disappear(void *ctx, void *stack, void *result) {
    if (is_protected((uintptr_t)ctx)) {
        logf_("ExecuteDisappearExpression on 0x%llX  *** SKIPPED (keep_dying) ***", (unsigned long long)(uintptr_t)ctx);
        if (stack && readable((uintptr_t)stack + 0x20, 8)) {
            uintptr_t *code = (uintptr_t *)((uintptr_t)stack + 0x20);
            if (*code) *code += 1;
        }
        return;
    }
    g_orig_disappear(ctx, stack, result);
}

// After a true death the guest is only kept "dying" by our veto, so the partner (Io) could still revive him (session 8). The
// game's own "can this character never be revived" check is CharacterHelper::IsNeverRevive (thunk 0x140A6E6C0 -> impl stub
// 0x1404F8170(target) -> bool); answer true for the character in CoopNative.truedead until the host's rest respawns him.
#define ADDR_NEVER_REVIVE 0x1404F8170ULL
typedef uint8_t (*neverrevive_fn)(void *target);
static neverrevive_fn g_orig_never_revive;
static volatile ULONGLONG g_last_nr_log;
static uintptr_t true_dead_now(void) {
    char buf[32];
    if (!read_small_file("CoopNative.truedead", buf, sizeof buf)) return 0;
    return (uintptr_t)strtoull(buf, NULL, 16);
}
static uint8_t detour_never_revive(void *target) {
    uint8_t r = g_orig_never_revive(target);
    uintptr_t td = true_dead_now();
    if (!r && td && (uintptr_t)target == td) {
        ULONGLONG now = GetTickCount64();
        if (now - g_last_nr_log > 1000) { g_last_nr_log = now; logf_("IsNeverRevive(0x%llX) -> true (true death, keep_dying)", (unsigned long long)td); }
        return 1;
    }
    return r;
}

static exec_fn g_orig_server_addexp;
static void detour_server_addexp(void *ctx, void *stack, void *result) {
    // RPCs run through ProcessEvent: no bytecode (FFrame+0x20 == 0), the parameters are at FFrame+0x28 (Locals), Value first
    int value = 1;
    if (stack && readable((uintptr_t)stack + 0x20, 16) && *(uintptr_t *)((uintptr_t)stack + 0x20) == 0) {
        uintptr_t locals = *(uintptr_t *)((uintptr_t)stack + 0x28);
        if (locals && readable(locals, 4)) value = *(int *)locals;
    }
    if (g_levelup_relay && value != 0) logf_("Server_AddExp(%d) from component 0x%llX", value, (unsigned long long)(uintptr_t)ctx);
    if (g_levelup_relay && value == 0) {
        uintptr_t pawn = first_protected();
        if (pawn) {
            g_levelup_ctx = (uintptr_t)ctx; g_levelup_pawn = pawn;
            InterlockedIncrement(&g_levelup_n);
            InterlockedExchange(&g_levelup_pending, 1);
            logf_("Server_AddExp(0) from component 0x%llX = guest level-up -> +1 for pawn 0x%llX", (unsigned long long)(uintptr_t)ctx,
                  (unsigned long long)pawn);
            g_orig_server_addexp(ctx, stack, result);
            return;
        }
    }
    g_orig_server_addexp(ctx, stack, result);
}
static uint8_t detour_level_up(void *ctx, int uplevel) {
    // Session 8: the guest owns his level and haze (the host copy mirrors him; host-side level-ups do not stick, and his haze went up
    // when the host sent Server_AddExp). His own level-up "succeeded" but his level never moved: on his PC 0x141DF4530(ctx, 0) seems
    // to return the host's character, so the level-up went to the host's copy there. Redirect it to his own pawn (the protect list
    // holds his pawn on his PC); CoopProbe's resync after the menu then carries the new level to the host.
    if (g_guest_rest && !g_override_player) {
        uintptr_t me = first_protected();
        if (me) {
            g_override_tid = GetCurrentThreadId(); g_override_player = me;
            uint8_t r = g_orig_level_up(ctx, uplevel);
            g_override_player = 0;
            logf_("LevelUpPlayer(+%d) on the guest, redirected to his own pawn 0x%llX -> %u", uplevel, (unsigned long long)me, r);
            return r;
        }
    }
    return g_orig_level_up(ctx, uplevel);
}

static void detour_death_process(void *component, void *arg) {
    uintptr_t c = (uintptr_t)component, who = 0;
    if (c && readable(c + 0xE8, 8)) who = *(uintptr_t *)(c + 0xE8);
    if (is_protected(who)) {
        ULONGLONG now = GetTickCount64();
        if (now - g_last_proc_veto_log > 1000) {
            g_last_proc_veto_log = now;
            logf_("DeathProcess for 0x%llX  *** VETOED (keep_dying) ***", (unsigned long long)who);
            note_true_death(who);
        }
        return;
    }
    g_orig_death_process(component, arg);
}

// Initial-data sync trace (always on, log only; session 8 R6): the join-time sync runs per component (AppCharacter /
// AppActorComponent ServerInitialDataSync, Jinkaku/Weapon ServerCustomInitialDataSync, and the Multicast answers) and each marks
// itself bIsInitialSynced. A guest's menu changes (level, gifts) are never synced after joining; CoopProbe clears the flags after his
// Mistle menu closes to see whether the game sends the sync again. These detours only log the calls.
#define SYNC_N 6
static const char *g_sync_name[SYNC_N] = {"AppCharacter:ServerInitialDataSync", "AppCharacter:MulticastInitialDataSync", "AppActorComponent:ServerInitialDataSync", "AppActorComponent:MulticastInitialDataSync", "JinkakuComponent:ServerCustomInitialDataSync", "WeaponComponent:ServerCustomInitialDataSync"};
static const uint64_t g_sync_addr[SYNC_N] = {0x1409EFA20ULL, 0x1409EE880ULL, 0x1409EFAD0ULL, 0x1409EE900ULL, 0x140AE9390ULL, 0x140B83930ULL};
static const char *g_sync_expect[SYNC_N] = {"\xE9\x5B\x7D\xB6\x06\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC", "\xE9\x4B\x36\xB6\x06\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC", "\xE9\x4B\x80\xB6\x06\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC", "\xE9\xAB\x37\xB6\x06\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC", "\xE9\xAB\xE6\xB0\x07\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC", "\xE9\xEB\x31\x5E\x08\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC"};
static exec_fn g_sync_orig[SYNC_N];
static volatile LONG g_sync_count[SYNC_N];
// the first calls of each: native call chain, to find the function that builds and sends the sync at a join
static void sync_backtrace(int i) {
    if (g_sync_count[i] > 3) return;
    void *frames[20];
    USHORT n = RtlCaptureStackBackTrace(2, 20, frames, NULL);
    char bt[20 * 20] = "";
    for (USHORT k = 0; k < n; k++) { char one[20]; snprintf(one, sizeof one, " %llX", (unsigned long long)(uintptr_t)frames[k]); strcat_s(bt, sizeof bt, one); }
    logf_("SYNC   backtrace:%s", bt);
}
#define SYNC_DETOUR(i) static void sync_det_##i(void *c, void *st, void *r) { \
    if (InterlockedIncrement(&g_sync_count[i]) <= 400) { logf_("SYNC %s on 0x%llX", g_sync_name[i], (unsigned long long)(uintptr_t)c); sync_backtrace(i); } \
    g_sync_orig[i](c, st, r); }
SYNC_DETOUR(0)
SYNC_DETOUR(1)
SYNC_DETOUR(2)
SYNC_DETOUR(3)
SYNC_DETOUR(4)
SYNC_DETOUR(5)
static void *g_sync_det[SYNC_N] = {(void *)sync_det_0, (void *)sync_det_1, (void *)sync_det_2, (void *)sync_det_3, (void *)sync_det_4, (void *)sync_det_5};

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
    {
        char ini[256];
        if (read_small_file("CoopNative.ini", ini, sizeof ini) && strstr(ini, "trace_death=1"))
            for (int i = 0; i < TRACE_N; i++)
                install(g_trace_name[i], g_trace_addr[i], g_trace_expect[i], g_trace_det[i], (void **)&g_trace_orig[i]);
    }
    for (int i = 0; i < SYNC_N; i++) install(g_sync_name[i], g_sync_addr[i], g_sync_expect[i], g_sync_det[i], (void **)&g_sync_orig[i]);
    install("LevelCheck", ADDR_LEVEL_CHECK,
            "\xE9\x0B\x6B\xD6\x05\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC", (void *)detour_level_check, (void **)&g_orig_level_check);
    install("GetPlayerCharacter", ADDR_GET_PLAYER_CHAR,
            "\xE9\x8B\xD1\xE3\x0C\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC", (void *)detour_get_player_char, (void **)&g_orig_get_player_char);
    install("LevelUp", ADDR_LEVEL_UP,
            "\xE9\xBB\xF0\x90\x06\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC", (void *)detour_level_up, (void **)&g_orig_level_up);
    install("Server_AddExp", ADDR_EXEC_SERVER_ADDEXP,
            "\xE9\x1B\xE1\xA0\x07\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC", (void *)detour_server_addexp, (void **)&g_orig_server_addexp);
    install("MistleInput", ADDR_MISTLE_INPUT,
            "\xE9\x5B\x16\x36\x06\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC", (void *)detour_mistle_input, (void **)&g_orig_mistle_input);
    install("CloseOnlineSession", ADDR_CLOSE_SESSION,
            "\xE9\x2B\x4D\x64\x06\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC", (void *)detour_close_session, (void **)&g_orig_close_session);
    install("IsNeverRevive", ADDR_NEVER_REVIVE,
            "\xE9\x9B\x40\xC4\x05\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC", (void *)detour_never_revive, (void **)&g_orig_never_revive);
    install("ExecuteDisappearExpression", ADDR_EXEC_DISAPPEAR,
            "\xE9\xBB\x1E\xB6\x06\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC", (void *)detour_disappear, (void **)&g_orig_disappear);
    install("DeathProcess", ADDR_DEATH_PROCESS,
            "\xE9\x2B\x4B\xD2\x05\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC", (void *)detour_death_process, (void **)&g_orig_death_process);
    install("TryExecuteDeath", ADDR_TRY_EXECUTE_DEATH,
            "\xE9\x9B\x59\xC7\x05\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC", (void *)detour_try_death, (void **)&g_orig_try_death);
    install("SwitchToEndCoop", ADDR_SWITCH_END_COOP,
            "\xC6\x81\xB8\x0A\x00\x00\x01\x48\x63\x81\x90\x0D\x00\x00\x83\xF8", (void *)detour_switch_end_coop, (void **)&g_orig_switch_end_coop);
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
        LONG vo = InterlockedExchange(&g_vestige_overrides, 0);
        if (vo) logf_("vestige_menu: answered Idle to the Mistle menu %ld times in the last second", vo);
        LONG rr = InterlockedExchange(&g_reset_overrides, 0);
        if (rr) logf_("rest_in_coop: answered IsOnlineMultiplay=false to the enemy-reset callers %ld times in the last second", rr);
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
