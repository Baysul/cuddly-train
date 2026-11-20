using Dalamud.Configuration;

namespace XIVPerf;

public sealed class Configuration : IPluginConfiguration
{
    public int Version { get; set; } = 1;
    public bool HavokChildTable { get; set; } = true;
    public bool NotifierSkip { get; set; } = true;
    public bool TempAllocator { get; set; } = true;
}
