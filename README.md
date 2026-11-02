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
  - With `KeepVolume`, the plugin undoes Mumble's distance fade for radio voices. They then play at
    `RadioVolume` whatever the distance.

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
   **Valheim** and tick *Positional audio* for it.
2. *Settings → Audio Output → Positional Audio*:
   - enable it (you need stereo output or headphones);
   - recommended values:

     | Setting | Value |
     |---|---|
     | Minimum Distance | 2 m |
     | Maximum Distance | 60 m (the server's `FullDistance`) |
     | Minimum Volume | 30 % |

     **Minimum Volume must be above 0**, otherwise far voices are muted before they can turn into radio.
3. *Settings → Audio Input → Transmit position* must be on.
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

**Mumble plugin** (C++17; the only dependency is the vendored `include/MumblePlugin.h`, BSD license). The
prebuilt bundle in `dist/` was cross-compiled with zig:

```sh
cd mumble-plugin
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --config Release
# or from Linux for both OSes (pip install ziglang):
SRC="src/plugin.cpp src/RadioFx.cpp src/Settings.cpp"
python -m ziglang c++ -target x86_64-windows-gnu -std=c++17 -O2 -s -shared -fvisibility=hidden \
    -Iinclude -Isrc $SRC -lws2_32 -o valheim_radio.dll
python -m ziglang c++ -target x86_64-linux-gnu.2.28 -std=c++17 -O2 -s -shared -fPIC -fvisibility=hidden \
    -Iinclude -Isrc $SRC -o libvalheim_radio.so
python3 tools/package.py ../dist/valheim_radio.mumble_plugin --windows valheim_radio.dll --linux libvalheim_radio.so
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
- **KeepVolume** assumes positional audio is active in the listener's Mumble. If it's off, radio voices
  get up to `MaxBoost` louder.
- **Effect scope:** only users in your own Mumble channel get the radio effect, not users heard through
  linked channels or channel listeners.
