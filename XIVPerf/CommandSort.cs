using System;
using Dalamud.Plugin.Services;

namespace XIVPerf;

/// <summary>Replaces Context.GatherCommands' copy-twice-and-merge-sort of each context's render commands with one copy
/// and a stable radix sort: the same records in the same order (native code; docs/research/engine-patches.md,
/// "Command sort").</summary>
internal sealed class CommandSort : IDisposable
{
    // FFXIVClientStructs Kernel.Device.GatherCommands and Kernel.Context.GatherCommands.
    private const string DeviceSignature =
        "4C 89 4C 24 20 4C 89 44 24 18 89 54 24 10 48 89 4C 24 08 48 83 EC 48 48 8B 44 24 50";
    private const string ContextSignature =
        "48 89 54 24 10 55 41 54 41 56 48 83 EC 30 41 8B C0 4D 8B F1 48 FF C0 C7 02 00 00 00 00";

    private readonly IPluginLog log;
    private readonly nint device;
    private readonly nint context;

    public string Status { get; private set; } = "not installed";
    public bool Installed => Native.GetSortStats().Installed != 0;
    public bool Available => (device != 0 && context != 0) || Installed;

    public CommandSort(ISigScanner scanner, IPluginLog log)
    {
        this.log = log;
        if (Installed)   // left installed by an earlier instance whose Dispose did not run (same native module)
            Status = "installed";
        try
        {
            device = scanner.ScanText(DeviceSignature);
            context = scanner.ScanText(ContextSignature);
            log.Information($"Device.GatherCommands at {device:X}, Context.GatherCommands at {context:X}");
        }
        catch (Exception e)
        {
            Status = $"signature not found: {e.Message}";
            log.Warning(e, "Command sort: signature not found");
        }
    }

    public void SetEnabled(bool on)
    {
        if (on == Installed || (on && (device == 0 || context == 0)))   // removing needs no address: the native side keeps it
            return;
        var r = on ? Native.SortInstall(device, context) : Native.SortUninstall();
        Status = r == 0 ? (on ? "installed" : "not installed") : Native.Describe(r);
        if (r == 0)
            log.Information($"Command sort {(on ? "installed" : "removed")}");
        else
            log.Warning($"Command sort {(on ? "install" : "removal")} failed: {Status}");
    }

    public void Dispose() => SetEnabled(false);
}
