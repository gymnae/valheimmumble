Positional audio for Valheim in Mumble, with a walkie-talkie radio effect for distant players. Players keep a vanilla game; they only install a Mumble plugin.

## Downloads

| File | Who installs it |
|---|---|
| `ValheimMumbleRadioServer.dll` | The server admin. Copy it to `BepInEx/plugins/` on the Valheim dedicated server (BepInExPack Valheim required). |
| `valheim_radio.mumble_plugin` | Every player. Open it with Mumble 1.5 or later. |
| `radio_demo_0-100m.wav` | Optional. A demo of the effect from 0 to 100 m. |

The server mod is built automatically against the current Valheim dedicated server. If a game update breaks it, rebuild it yourself: see [Building](https://github.com/gymnae/valheimmumble#building).

## Setup in short

1. The server admin installs BepInEx and the server mod, sets a `Key` in `BepInEx/config/de.grundstil.valheim.mumbleradio.server.cfg`, and opens TCP port 47812.
2. In the Mumble channel description, add `valheim-radio=your.server.address:47812/yourkey`.
3. Players install the plugin. Your Mumble name must match your character name, or set `name` in the plugin's ini file.

See the [README](https://github.com/gymnae/valheimmumble#readme) for the full details.
