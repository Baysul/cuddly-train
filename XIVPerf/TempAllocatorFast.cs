using System;
using Dalamud.Plugin.Services;

namespace XIVPerf;

/// <summary>
/// Replaces Client.Graphics.Temp.Allocator's vtable (AllocatorManager+0x10, an AllocatorHybrid) with a native
/// per-thread slab cache for allocations &lt;= 16 KiB and &lt;= 16-byte aligned (docs/research/engine-patches.md,
/// "TempAllocator fast slab cache"). Eliminates the 15-thread CRITICAL_SECTION lock convoy across Main, the 14
/// JobPool workers, and RenderThread.
/// </summary>
internal sealed unsafe class TempAllocatorFast : IDisposable
{
    // FFXIVClientStructs Client::Graphics::AllocatorManager.Instance (static pointer to AllocatorManager*).
    private const string AllocatorManagerSig = "74 12 48 8B 05 ?? ?? ?? ?? 48 8B 48 38";
    // AllocatorManager.Initialize: lea rax,[vtbl_AllocatorHybrid]; ...; lea rax,[vtbl_AllocatorHeap].
    private const string HybridHeapVtblSig =
        "48 8D 05 ?? ?? ?? ?? 48 89 03 48 8D 4B 68 48 8D 05 ?? ?? ?? ?? 48 89 43 08 33 C0 48 89 43 10";
    // FFXIVClientStructs AllocatorPool ctor: lea rax,[vtbl_AllocatorPool].
    private const string PoolVtblSig = "48 8D 05 ?? ?? ?? ?? 48 8B D9 48 89 01 33 C0 48 89 81";

    private readonly IPluginLog log;
    private bool wanted;

    public string Status { get; private set; } = "off";
    public bool Available { get; }
    public bool Wanted => wanted;

    private static nint Rel32(nint site, int dispOffset, int insnEnd) =>
        site + insnEnd + *(int*)(site + dispOffset);

    public TempAllocatorFast(ISigScanner scanner, IPluginLog log)
    {
        this.log = log;
        try
        {
            var amSite = scanner.ScanText(AllocatorManagerSig);
            var amSlot = Rel32(amSite, 5, 9);
            var am = *(nint*)amSlot;
            if (am == 0)
            {
                Status = "AllocatorManager is not initialised";
                log.Warning($"TempAllocator: {Status}");
                return;
            }
            var tempAlloc = *(nint*)(am + 0x10);
            if (tempAlloc == 0)
            {
                Status = "TempAllocator pointer is null";
                log.Warning($"TempAllocator: {Status}");
                return;
            }

            var hybridSite = scanner.ScanText(HybridHeapVtblSig);
            var hybridVtbl = Rel32(hybridSite, 3, 7);
            var heapVtbl = Rel32(hybridSite, 17, 21);
            var poolSite = scanner.ScanText(PoolVtblSig);
            var poolVtbl = Rel32(poolSite, 3, 7);

            // Real game .text bounds (see NotifierSkip: scanner.TextSectionBase points into Dalamud's copy).
            var image = scanner.Module.BaseAddress;
            var textStart = (nint)(image + scanner.TextSectionOffset);
            var textEnd = textStart + scanner.TextSectionSize;

            var r = Native.AllocInstall(tempAlloc, hybridVtbl, heapVtbl, poolVtbl, textStart, textEnd);
            if (r != 0)
            {
                Status = Native.Describe(r);
                log.Warning($"TempAllocator: not installed: {Status}");
                return;
            }
            Available = true;
            var s = Native.GetAllocStats();
            log.Information($"TempAllocator at {tempAlloc:X} installed (chain depth {s.ChainDepth}); "
                          + $"vtables hybrid {hybridVtbl:X}, heap {heapVtbl:X}, pool {poolVtbl:X}");
        }
        catch (Exception e)
        {
            Status = $"signature not found: {e.Message}";
            log.Warning(e, "TempAllocator: signature not found");
        }
    }

    public void SetEnabled(bool on)
    {
        wanted = on;
        if (!Available)
            return;
        var r = Native.AllocSetActive(on);
        if (r != 0)
        {
            Status = Native.Describe(r);
            log.Warning($"TempAllocator: {Status}");
        }
        else
        {
            Status = on ? "on" : "off";
            log.Information($"TempAllocator fast slab cache: {Status}");
        }
    }

    public void Dispose()
    {
        if (!Available)
            return;
        var r = Native.AllocUninstall();
        if (r != 0)
            log.Warning($"TempAllocator: removing the patch failed: {Native.Describe(r)}");
    }
}
