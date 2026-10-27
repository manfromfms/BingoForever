# BingoForever

> **VIBECODED SOFTWARE WARNING**
>
> This mod is vibecoded. 

> **BETA SOFTWARE WARNING**
>
> This mod is in beta. Expect bugs, crashes, protocol changes, and lost match
> state. Back up your `TwinkieModloader.dll` and game files before using it.
> Both players must run the same build. Do not use this for competitive events
> without testing it first.

BingoForever is a fork of [TwinkieForever](https://github.com/TwinkieTweaks/TwinkieForever)
for TrackMania Nations Forever / United Forever. It keeps the Twinkie module
system and adds a multiplayer **Bingo** module: teams race maps from a shared
board and claim squares by setting the fastest time.

## Bingo features

- Host/client play over TCP (default port `42123`).
- 2 to 8 teams, with team selection/changes in the lobby.
- Configurable board size from 3x3 to 7x7 (default 5x5).
- One record per square; the fastest time owns it.
- Empty squares are claimed by finishing the map, optionally subject to a
  Bronze/Silver/Gold/Author medal requirement.
- Claimed squares can only be taken with a strictly faster time.
- Configurable match timer (default 90 minutes, `0` disables it).
- Start-of-match grace period (default 5 minutes); claims count during grace,
  but no line can win until it ends.
- On timeout, the team with the most occupied squares wins; ties are draws.
- Click a square to launch its map with `/useexedir /singleinst /file=...`.
- Hover a square to see the target time: the previous player's record, or the
  medal requirement for an empty square.
- Claim/reclaim, join, team change, grace, time-left, and winner notifications.
- Player names come from the active local profile; optional Nadeo-login check
  for host/join.

## Requirements

- Windows, with a 32-bit TrackMania Nations Forever or United Forever install.
- [Trackmania Modloader](https://tomashu.dev/software/tmloader).
- The x86 Microsoft Visual C++ 2015-2022 Redistributable.
- The same BingoForever/Twinkie build on every player's machine.

## Installation

1. Install the Trackmania Modloader if you do not already use it.
2. Install Twinkie normally, or set up the Modloader manually:
   - Put `TwinkieModloader.dll`, `LuaEngine2.dll`, and `lua54.dll` in the
     TrackMania game folder.
   - Make sure `ModLoader.ini` contains entries for those files, for example:
     ```ini
     LuaEngine2=LuaEngine2.dll
     lua54.dll=lua54.dll
     TwinkieModloader.dll=TwinkieModloader.dll
     ```
3. Start the game and enable the **Bingo** module in Twinkie settings.
4. Configure the map folder, player settings, and port in
   **Twinkie Settings -> Bingo**.

## How to play

1. The host opens **Twinkie Settings -> Bingo** and clicks **Host**.
2. Other players enter the host IP and click **Join**.
3. Pick teams in the lobby.
4. The host generates a board and starts the match.
5. Players click board squares to load maps, then finish them to claim or
   reclaim squares.
6. A full row, column, or diagonal after the grace period wins the match, or
   the team with the most squares wins on timeout.

For internet play, the host's TCP port (default `42123`) must be reachable.
If you are behind a router, use port forwarding or a VPN/LAN.

## Building

Use a **MSVC-compatible toolchain** (Visual Studio 2022, x86). Do not build
this project with MinGW: the prebuilt `LuaEngine2.dll` is MSVC-built and
imports Twinkie C++ symbols, and MinGW's C++ ABI is incompatible.

Open `Twinkie.sln`, select a 32-bit configuration, and build
`TwinkieModloader.dll`.

## Credits

- Original Twinkie / TwinkieForever by the TwinkieTweaks authors and
  contributors.
- MinHook by Tsuda Kageyu.
- Dear ImGui by Omar Cornut and contributors.
- Lua and the LuaEngine components by their respective authors.
- Openplanet ManiaIcons by the Openplanet team.

## License

MIT. See [LICENSE](LICENSE).
