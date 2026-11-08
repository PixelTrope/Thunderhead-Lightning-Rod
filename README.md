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

## Agent start-up and the network

A node that starts the agent at logon may not have its DHCP lease yet, so the
agent can read no address, 127.x or an APIPA 169.254.x.y address. From 0.2.15
it then waits 30 seconds and reads the address again, up to four times, and
shows the count in its window instead of announcing from the bad address. When
a valid address appears, Lightning, which was started at logon as well and most
likely bound the same bad address, is stopped and started again if it is
running. After the fourth failed re-read the agent shows "Network Error:
Invalid IP address. Check network settings or hardware and restart Lightning
Rod." with the error sound; OK starts a fresh agent, which begins the same
checks again. A loose cable gives the same symptom and is outside what the
agent can do.

## Deploy (version 1)

Copy `LightningRod.exe` to `C:\LightningRod` on each node, share that folder as
`LightningRod`, and start the agent at logon with a shortcut in the Startup folder
or a Run registry entry. Thunderhead runs from any folder; it writes
`settings.txt`, `nodes.txt` and `logs\` beside itself, and pushes the
`LightningRod.exe` placed beside `Thunderhead.exe` when *Update checked* is used.
