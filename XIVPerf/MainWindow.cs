using Dalamud.Bindings.ImGui;
using Dalamud.Interface.Windowing;

namespace XIVPerf;

internal sealed class MainWindow : Window
{
    private readonly Plugin plugin;

    public MainWindow(Plugin plugin) : base("XIVPerf")
    {
        this.plugin = plugin;
        SizeConstraints = new WindowSizeConstraints { MinimumSize = new(380, 200) };
    }

    public override void Draw()
    {
        var stats = Native.GetStats();
        var rates = plugin.Rates;

        var havok = stats.Installed != 0;
        if (ImGui.Checkbox("Havok child table", ref havok))
            plugin.SetHavok(havok);
        ImGui.SameLine();
        ImGui.TextDisabled(plugin.Havok.Status);
        ImGui.TextWrapped("hkaPose::accessBoneModelSpace finds a bone's children through a table instead of scanning "
                        + "every bone. Same results; SimpleHeels' LivePose calls it for every bone of every posed character.");

        var verify = stats.Verify != 0;
        if (ImGui.Checkbox("Check every call against the original scan (slow)", ref verify))
            Native.HavokSetVerify(verify);

        ImGui.Separator();
        ImGui.TextUnformatted($"Per frame, last second ({rates.FrameMs:0.0} ms/frame):");
        ImGui.TextUnformatted($"  {rates.Fast:0} calls from the table, {rates.Fallback:0} to the original, "
                            + $"{rates.Propagate:0} propagating (original)");
        ImGui.TextUnformatted($"Checked {stats.Verified:N0} calls, {stats.Mismatches} disagreed");
        ImGui.TextUnformatted($"{stats.Tables} tables built, {stats.Rebuilds} rebuilt, {stats.Evictions} evicted; "
                            + $"{stats.Bytes / 1024.0:0.0} KiB");

        ImGui.Separator();
        var n = plugin.Notifier;
        var ns = Native.GetNotifierStats();
        var notifier = n.Wanted;
        if (!n.Available)
            ImGui.BeginDisabled();
        if (ImGui.Checkbox("Notifier list: skip idle resources", ref notifier))
            plugin.SetNotifier(notifier);
        if (!n.Available)
            ImGui.EndDisabled();
        ImGui.SameLine();
        ImGui.TextDisabled(n.Status);
        ImGui.TextWrapped("Textures and buffers whose per-frame callbacks can never do anything stay off the list "
                        + "PostTick walks twice a frame.");
        ImGui.TextUnformatted($"{ns.Skipped:N0} off the list now; new: {ns.LinksSkipped:N0} kept off, "
                            + $"{ns.LinksPassed:N0} listed ({rates.LinksPerFrame:0.0} per frame)");
        ImGui.TextUnformatted($"Re-checked {ns.Checked:N0}; {ns.Flips} became active and were put back, {ns.Stale} dropped");

        ImGui.Separator();
        var a = plugin.TempAlloc;
        var asStats = Native.GetAllocStats();
        var tempAlloc = a.Wanted;
        if (!a.Available)
            ImGui.BeginDisabled();
        if (ImGui.Checkbox("TempAllocator: per-thread slab cache", ref tempAlloc))
            plugin.SetTempAllocator(tempAlloc);
        if (!a.Available)
            ImGui.EndDisabled();
        ImGui.SameLine();
        ImGui.TextDisabled(a.Status);
        ImGui.TextWrapped("Serves Client.Graphics.Temp.Allocator requests (<=16 KiB, <=16-byte aligned) from per-thread "
                        + "slab magazines, removing the 15-thread CRITICAL_SECTION convoy during RenderView.");
        var centralTotal = asStats.CentralHits + asStats.CentralMisses;
        var hitPct = centralTotal > 0 ? 100.0 * asStats.CentralHits / centralTotal : 0.0;
        ImGui.TextUnformatted($"Per frame: {rates.AllocFast:N0} fast, {rates.AllocFallback:N0} fallback "
                            + $"(live: {asStats.LiveBlocks:N0} blocks, {asStats.LiveBytes / 1048576.0:0.0} MiB)");
        ImGui.TextUnformatted($"Slabs: {asStats.SlabsUsed:N0}/{asStats.SlabsMax:N0} ({asStats.CommittedBytes / 1048576.0:0.0} MiB); "
                            + $"central hit {hitPct:0.0}%; threads {asStats.ActiveThreads} active, {asStats.ExitedThreads} exited");
        if (asStats.ArenaFull > 0 || asStats.ChainDepth > 1 || asStats.Corrupt > 0)
            ImGui.TextDisabled($"Arena-full fallbacks: {asStats.ArenaFull:N0}; reload chain depth: {asStats.ChainDepth}; corrupt links dropped: {asStats.Corrupt:N0}");

        ImGui.Separator();
        var s = plugin.Sort;
        var ss = Native.GetSortStats();
        var sort = ss.Installed != 0;
        if (!s.Available)
            ImGui.BeginDisabled();
        if (ImGui.Checkbox("Command sort: radix", ref sort))
            plugin.SetCommandSort(sort);
        if (!s.Available)
            ImGui.EndDisabled();
        ImGui.SameLine();
        ImGui.TextDisabled(s.Status);
        ImGui.TextWrapped("Context.GatherCommands copies each context's render commands once instead of twice and sorts "
                        + "them with a stable radix sort instead of a merge sort. Same commands in the same order.");
        var sortVerify = ss.Verify != 0;
        if (ImGui.Checkbox("Check every sort against the game's own (slow)", ref sortVerify))
            Native.SortSetVerify(sortVerify);
        var passes = ss.Calls > ss.Small ? (double)ss.Passes / (ss.Calls - ss.Small) : 0;
        ImGui.TextUnformatted($"Per frame: {rates.SortCalls:0.0} lists, {rates.SortRecords:N0} commands; "
                            + $"{passes:0.0} passes per list, {ss.ExtraCopies:N0} extra copies, {ss.Fallbacks} to the original");
        ImGui.TextUnformatted($"Checked {ss.Verified:N0} sorts, {ss.Mismatches} disagreed");

        ImGui.Separator();
        ImGui.TextDisabled($"Native DLL: {Native.LoadedFrom}");
    }
}
