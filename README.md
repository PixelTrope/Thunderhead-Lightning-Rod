# Thunderhead-Lightning-Rod
Claude coded Bryce 3D render farm management and control software. Thunderhead (host) and Lightning Rod (clients)

## Layout

| Path                | Contents                                                        |
|---------------------|-----------------------------------------------------------------|
| `docs/PROTOCOL.md`  | wire protocol, ports, file formats                              |
| `src/common`        | message encoding, sockets, logging, string helpers (static lib) |
| `src/LightningRod`  | node agent: discovery, reports, Lightning start/stop            |
| `src/Thunderhead`   | host console: Win32 window, node table, node database, server   |

## Build

Visual Studio 2022 with the C++ workload and CMake 3.21 or later.

Windows 7 and later targets, 32-bit, static runtime:

```
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
```

Windows XP SP3 agent, same code on the v141_xp toolset (needs the
"C++ Windows XP Support for VS 2017 (v141) tools" and "MSVC v141" components):

```
cmake -S . -B build-xp -G "Visual Studio 17 2022" -A Win32 -T v141_xp
cmake --build build-xp --config Release --target LightningRod
```

The XP build of `LightningRod.exe` runs on every node from XP SP3 up and needs
no runtime installed, so it is the one to deploy.

## Deploy (version 1)

Copy `LightningRod.exe` to `C:\LANCMD` on each node and start it at logon with
a shortcut in the Startup folder or a Run registry entry. Thunderhead runs from
any folder; it writes `settings.txt`, `nodes.txt` and `logs\` beside itself.
