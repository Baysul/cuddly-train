using System;
using System.Diagnostics;
using System.IO;
using Dalamud.Game.Command;
using Dalamud.Interface.Windowing;
using Dalamud.IoC;
using Dalamud.Plugin;
using Dalamud.Plugin.Services;

namespace XIVPerf;

public sealed class Plugin : IDalamudPlugin
{
    [PluginService] internal static IDalamudPluginInterface PluginInterface { get; private set; } = null!;
    [PluginService] internal static IFramework Framework { get; private set; } = null!;
    [PluginService] internal static ISigScanner SigScanner { get; private set; } = null!;
    [PluginService] internal static ICommandManager CommandManager { get; private set; } = null!;
    [PluginService] internal static IChatGui Chat { get; private set; } = null!;
    [PluginService] internal static IPluginLog Log { get; private set; } = null!;

    private const string Command = "/xivperf";

    internal Configuration Config { get; }
    internal HavokChildTable Havok { get; } = null!;
    internal NotifierSkip Notifier { get; } = null!;
    internal TempAllocatorFast TempAlloc { get; } = null!;
    internal CommandSort Sort { get; } = null!;
    internal FrameStats Rates { get; } = null!;   // after Native.Load: it reads the native counters

    private readonly WindowSystem windows = new("XIVPerf");
    private readonly MainWindow mainWindow = null!;
    private bool frameworkHooked, commandAdded, uiHooked;

    public Plugin()
    {
        Config = PluginInterface.GetPluginConfig() as Configuration ?? new Configuration();
        Native.Load(PluginInterface.AssemblyLocation.DirectoryName!,
                    Path.Combine(PluginInterface.ConfigDirectory.FullName, "native"));
        Log.Information($"native DLL: {Native.LoadedFrom}");
        try
        {
            Rates = new FrameStats();
            Havok = new HavokChildTable(SigScanner, Log);
            Havok.SetEnabled(Config.HavokChildTable);
            Notifier = new NotifierSkip(SigScanner, Log);
            Notifier.SetEnabled(Config.NotifierSkip);
            TempAlloc = new TempAllocatorFast(SigScanner, Log);
            TempAlloc.SetEnabled(Config.TempAllocator);
            Sort = new CommandSort(SigScanner, Log);
            Sort.SetEnabled(Config.CommandSort);

            mainWindow = new MainWindow(this);
            windows.AddWindow(mainWindow);
            PluginInterface.UiBuilder.Draw += windows.Draw;
            PluginInterface.UiBuilder.OpenMainUi += mainWindow.Toggle;
            PluginInterface.UiBuilder.OpenConfigUi += mainWindow.Toggle;
            uiHooked = true;
            CommandManager.AddHandler(Command, new CommandInfo(OnCommand)
            {
                HelpMessage = "Open the settings window.",
            });
            commandAdded = true;
            Framework.Update += OnFrameworkUpdate;
            frameworkHooked = true;
        }
        catch
        {
            // Dalamud does not call Dispose when the constructor throws: take the patches out here.
            Dispose();
            throw;
        }
    }

    /// <summary>The setting records what was asked for; the patch follows when it can (it may be unavailable).</summary>
    internal void SetNotifier(bool on)
    {
        Config.NotifierSkip = on;
        PluginInterface.SavePluginConfig(Config);
        Notifier.SetEnabled(on);
    }

    internal void SetHavok(bool on)
    {
        Config.HavokChildTable = on;
        PluginInterface.SavePluginConfig(Config);
        Havok.SetEnabled(on);
    }

    internal void SetTempAllocator(bool on)
    {
        Config.TempAllocator = on;
        PluginInterface.SavePluginConfig(Config);
        TempAlloc.SetEnabled(on);
    }

    internal void SetCommandSort(bool on)
    {
        Config.CommandSort = on;
        PluginInterface.SavePluginConfig(Config);
        Sort.SetEnabled(on);
    }

    private void OnFrameworkUpdate(IFramework framework)
    {
        Native.NewFrame();
        Notifier.Tick();
        Rates.Frame();
    }

    private void OnCommand(string command, string args)
    {
        var words = args.Trim().ToLowerInvariant().Split(' ', StringSplitOptions.RemoveEmptyEntries);
        if (words.Length == 0)
        {
            mainWindow.Toggle();
            return;
        }
        bool? Parse(string? w, bool current) => w switch
        {
            "on" => true,
            "off" => false,
            "toggle" or null => !current,
            _ => null,
        };
        var value = words.Length > 1 ? words[1] : null;
        switch (words[0])
        {
            case "havok" when Parse(value, Havok.Installed) is { } on:
                SetHavok(on);
                Chat.Print($"Havok child table: {Havok.Status}", "XIVPerf");
                break;
            case "notifier" when Parse(value, Notifier.Wanted) is { } on:
                SetNotifier(on);
                Chat.Print($"Notifier list: {Notifier.Status}{(Notifier.Available ? "" : " (setting saved for the next start)")}", "XIVPerf");
                break;
            case "alloc" or "allocator" when Parse(value, TempAlloc.Wanted) is { } on:
                SetTempAllocator(on);
                Chat.Print($"TempAllocator: {TempAlloc.Status}{(TempAlloc.Available ? "" : " (setting saved for the next start)")}", "XIVPerf");
                break;
            case "sort" when Parse(value, Sort.Installed) is { } on:
                SetCommandSort(on);
                Chat.Print($"Command sort: {Sort.Status}", "XIVPerf");
                break;
            case "sortverify" when Parse(value, Native.GetSortStats().Verify != 0) is { } on:
                Native.SortSetVerify(on);
                Chat.Print($"Command sort verification {(on ? "on" : "off")}", "XIVPerf");
                break;
            case "verify" when Parse(value, Native.GetStats().Verify != 0) is { } on:
                Native.HavokSetVerify(on);
                Chat.Print($"Havok child table verification {(on ? "on" : "off")}", "XIVPerf");
                break;
            default:
                Chat.PrintError("usage: /xivperf [havok on|off|toggle] [verify on|off] [notifier on|off|toggle] [alloc on|off|toggle] "
                              + "[sort on|off|toggle] [sortverify on|off]", "XIVPerf");
                break;
        }
    }

    public void Dispose()
    {
        if (frameworkHooked)
            Framework.Update -= OnFrameworkUpdate;
        if (commandAdded)
            CommandManager.RemoveHandler(Command);
        if (uiHooked)
        {
            PluginInterface.UiBuilder.Draw -= windows.Draw;
            PluginInterface.UiBuilder.OpenMainUi -= mainWindow.Toggle;
            PluginInterface.UiBuilder.OpenConfigUi -= mainWindow.Toggle;
        }
        windows.RemoveAllWindows();
        try
        {
            Sort?.Dispose();   // points the call back at the game's function
        }
        catch (Exception e)
        {
            Log.Error(e, "removing the command sort patch failed");
        }
        try
        {
            TempAlloc?.Dispose();
        }
        catch (Exception e)
        {
            Log.Error(e, "removing the TempAllocator patch failed");
        }
        finally
        {
            try
            {
                Notifier?.Dispose();   // puts the skipped nodes back, then the original code
            }
            catch (Exception e)
            {
                Log.Error(e, "removing the Notifier patch failed");
            }
            finally
            {
                Havok?.Dispose();      // restores the game's code; the native DLL stays loaded
            }
        }
    }
}

/// <summary>Native counters turned into per-frame rates over the last second.</summary>
internal sealed class FrameStats
{
    private readonly Stopwatch clock = Stopwatch.StartNew();
    private Native.Stats last = Native.GetStats();
    private ulong lastLinks = Links(Native.GetNotifierStats());
    private Native.AllocStats lastAlloc = Native.GetAllocStats();
    private Native.SortStats lastSort = Native.GetSortStats();
    private int frames;

    public double Fast { get; private set; }
    public double Fallback { get; private set; }
    public double Propagate { get; private set; }
    public double LinksPerFrame { get; private set; }
    public double AllocFast { get; private set; }
    public double AllocFallback { get; private set; }
    public double SortCalls { get; private set; }
    public double SortRecords { get; private set; }
    public double FrameMs { get; private set; }

    private static ulong Links(Native.NotifierStats s) => s.LinksSkipped + s.LinksPassed;

    // The Havok counters are plain increments from several threads and can step back by a little.
    private double Rate(ulong now, ulong before) => now >= before ? (now - before) / (double)frames : 0;

    public void Frame()
    {
        frames++;
        var elapsed = clock.Elapsed.TotalSeconds;
        if (elapsed < 1.0)
            return;
        var now = Native.GetStats();
        var links = Links(Native.GetNotifierStats());
        var alloc = Native.GetAllocStats();
        var sort = Native.GetSortStats();
        Fast = Rate(now.Fast, last.Fast);
        Fallback = Rate(now.Fallback, last.Fallback);
        Propagate = Rate(now.Propagate, last.Propagate);
        LinksPerFrame = Rate(links, lastLinks);
        AllocFast = Rate(alloc.FastAllocs, lastAlloc.FastAllocs);
        AllocFallback = Rate(alloc.FallbackAllocs, lastAlloc.FallbackAllocs);
        SortCalls = Rate(sort.Calls, lastSort.Calls);
        SortRecords = Rate(sort.Records, lastSort.Records);
        FrameMs = elapsed * 1000.0 / frames;
        last = now;
        lastLinks = links;
        lastAlloc = alloc;
        lastSort = sort;
        frames = 0;
        clock.Restart();
    }
}
