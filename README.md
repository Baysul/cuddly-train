# XIVPerf (`cuddly-train`)

A Dalamud plugin (API 15, .NET 10) with a companion native Windows x64 DLL (`xivperf_native.dll`) that applies targeted client-side engine patches for crowded FFXIV cities.

## Patches

Each patch runs purely in native code on game threads (no reverse-P/Invoke on hot paths, byte-verified before installing, safe fallback on game updates) and can be toggled independently at runtime via `/xivperf`:

1. **Havok child table (`/xivperf havok on|off|toggle`)** — replaces the O(bones²) parent-index scan in `hkaPose::accessBoneModelSpace(..., DONT_PROPAGATE)` with a per-skeleton direct-child table. Includes an optional runtime verifier (`/xivperf verify on|off`).
2. **`Kernel::Notifier` skip list (`/xivperf notifier on|off|toggle`)** — keeps idle GPU resources whose `BeginFrame`/`EndFrame` callbacks can never do work off the global `Notifier` list walked twice per frame by `DeviceDX11.PostTick`, with a per-frame watchdog safety net.
3. **`TempAllocator` fast slab cache (`/xivperf alloc on|off|toggle`)** — atomically replaces `Client.Graphics.Temp.Allocator`'s vtable (`AllocatorManager+0x10`) with a per-thread 2-magazine slab cache (`<= 16 KiB`, `<= 16`-byte aligned, direct TEB TLS lookup, `FlsAlloc` thread-exit draining, free-list link validation) backed by per-class central batch depots, removing the 15-thread `CRITICAL_SECTION` lock convoy during `RenderView`.
4. **Command sort (`/xivperf sort on|off|toggle`, `/xivperf sortverify on|off`)** — replaces `Kernel::Device.GatherCommands`' call to `Kernel::Context.GatherCommands` with a single-copy stable sort (in-place insertion sort under 64 records, 4-byte LSD radix sort skipping uniform key bytes above 64, with a collision-free per-context/list pass-parity hint table), matching `output[0, blocks*1024)` byte-for-byte.

## Installing via Custom Plugin Repository

1. In-game, open `/xlsettings` → **Experimental** → **Custom Plugin Repositories**.
2. Add:
   ```
   https://raw.githubusercontent.com/Baysul/dalamud-plogons/main/pluginmaster.json
   ```
3. Save, open `/xlplugins`, and install **XIVPerf**.

## Building locally (Linux)

Prerequisites: `.NET 10 SDK`, `x86_64-w64-mingw32-gcc` (`mingw-w64`), and a Dalamud installation (default `~/.xlcore/dalamud/Hooks/dev/`, or set `DALAMUD_HOME`).

```bash
cd XIVPerf
dotnet build -c Release
```

The build compiles `native/xivperf_native.c` via `native/build.sh` and packages `XIVPerf.dll`, `XIVPerf.json`, `XIVPerf.deps.json`, and `xivperf_native.dll` into `XIVPerf/bin/Release/XIVPerf/latest.zip`.

## Manual CI builds

Go to **Actions → Build XIVPerf → Run workflow** to trigger a manual build (with options to publish a GitHub Release, override the version, or target `release` vs `staging` Dalamud).
