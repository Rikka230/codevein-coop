# Code Vein seamless co-op (work in progress)

Mods that keep a Code Vein (2019) co-op group together: automatic re-hosting and re-joining after the deaths and boss kills
that end a vanilla session, co-op in areas whose boss is dead, and native experiments for resting at a Mistle in co-op and
keeping a dead guest in the session to spectate. **Not a release**: tested by hand on two PCs only, one guest (vanilla cap).

Findings, teardown table and QA live in the Co-op Anything write-up:
<https://github.com/cranebeaker/coop-anything/tree/main/games/code-vein>

Built for **Steam, version 1.53.93354** (exe SHA-256 `8fa550fe00a8c28c8947a9511128c21924860be0bb4ebf5a09b8a6cf50e0e172`).
CoopNative's addresses are fixed for that build; it checks 16 bytes at every target and skips any hook that does not match.

## Parts

| Path | What | Needed on |
|---|---|---|
| `mods/CoopSeamless` | UE4SS Lua mod: re-host / re-join reconciler, co-op in beaten areas. Switches in `config.txt` | host and guests |
| `mods/CoopProbe` | UE4SS Lua mod: state log every 0.2 s; tells CoopNative when a spectate window is open; spectate camera | everyone running CoopNative |
| `native/coopnative.c` | C DLL (MinHook detours), loaded by Ultimate ASI Loader. Experiments, all **off** unless enabled | host and guests (for tests) |
| `mods/CoopProgress` | UE4SS Lua mod: logs boss rewards, progress flags and the story position; gives the guest the host's boss items and story progress (both **off** unless enabled in `config.txt`, save backed up first) | guests (logging: anyone) |
| `tools/` | Read-only analysis: pak v4 reader, Kismet call/arg finders, exe scanners, headless Ghidra scripts | developers |

## Install (game closed)

Everything goes into `<game dir>\CodeVein\Binaries\Win64`.

1. **UE4SS v3.0.1-1181** (the `experimental-latest` build, <https://github.com/UE4SS-RE/RE-UE4SS/releases>). In `UE4SS-settings.ini` set **`HookLoadMap = 0`**
   (`1` crashes at the first map travel on this game).
2. Copy `mods/CoopSeamless` and `mods/CoopProbe` to `ue4ss\Mods\` and enable both in `ue4ss\Mods\mods.txt`
   (`CoopSeamless : 1`, `CoopProbe : 1`) and in `mods.json` if your UE4SS has one.
3. Optional, native experiments: put **Ultimate ASI Loader** (<https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases>,
   x64) in `Win64` as `winmm.dll`, build `CoopNative.asi` (below) and put it in `Win64\plugins\`. To switch experiments on,
   create `plugins\CoopNative.ini` with any of:
   ```
   veto_endcoop=1   keep the session when a player dies for good in a boss fight while another lives (both PCs)
   spectate_cam=1   while held that way, the dead player's camera follows a living player (CoopProbe)
   keep_dying=1     group respawn (both PCs): a guest whose revive window runs out stays in the host's world, his body is
                    removed, he spectates, and the host's next Mistle rest respawns him next to the host (appear effect)
   rest_in_coop=1   allow resting at a Mistle with a guest connected (host; on a guest only together with guest_rest)
   guest_rest=1     guest only: E at a Mistle opens his own Mistle menu instead of the vanilla rest (which asserts on a guest);
                    switch partner is disabled there; level-up in progress (see Status)
   levelup_relay=1  host only, research: guest level-up relay (Server_AddExp(0) signal)
   vestige_menu=1   host only, research: the Mistle menu sees "no signal" so its memory/vestige entries are enabled
   trace_death=1    log only: native trace of death/fade/spawn UFunctions (diagnostics)
   group_rest=1     host only, untested: heal every living player when the host rests (CoopProbe)
   ```
   CoopNative writes `plugins\CoopNative.log` on every launch.
4. Everyone in the group uses the **same matchmaking password**; the automatic search joins the first matching signal.

Uninstall: remove the two mod folders, `winmm.dll` and `plugins\`.

## Build CoopNative

Needs [Zig](https://ziglang.org/download/) (0.17 used; its clang cross-compiles for Windows without Visual Studio):
`ZIG=/path/to/zig sh native/build.sh` → `native/build/CoopNative.asi`.

## Status

Works (two PCs): rejoin after guest field death, host death, boss-room death + wipe, boss kill; co-op in beaten areas.

Works (two PCs, experiments):
- `rest_in_coop`: the host rests at a Mistle with a guest connected; the guest stays. Enemies do not reset yet, and the partner
  NPC respawns per rest.
- `veto_endcoop` + `spectate_cam`: a guest who dies for good in a boss fight stays in the host's world and watches the host fight.
  A wipe still ends the session normally. Two veto points are needed: `EndCooperativeMode` and the small function that actually
  switches co-op off (`0x14081BDA0`), which a Blueprint-driven death path calls directly. The black screen was the death overlay
  `WDT_DeathParts`, hidden while spectating. The low-health screen filter and heartbeat still play.

- Boss kill while a guest is held: the held guest replays the vetoed leave, the host's outro plays, the guest is sent home and
  rejoins (works).
- `keep_dying` group respawn (works): the final death is vetoed on both PCs (`TryExecuteDeath` 0x140508540 for falls decided on the
  host, the component death processing 0x14056AC60 for an expired revive window decided on the guest), the true-dead body is removed
  at once (the partner cannot revive it, the dissolve does not matter), the guest spectates, and the host's next rest spawns him next
  to the host with `CharacterHelper:RequestPlayerSpawn` and the appear sequence.
- `guest_rest`: his own Mistle menu opens on E (works); heal sticks; switch partner disabled. Level-up: research in progress (the
  guest owns his level and haze; his level-up resolved "the player character" to the host's copy on his PC; a redirect and a
  resync via the join-time sender `SyncAll` 0x140465CA0 are being tested).

In progress: enemy reset on a co-op rest (works once per session), vestige restoration (menu entries enabled; the restoration screen
still refuses), falls while protected (survive at the bottom; to become an immediate true death), 3+ players, group teleport (vote).
Guest progression and shared boss rewards: see issue #1 (CoopProgress).

## Contributing

Pull requests welcome. Session, rejoin and spectate code is actively changing here; please coordinate in an issue first.
Anything that writes to a player's save must be off by default and back the save up first. Never commit game files, saves,
logs or anything that identifies your machine.

## License

MIT (see `LICENSE`). MinHook (`native/third_party/minhook`) is BSD 2-Clause.
