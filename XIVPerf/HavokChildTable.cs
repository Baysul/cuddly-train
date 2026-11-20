using System;
using Dalamud.Plugin.Services;

namespace XIVPerf;

/// <summary>Replaces hkaPose::accessBoneModelSpace's scan for a bone's children with a per-skeleton table (native
/// code; docs/research/client-crowd-performance.md, "The Havok child scan").</summary>
internal sealed class HavokChildTable : IDisposable
{
    // FFXIVClientStructs hkaPose.AccessBoneModelSpace is "48 8B C4 89 50 10 53 57"; longer here so it is unique.
    private const string AccessSignature =
        "48 8B C4 89 50 10 53 57 48 81 EC B8 00 00 00 48 89 68 18 48 8B D9 48 89 70 E8 48 8B 01 4C 63 CA 45 85 C0";
    // FFXIVClientStructs hkaPose.CalculateBoneModelSpace: a call to it; ScanText resolves the call's target.
    private const string CalculateSignature = "E8 ?? ?? ?? ?? 0F 28 2F";

    private readonly IPluginLog log;
    private readonly nint access;
    private readonly nint calculate;

    public string Status { get; private set; } = "not installed";
    public bool Installed => Native.GetStats().Installed != 0;

    public HavokChildTable(ISigScanner scanner, IPluginLog log)
    {
        this.log = log;
        if (Installed)   // left installed by an earlier instance whose Dispose did not run (same native module)
            Status = "installed";
        try
        {
            access = scanner.ScanText(AccessSignature);
            calculate = scanner.ScanText(CalculateSignature);
            log.Information($"hkaPose::accessBoneModelSpace at {access:X}, calculateBoneModelSpace at {calculate:X}");
        }
        catch (Exception e)
        {
            Status = $"signature not found: {e.Message}";
            log.Warning(e, "Havok child table: signature not found");
        }
    }

    public void SetEnabled(bool on)
    {
        if (on == Installed || (on && access == 0))   // removing needs no address: the native side keeps it
            return;
        var r = on ? Native.HavokInstall(access, calculate) : Native.HavokUninstall();
        if (on && r == -3)
        {
            // The native side checked the function's bytes and reads the callee from its own call instruction;
            // the signature result is only a cross-check.
            log.Warning($"calculateBoneModelSpace from the signature ({calculate:X}) is not the one the function calls; using the function's own");
            r = Native.HavokInstall(access, 0);
        }
        Status = r == 0 ? (on ? "installed" : "not installed") : Native.Describe(r);
        if (r == 0)
            log.Information($"Havok child table {(on ? "installed" : "removed")}");
        else
            log.Warning($"Havok child table {(on ? "install" : "removal")} failed: {Status}");
    }

    public void Dispose() => SetEnabled(false);
}
