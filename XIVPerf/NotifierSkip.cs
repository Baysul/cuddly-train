using System;
using Dalamud.Plugin.Services;

namespace XIVPerf;

/// <summary>
/// Keeps Kernel::Notifier nodes whose callbacks can never do work off the list that DeviceDX11.PostTick walks with
/// EndFrame and BeginFrame every frame (docs/research/engine-patches.md, "Notifier list"). Native code replaces
/// Notifier.Link and Notifier.Unlink and does all list work under the game's list lock; this class finds the functions,
/// turns the skip on and off, and runs the watchdog once a frame.
/// </summary>
internal sealed class NotifierSkip : IDisposable
{
    private const string LinkSignature = "E8 ?? ?? ?? ?? 41 0F B6 C5 E9";      // FFXIVClientStructs Notifier.Link
    private const string UnlinkSignature = "E8 ?? ?? ?? ?? 45 33 F6 48 8D 5E";  // FFXIVClientStructs Notifier.Unlink
    private const int WatchPerFrame = 512;
    private const int MaxLoggedFlips = 20;
    private static readonly string[] GateNames = ["unrecognised", "always idle", "buffer", "index buffer", "texture", "constant buffer"];

    private readonly IPluginLog log;
    private bool wanted;
    private int lastResult = int.MinValue;
    private ulong flipsSeen;
    private int flipsLogged;

    public string Status { get; private set; } = "off";
    public bool Available { get; }
    /// <summary>What was asked for; <see cref="Active"/> follows after the native grace period.</summary>
    public bool Wanted => wanted;
    public bool Active => Available && Native.GetNotifierStats().Active != 0;

    public NotifierSkip(ISigScanner scanner, IPluginLog log)
    {
        this.log = log;
        try
        {
            var link = scanner.ScanText(LinkSignature);
            var unlink = scanner.ScanText(UnlinkSignature);
            // The game's own image. Not scanner.TextSectionBase: Dalamud's scanner searches a copy of the module
            // (TargetSigScanner(doCopy: true)) and TextSectionBase points into that copy; ScanText returns real addresses.
            var image = scanner.Module.BaseAddress;
            var textStart = (nint)(image + scanner.TextSectionOffset);
            var textEnd = textStart + scanner.TextSectionSize;
            var imageEnd = image + scanner.Module.ModuleMemorySize;
            flipsSeen = Native.GetNotifierStats().Flips;   // the same native module keeps its counters across reloads
            var r = Native.NotifierInstall(link, unlink, textStart, textEnd, image, imageEnd);
            if (r != 0)
            {
                Status = Native.Describe(r);
                log.Warning($"Notifier list: not installed: {Status}");
                return;
            }
            Available = true;
            log.Information($"Notifier.Link at {link:X}, Unlink at {unlink:X} replaced; code {textStart:X}-{textEnd:X}");
        }
        catch (Exception e)
        {
            Status = $"signature not found: {e.Message}";
            log.Warning(e, "Notifier list: signature not found");
        }
    }

    public void SetEnabled(bool on)
    {
        wanted = on;
        Apply();
    }

    /// <summary>Once per frame: finishes turning on after the grace period, and re-checks some skipped nodes.</summary>
    public void Tick()
    {
        if (!Available)
            return;
        var s = Native.GetNotifierStats();
        if (wanted && s.Active == 0)
        {
            Apply();
            return;
        }
        if (s.Active == 0)
            return;
        Native.NotifierWatch(WatchPerFrame);
        s = Native.GetNotifierStats();
        if (s.Flips != flipsSeen)
        {
            flipsSeen = s.Flips;
            if (flipsLogged++ < MaxLoggedFlips)
                log.Warning($"Notifier list: node {s.LastFlipNode:X} ({GateName(s.LastFlipGate)}) can now do work; put back on the list");
        }
    }

    private void Apply()
    {
        if (!Available)
            return;
        var r = Native.NotifierSetActive(wanted);
        var s = Native.GetNotifierStats();
        if (r == Native.ErrorWait)
            Status = "turning on in a moment";
        else if (r != 0)
            Status = Native.Describe(r);
        else if (wanted)
            Status = $"on: took {s.IdleAtEnable:N0} of {s.ListedAtEnable:N0} nodes off the list "
                   + $"({s.WorkingAtEnable:N0} can work, {s.UnrecognisedAtEnable:N0} unrecognised)";
        else
            Status = "off";
        if (r == lastResult && r != 0)
            return;   // still waiting, or the same failure: logged once
        lastResult = r;
        if (r == Native.ErrorWait)
            return;
        if (r != 0)
            log.Warning($"Notifier list: {Status}");
        else
            log.Information($"Notifier list: {Status}");
        if (r == 0 && wanted && s.ListedAtEnable >= 1000 && s.UnrecognisedAtEnable * 2 > s.ListedAtEnable)
            log.Warning($"Notifier list: most nodes are unrecognised, {Status}");
    }

    public static string GateName(int gate) => gate >= 0 && gate < GateNames.Length ? GateNames[gate] : $"gate {gate}";

    public void Dispose()
    {
        if (!Available)
            return;
        var r = Native.NotifierUninstall();   // puts every skipped node back first
        if (r != 0)
            log.Warning($"Notifier list: removing the patch failed: {Native.Describe(r)}");
    }
}
