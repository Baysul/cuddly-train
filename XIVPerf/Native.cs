using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;

namespace XIVPerf;

/// <summary>xivperf_native.dll (plugin/native/xivperf_native.c). The DLL is never unloaded: game threads may still be
/// inside its code after a patch is removed.</summary>
internal static unsafe class Native
{
    public const int ExpectedVersion = 5;
    public const int ErrorWait = -9;

    [StructLayout(LayoutKind.Sequential)]
    public struct Stats
    {
        public ulong Fast, Fallback, Propagate, Verified, Mismatches, Tables, Rebuilds, Bytes, Evictions;
        public int Installed, Verify, LastError, Version;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct NotifierStats
    {
        public ulong LinksSkipped, LinksPassed, Unlinks, Checked, Flips, Stale, LastFlipNode;
        public int Skipped, ListedAtEnable, IdleAtEnable, WorkingAtEnable, UnrecognisedAtEnable, Installed, Active,
                   LastError, LastFlipGate, Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct AllocStats
    {
        public ulong FastAllocs, FastFrees, FallbackAllocs, FallbackFrees, CentralHits, CentralMisses, ArenaFull, Corrupt;
        public long LiveBlocks, LiveBytes;
        public ulong CommittedBytes;
        public int SlabsUsed, SlabsMax, ActiveThreads, ExitedThreads, Installed, Active, ChainDepth, LastError;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct SortStats
    {
        public ulong Calls, Records, Passes, ExtraCopies, Small, Fallbacks, Verified, Mismatches;
        public int Installed, Verify, LastError, Version;
    }

    private static delegate* unmanaged<int> version;
    private static delegate* unmanaged<nint, nint, int> havokInstall;
    private static delegate* unmanaged<int> havokUninstall;
    private static delegate* unmanaged<int, void> havokSetVerify;
    private static delegate* unmanaged<void> newFrame;
    private static delegate* unmanaged<Stats*, void> getStats;
    private static delegate* unmanaged<nint, nint, nint, nint, nint, nint, int> notifierInstall;
    private static delegate* unmanaged<int> notifierUninstall;
    private static delegate* unmanaged<int, int> notifierSetActive;
    private static delegate* unmanaged<int, void> notifierWatch;
    private static delegate* unmanaged<NotifierStats*, void> notifierGetStats;
    private static delegate* unmanaged<nint, nint, nint, nint, nint, nint, int> allocInstall;
    private static delegate* unmanaged<int, int> allocSetActive;
    private static delegate* unmanaged<int> allocUninstall;
    private static delegate* unmanaged<AllocStats*, void> allocGetStats;
    private static delegate* unmanaged<nint, nint, int> sortInstall;
    private static delegate* unmanaged<int> sortUninstall;
    private static delegate* unmanaged<int, void> sortSetVerify;
    private static delegate* unmanaged<SortStats*, void> sortGetStats;

    public static string? LoadedFrom { get; private set; }

    /// <summary>Loads a content-named copy of the DLL, so rebuilding the plugin never rewrites a file the game has
    /// mapped. The same build is the same module: after a plugin reload its caches and counters are still there (the
    /// patches themselves are removed on unload and installed again).</summary>
    public static void Load(string pluginDir, string copyDir)
    {
        var bytes = File.ReadAllBytes(Path.Combine(pluginDir, "xivperf_native.dll"));
        var name = $"xivperf_native_{Convert.ToHexString(SHA256.HashData(bytes))[..16].ToLowerInvariant()}.dll";
        Directory.CreateDirectory(copyDir);
        var copy = Path.Combine(copyDir, name);
        if (!File.Exists(copy))
        {
            var tmp = copy + ".tmp";
            File.WriteAllBytes(tmp, bytes);
            File.Move(tmp, copy, true);
        }
        var lib = NativeLibrary.Load(copy);
        version = (delegate* unmanaged<int>)NativeLibrary.GetExport(lib, "xp_version");
        if (version() != ExpectedVersion)
            throw new InvalidOperationException($"{name} is version {version()}, expected {ExpectedVersion}");
        havokInstall = (delegate* unmanaged<nint, nint, int>)NativeLibrary.GetExport(lib, "xp_havok_install");
        havokUninstall = (delegate* unmanaged<int>)NativeLibrary.GetExport(lib, "xp_havok_uninstall");
        havokSetVerify = (delegate* unmanaged<int, void>)NativeLibrary.GetExport(lib, "xp_havok_set_verify");
        newFrame = (delegate* unmanaged<void>)NativeLibrary.GetExport(lib, "xp_new_frame");
        getStats = (delegate* unmanaged<Stats*, void>)NativeLibrary.GetExport(lib, "xp_get_stats");
        notifierInstall = (delegate* unmanaged<nint, nint, nint, nint, nint, nint, int>)NativeLibrary.GetExport(lib, "xp_notifier_install");
        notifierUninstall = (delegate* unmanaged<int>)NativeLibrary.GetExport(lib, "xp_notifier_uninstall");
        notifierSetActive = (delegate* unmanaged<int, int>)NativeLibrary.GetExport(lib, "xp_notifier_set_active");
        notifierWatch = (delegate* unmanaged<int, void>)NativeLibrary.GetExport(lib, "xp_notifier_watch");
        notifierGetStats = (delegate* unmanaged<NotifierStats*, void>)NativeLibrary.GetExport(lib, "xp_notifier_get_stats");
        allocInstall = (delegate* unmanaged<nint, nint, nint, nint, nint, nint, int>)NativeLibrary.GetExport(lib, "xp_alloc_install");
        allocSetActive = (delegate* unmanaged<int, int>)NativeLibrary.GetExport(lib, "xp_alloc_set_active");
        allocUninstall = (delegate* unmanaged<int>)NativeLibrary.GetExport(lib, "xp_alloc_uninstall");
        allocGetStats = (delegate* unmanaged<AllocStats*, void>)NativeLibrary.GetExport(lib, "xp_alloc_get_stats");
        sortInstall = (delegate* unmanaged<nint, nint, int>)NativeLibrary.GetExport(lib, "xp_sort_install");
        sortUninstall = (delegate* unmanaged<int>)NativeLibrary.GetExport(lib, "xp_sort_uninstall");
        sortSetVerify = (delegate* unmanaged<int, void>)NativeLibrary.GetExport(lib, "xp_sort_set_verify");
        sortGetStats = (delegate* unmanaged<SortStats*, void>)NativeLibrary.GetExport(lib, "xp_sort_get_stats");
        LoadedFrom = copy;
    }

    public static int HavokInstall(nint accessBoneModelSpace, nint calculateBoneModelSpace) =>
        havokInstall(accessBoneModelSpace, calculateBoneModelSpace);

    public static int HavokUninstall() => havokUninstall();

    public static void HavokSetVerify(bool on) => havokSetVerify(on ? 1 : 0);

    public static void NewFrame() => newFrame();

    public static Stats GetStats()
    {
        Stats s;
        getStats(&s);
        return s;
    }

    public static int NotifierInstall(nint link, nint unlink, nint textStart, nint textEnd, nint imageStart, nint imageEnd) =>
        notifierInstall(link, unlink, textStart, textEnd, imageStart, imageEnd);

    public static int NotifierUninstall() => notifierUninstall();

    public static int NotifierSetActive(bool on) => notifierSetActive(on ? 1 : 0);

    public static void NotifierWatch(int count) => notifierWatch(count);

    public static NotifierStats GetNotifierStats()
    {
        NotifierStats s;
        notifierGetStats(&s);
        return s;
    }

    public static int AllocInstall(nint tempAlloc, nint hybridVtbl, nint heapVtbl, nint poolVtbl, nint textStart, nint textEnd) =>
        allocInstall(tempAlloc, hybridVtbl, heapVtbl, poolVtbl, textStart, textEnd);

    public static int AllocSetActive(bool on) => allocSetActive(on ? 1 : 0);

    public static int AllocUninstall() => allocUninstall();

    public static AllocStats GetAllocStats()
    {
        AllocStats s;
        allocGetStats(&s);
        return s;
    }

    public static int SortInstall(nint deviceGather, nint contextGather) => sortInstall(deviceGather, contextGather);

    public static int SortUninstall() => sortUninstall();

    public static void SortSetVerify(bool on) => sortSetVerify(on ? 1 : 0);

    public static SortStats GetSortStats()
    {
        SortStats s;
        sortGetStats(&s);
        return s;
    }

    public static string Describe(int code) => code switch
    {
        0 => "ok",
        -1 => "bad address",
        -2 => "the function's code differs from the build this patch was written for (another hook, or a game update)",
        -3 => "calculateBoneModelSpace differs from the one the function calls",
        -4 => "no free memory within 2 GB of the function for the jump stub",
        -5 => "VirtualProtect failed",
        -6 => "the code changed while patching",
        -7 => "invalid state for this operation",
        -8 => "Notifier.Link and Unlink disagree on the list globals",
        -9 => "turning on in a moment",
        -10 => "out of memory",
        -11 => "TempAllocator hot-reload vtable chain limit reached (restart the game)",
        _ => $"error {code}",
    };
}
