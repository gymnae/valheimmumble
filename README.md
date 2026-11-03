# Valheim Positional Audio + Radio for Mumble (vanilla game clients)

Positional audio for Valheim in Mumble. **Players keep an unmodified game.** They only install a
plugin in Mumble. Players who are close sound like they are standing next to you. Further away,
their voice gradually turns into a walkie-talkie: band-limited, slightly distorted and compressed,
with static, crackle and a squelch burst. Radio voices stay audible at any distance instead of fading
out.

| Part | Installed by | Where |
|---|---|---|
| `ValheimMumbleRadioServer.dll` (BepInEx mod) | server admin | Valheim **dedicated server** (or the PC hosting a world) |
| `valheim_radio.mumble_plugin` | every player | Mumble ≥ 1.5 |

## How it works

```
Valheim server + mod ──TCP 47812, ~15×/s: every player's name, position, facing──▶ each player's Mumble plugin
                                                                                       │
            Mumble channel description: "valheim-radio=host:port/key" ──(discovery)──▶ │
                                                                                       ├─▶ own position ▶ Mumble 3D audio
                                                                                       └─▶ others' distance ▶ radio effect
```

- **Positions come from the server.** The dedicated server already knows every player's position
  (their character's ZDO). The mod streams all of them to every connected Mumble plugin.
- **Players are matched by name.** Your Mumble username must equal your character name (case doesn't
  matter). If it doesn't, set `name` in the plugin's ini file. The plugin then announces your character
  name to the other players' plugins.
- **Zero-config discovery.** The admin puts a tag like `valheim-radio=vh.example.com:47812/secret` into
  the Mumble channel description, or the root channel's. Plugins in that channel find the server
  automatically.
- **Radio settings come from the server**, so everyone gets the same distances. Each player can override
  them locally.
- **The effect** works on each speaker's voice before Mumble's 3D panning, so radio voices still come
  from the right direction.
  - Between `StartDistance` and `FullDistance` the voice morphs continuously: the band-pass narrows from
    20 Hz–20 kHz to 350 Hz–2.8 kHz, and a 1.7 kHz "honk", saturation and static fade in. Changes are
    smoothed over about 0.35 s.
  - With `KeepVolume` (default), the plugin replaces Mumble's distance fade with its own: full volume up
    to Mumble's *Minimum Distance*, a gentle fade down to `RadioVolume` at `StartDistance`, then
    `RadioVolume` whatever the distance. Mumble's *Maximum Distance* then barely matters.

## Server admin setup

1. Install [BepInExPack Valheim](https://thunderstore.io/c/valheim/p/denikson/BepInExPack_Valheim/) on the
   dedicated server.
2. Build the mod against your server: see [Building](#building). Copy `ValheimMumbleRadioServer.dll` to
   `BepInEx/plugins/` and start the server once.
3. Edit `BepInEx/config/de.grundstil.valheim.mumbleradio.server.cfg`:
   - `Key`: set a secret. Anyone with the key can see where every player is.
   - `Port`: default 47812. **Open this TCP port** in your firewall or router.
   - `[Radio]`: distances and the character of the effect.
4. In Mumble, edit the channel your group uses, or the root channel. Add a line to the description:

   `valheim-radio=your.server.address:47812/yourkey`

   Keep the description short (under about 128 characters). Mumble only sends longer descriptions to a
   client after someone has opened them, so the plugin might not see the tag.

The mod doesn't need to be installed on clients, and vanilla clients can join.

## Player setup

1. Mumble ≥ 1.5: *Settings → Plugins → Install plugin…*, pick `valheim_radio.mumble_plugin`, enable
   **Valheim** and tick *Positional audio* for it. *Link to game and transmit position* must be on.
2. *Settings → Audio Output → Positional Audio*:
   - enable it. You need stereo output. A Bluetooth headset in hands-free (mic) mode is mono, and
     Mumble skips 3D audio on mono output. Tick *Headphones* if you use them;
   - recommended values (they make Mumble's fade match the plugin's, so nothing needs correcting):

     | Setting | Value |
     |---|---|
     | Minimum Distance | 2 m (full volume this close) |
     | Maximum Distance | 30 m (the server's `StartDistance`) |
     | Minimum Volume | 70 % (the server's `RadioVolume`) |
     | Bloom | leave as is |

     If the server admin changes `StartDistance` or `RadioVolume`, change these to match. The Mumble
     log prints your settings and the server's values when you're linked.

     Other values work too, because the plugin corrects Mumble's fade (`KeepVolume`). But **Minimum
     Volume must never be 0**, or Mumble mutes far voices before they can turn into radio. Below about
     `RadioVolume` ÷ `MaxBoost` (18 % by default), far voices get quieter than intended. The log warns
     about both.
3. *Settings → Audio Input → Transmit position* must be on, or nobody hears *you* in 3D.
4. Use your character name as your Mumble name, or set it in the ini (below).
5. Join the channel and load into the world. Mumble's log shows `Valheim Radio: found you ("name") in the
   game - positional audio active`.

### Optional ini

The plugin creates a commented template on first start:

- **Windows:** `%APPDATA%\Mumble\valheim_radio.ini`
- **Linux:** `~/.config/Mumble/valheim_radio.ini`

Edits apply live.

```ini
[server]
address = vh.example.com:47812   ; instead of the channel tag
key = secret
[player]
name = Ragnar                    ; your character, if your Mumble name differs
[radio]
start = 40                       ; any radio setting, for you only
squelch = off
```

## Building

**Server mod** (needs the .NET SDK and a Valheim install with BepInEx; either the dedicated server or the
game works):

```sh
cd server-mod
dotnet build -c Release -p:ValheimDir="/path/to/Valheim dedicated server"
# -> bin/Release/ValheimMumbleRadioServer.dll
```

**Mumble plugin** (C++17; the only dependency is the vendored `include/MumblePlugin.h`, BSD license).
For your own machine:

```sh
cd mumble-plugin
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --config Release
```

The release bundle in `dist/` holds builds for Windows (x64, x86, arm64), Linux (x64, x86, arm64,
armv7) and macOS (universal: x64 + arm64). All are cross-compiled from Linux with zig
(`pip install ziglang`):

```sh
mumble-plugin/tools/build_all.sh dist/valheim_radio.mumble_plugin
```

**Tests:** run `tests/run_tests.sh` (Linux). It covers:

- the plugin end to end, with a fake Mumble API and a fake Valheim server: discovery, wrong key, name
  matching and announcements, radio effect, unlinking;
- a radio render;
- the server mod's real C# serializer against the plugin's C++ parser.

**Hearing the effect:** `radio_demo out.wav [voice.wav]` renders a voice walking from 0 to 100 m. See
`dist/radio_demo_0-100m.wav` (synthetic voice).

## Limitations

- **Listener direction** is the character's facing, not the camera: the server doesn't know the camera.
- **Name matching:** non-ASCII letters match case-sensitively. Two players with the same character name
  will be confused.
- **Positions lag** about 100–200 ms (client → server → plugin). That's fine for voice.
- **Privacy:** positions of everyone on the server go to every plugin that knows the key. On PvP servers
  that is a potential radar.
- **KeepVolume** assumes Mumble actually faded the voice by distance: positional audio is on in the
  listener's Mumble *and* the speaker transmits position. If not, distant voices get up to `MaxBoost`
  louder.
- **Effect scope:** only users in your own Mumble channel get the radio effect, not users heard through
  linked channels or channel listeners.
