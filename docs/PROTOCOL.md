# Thunderhead / Lightning Rod protocol, version 1

Thunderhead is the host console. Lightning Rod is the node agent. All traffic
is plain TCP and UDP on the LAN; no file sharing is used at run time.

## Ports

| Purpose                                   | Transport | Port                      |
|-------------------------------------------|-----------|---------------------------|
| Agent announcement, Thunderhead listens   | UDP       | 42001 (fixed)             |
| Agent connection, Thunderhead listens     | TCP       | 42002 (Thunderhead setting, advertised in OFFER) |

The agent uses ephemeral ports for both sockets, so no inbound rule is needed on
a node. Thunderhead needs inbound UDP 42001 and inbound TCP on its configured
port.

## Message format

A message is UTF-8 text. The first line is the verb. Each following line is
`key=value`. An empty line ends the message. Line endings are `\n`; `\r\n` is
accepted. Values contain no line breaks. A message is at most 65536 bytes.

```
REPORT
cpu_pct=12
cpu_temp_c=41.5
mem_free_mb=2900

```

## Discovery (UDP)

While the agent has no TCP connection it broadcasts every 5 seconds, to
255.255.255.255 and to the directed broadcast address of each adapter:

```
ANNOUNCE
mac=00-80-64-BF-49-77
name=WIN7E0007
agent=0.1.0
```

Thunderhead answers by unicast to the sender's address and port:

```
OFFER
host=UR
port=42002
```

The agent connects by TCP to the address the OFFER came from, at the port it
names.

## Session (TCP)

The agent sends HELLO first. The static fields are gathered once at agent start.

```
HELLO
mac=00-80-64-BF-49-77
name=WIN7E0007
agent=0.1.0
ip=42.32.22.107
os=Windows Embedded Standard Service Pack 1 (6.1.7601)
cpu=AMD G-T56N Processor
cpu_mhz=1650
cores=2
mem_mb=3892
disk_total_mb=3889
board=Dell Inc. 0XXXXX
chipset=AMD A55E Host Bridge
gpu=AMD Radeon HD 6310 Graphics
vram_mb=384
```

Thunderhead replies with the report interval in seconds (5 to 60 in steps of 5,
default 15):

```
SET
interval=15
```

The agent then sends a REPORT every `interval` seconds. Only values that change
during a session are in it; everything fixed for the session is in HELLO and
is never repeated, and Thunderhead never polls. The first REPORT after HELLO
carries every value; each later REPORT carries only the values that differ from
the previous REPORT and may carry none, in which case the bare verb still
counts as a report for the offline rule. Thunderhead merges each REPORT into
the values it holds for the node. A full first report:

```
REPORT
cpu_pct=12
cpu_temp_c=41.5
mem_free_mb=2900
disk_free_mb=1040
uptime_s=12345
lightning=running
lightning_cpu_pct=97
rendering=yes
```

A later report when only the load figures moved:

```
REPORT
cpu_pct=14
uptime_s=12360
lightning_cpu_pct=96
```

`lightning` is `running` or `stopped`. `rendering` is `yes` when Lightning.exe
owns an established TCP connection (a job from the Bryce host), otherwise `no`.
`lightning_cpu_pct` is Lightning's share of the whole machine over the last
interval.
`cpu_temp_c` is the hottest ACPI thermal zone (WMI class
`MSAcpi_ThermalZoneTemperature` in `root\WMI`) in degrees Celsius with one
decimal, or empty when the node exposes no zone; Thunderhead shows it in
Celsius and Fahrenheit, or `n/a` when it is empty. The zone is whatever the
board firmware reports and is not always the processor itself.

Thunderhead may send at any time:

| Verb  | Fields        | Agent action                                           |
|-------|---------------|--------------------------------------------------------|
| SET   | interval      | change the report interval                             |
| START |               | start `DAZ 3D\BryceLightning7\Lightning.exe` under Program Files (x86) on 64-bit Windows, under Program Files on 32-bit Windows |
| STOP  |               | terminate Lightning.exe                                |
| PING  |               | reply PONG                                             |
| EXIT  |               | reply ACK, then end the agent process                  |
| RESTART |             | reply ACK, start a new copy of the agent, end this one |
| UPDATE |              | reply ACK, start the update helper, end this one (see Agent update) |
| RENAME | name          | set the computer name (takes effect at reboot); reply ACK with the name |
| PRIORITY | level       | set the priority class of Lightning.exe: RealTime, High, AboveNormal, Normal, BelowNormal or Low; reply ACK |
| REBOOT |               | reply ACK, then restart the node after 5 s, closing programs by force |
| SHUTDOWN |             | reply ACK, then shut the node down after 5 s, closing programs by force |

The agent replies to START, STOP, PRIORITY, EXIT, RESTART, UPDATE, RENAME, REBOOT
and SHUTDOWN with (RENAME adds `name=` with the name that was applied; PRIORITY
adds `level=` as requested and `applied=` with the class Windows reports
afterwards, and answers `ok=no` when the two differ, as when RealTime is
requested without administrator rights and Windows grants High):

```
ACK
cmd=START
ok=yes
error=
```

and to PING with `PONG`.

## Offline rule

Thunderhead marks a node offline when no REPORT has arrived for three
intervals plus two seconds, and closes the connection. The agent treats any
socket error or close as the end of the session and returns to announcing.
A node is also offline when its connection closes.

## Thunderhead files

All files live beside Thunderhead.exe.

`settings.txt`

```
interval=15
tcp_port=42002
agent_file=
share_name=LightningRod
```

`agent_file` is the LightningRod.exe to push to nodes; empty means the one beside
Thunderhead.exe. `share_name` is the share on every node that maps to the
agent's folder.

`nodes.txt`, one block per node, keyed by MAC; the user-edited fields are kept
here and survive re-imaging of the node:

```
[00-80-64-BF-49-77]
name=WIN7E0007
group=Rack 1
wol=yes
boot=yes
autologin=yes
autostart_rod=yes
autostart_lightning=no
auto_shutdown=no
comment=fan noisy
```

`auto_shutdown` is the *Auto Shutdown* check box column of the Client Inventory. It
is only stored for now; the shutdown daemon that will act on it is not designed yet.

`logs\thunderhead-YYYY-MM-DD.log` holds every event with a timestamp.
`logs\nodes\NAME-YYYY-MM-DD.log` holds every message to and from that node.

## Agent files

The agent lives in `C:\LightningRod` on each node, shared as `\\NODE\LightningRod`,
and writes `C:\LightningRod\LightningRod-YYYY-MM-DD.log`. It keeps no other state. Its window
shows the Thunderhead it reports to (name, IP, MAC), its own start time and
running time, and every value it has sent, refreshed at each report; closing
the window stops the agent. *Open Client's Shared Folder* in Thunderhead opens
`\\NAME\<share_name>\` of every checked client in Windows Explorer. *Copy files or
folder to Client* asks for files or one folder, then copies the selection to that
folder on every checked client with the Windows shell copy, one client after
another, so the usual progress window and overwrite prompts appear.

## Agent update

Thunderhead reads the version resource of the agent file it has to push.
*Check outdated* checks every online client whose reported `agent` version is
lower. *Update checked* updates each checked online client; the client is
shown as Updating until its new agent connects.

Thunderhead first copies the agent file to
`\\<node IP>\<share_name>\LightningRod.new` while the old agent is still
running, so a share that cannot be written is reported in the event log
before anything on the node is stopped. The copy goes by address rather than
name because freshly cloned nodes share a name; when the address is refused
and the name resolves to that address, `\\<node name>\<share_name>\` is
tried as well, since some nodes accept the host's credentials only under their
name. Only when the file is in place is UPDATE sent.

On UPDATE the agent copies itself to `LightningRod.update.exe`, starts that copy
with `--helper <pid>`, replies ACK and exits. The helper waits for the old
process to end, renames `LightningRod.new` over `LightningRod.exe` (retrying
while the old file is still closing), starts `LightningRod.exe` and exits; the
next agent start deletes the helper copy. The swap is local to the node, so it
does not depend on the network or on how fast the node is. Nothing outside the
agent is needed on the node.

For an agent older than 0.2.5, whose helper instead waits for a file named
`update.go` (at most five minutes from 0.2.5, two minutes before), Thunderhead
renames `LightningRod.new` over `LightningRod.exe` through the share once the
agent's connection has closed and then creates `update.go`. If the node has not
reconnected after ten minutes, Thunderhead drops the Updating state and logs it.

This path uses Windows file sharing and therefore covers the Windows 7 nodes;
the XP nodes need SMB1 on the host or another transfer method, to be decided.

## Rename and reboot

*Rename node* in Thunderhead opens one dialog per checked online client, showing
its current name, MAC and IP so that freshly cloned nodes with the same name can
be told apart, with the new name (1 to 15 characters, letters, digits and
hyphens, not all digits) and a *reboot after rename* option. On a successful
ACK Thunderhead records the new name at once and, if the option was set, sends
REBOOT. The node reconnects under its new name after the restart.

RENAME uses SetComputerNameEx and therefore needs the agent to run with
administrative rights; with UAC enabled the agent must be started elevated
(for example by a scheduled task with highest privileges) or the ACK reports
"Access is denied". REBOOT and SHUTDOWN need the shutdown privilege, which
ordinary users hold on Windows 7, and close running programs by force.
*Restart node* and *Shut down node* ask for confirmation before sending.
