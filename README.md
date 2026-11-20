# XIVPerf

A Dalamud plugin that improves framerate in crowded areas by speeding up a few slow CPU paths in the game client.

## How it works

When loaded, the plugin replaces a small set of slow routines in the game's animation, rendering, and memory handling with faster equivalents that produce the same results. Every change verifies the game build before activating, turns itself off safely after a game update, and can be toggled in-game with `/xivperf`.

## Installing

1. In-game, open `/xlsettings` → **Experimental** → **Custom Plugin Repositories**.
2. Add:
   ```
   https://raw.githubusercontent.com/Baysul/dalamud-plogons/main/pluginmaster.json
   ```
3. Save, open `/xlplugins`, and install **XIVPerf**.
