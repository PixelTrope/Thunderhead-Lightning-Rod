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
agent=0.4.0
challenge=9f1c2b7a0d4e6f8891a2b3c4d5e6f701
```

Thunderhead answers by unicast to the sender's address and port:

```
OFFER
host=UR
port=42002
challenge=3a7e...32 hexadecimal digits
auth=b41d...64 hexadecimal digits
```

`challenge` in the ANNOUNCE is a fresh challenge token, a random value used once, from an agent of 0.4.0 or
later; `auth` in the OFFER is Thunderhead's proof of the farm key over it
(see Authentication), and `challenge` in the OFFER is the host's own token, which the agent has to
sign in its HELLO. (0.4.0 named the field `nonce`; Thunderhead 0.4.1 still
reads that name and sends the OFFER token under both names, until no 0.4.0
agent remains.) An agent that holds farm.key connects only to a host whose
OFFER proves the key; an agent without the file connects to any host, as
agents before 0.4.0 do, which ignore the two fields.

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
mem_installed_mb=4096
mem_type=DDR3
disk_total_mb=3889
board=Dell Inc. 0XXXXX
chipset=AMD A55E Host Bridge
gpu=AMD Radeon HD 6310 Graphics
vram_mb=384
lightning3_path=C:\Program Files\DAZ 3D\BryceLightning7\Lightning.exe
lightning3_version=7.1.0.109
lightning2_path=
lightning2_version=
relay=no
relay_ip=
render_host=no
render_bin=C:\LightningRod\RENDER.BIN
render_bin_exists=yes
bryce3_path=C:\Program Files (x86)\DAZ 3D\Bryce7\Bryce.exe
bryce3_version=7.1.0.109
bryce2_path=
bryce2_version=
bryce1_path=
bryce1_version=
keyed=yes
challenge=3a7e...the challenge token of the OFFER
auth=7c02...64 hexadecimal digits
```

`mem_mb` is the physical memory the node's Windows can address, the figure
Windows calls usable. `mem_installed_mb` is what the firmware's SMBIOS tables
say is fitted, so the two differ on a 32-bit system with 4 GB or more, where
the usable figure stops at what lies below the address hole. Agents from
0.2.14 send `mem_installed_mb`; it is 0 where the firmware supplies nothing,
and Thunderhead shows that as n/a and an absent field as blank. Agents from
0.2.17 also send `mem_type`, the memory technology of the fitted modules as
the firmware's SMBIOS memory device records (type 17) state it: `DDR3`,
`DDR4`, `SDRAM` and so on, with `/` between the names when fitted modules
differ, `Unknown` when the firmware fills the field with Unknown or Other, and
empty when the table has no memory device records at all; Thunderhead shows
the empty value as n/a and an absent field as blank. The value is what the
BIOS wrote, not a reading from the modules. The same records supply
`mem_installed_mb` on Windows XP, where the agent reads the SMBIOS table
through WMI (`MSSmBios_RawSMBiosTables`) because the OS has no
GetSystemFirmwareTable.

Agents from 0.2.19 send `lightning3_path` and `lightning3_version`, the
installed Bryce 7 / 7 Pro Lightning as found at the agent's start by the
lookup described under Lightning lookup, and agents from 0.2.20 also
`lightning2_path` and `lightning2_version` for the Bryce 6.x Lightning. Each
pair is empty when none is installed, and Thunderhead shows that as "not
installed" and an absent field as blank. The window of the agent shows the
same.

Agents from 0.3.0 send `relay=yes|no` and `relay_ip=`: whether the agent's
Settings make it an SMB1 relay, and the address of the Windows XP client it
relays for (see SMB1 relay). Such an agent sends HELLO again, on the same
connection, when its Settings change; Thunderhead takes the new static fields
from it and treats the session as unbroken. Both sides play their HELLO sound
for that HELLO as for any other.

Agents from 0.4.2 send `render_host=yes|no`, the node's Bryce 3D Network
Render Host setting (see Bryce 3D Network Render Host), in HELLO and in every
full REPORT, and in a later REPORT whenever it changes. Agents from 0.4.5 send `render_bin`, the RENDER.BIN folder of the node, and
`render_bin_exists=yes|no`, in HELLO and in every full REPORT (see Bryce 3D
Network Render Host). Agents from 0.4.4 send
`bryce3_path` and `bryce3_version`, `bryce2_path` and `bryce2_version`,
`bryce1_path` and `bryce1_version`: the installed full Bryce 3D of each
generation (3 = Bryce 7 and 7 Pro, 2 = 6.x, 1 = 5.x) as found at the agent's
start, empty when none (see Bryce 3D on the host), and in every full REPORT
`bryce=running|stopped` and `bryce_cpu_pct`, the running Bryce.exe's share of
the whole machine since the previous report, whatever its generation.

Agents from 0.4.0 send `keyed=yes|no`, whether farm.key lies beside the
agent, and, when it does, `challenge=`, the challenge token of the OFFER the
session came from, and `auth=`, the agent's proof of the key over that token and its MAC
(see Authentication). Thunderhead refuses a HELLO whose proof does not check
and closes the connection with a line in the event log; a HELLO without the
fields is accepted as unauthenticated, and the shared-folder gate decides
what may be sent to the node.

Thunderhead replies with the report interval in seconds (5 to 60 in steps of 5,
default 15) and the node's phase: the delay in milliseconds before its first
report (see Report spacing):

```
SET
interval=15
phase=7200
```

Two more messages follow the SET of every HELLO: `AUDIO action=unmute` or
`action=mute` as the *All Lightning Rods Audio* switch of Settings stands (to
agents from 0.2.16), and `SOUND action=scheme` with the file names of the
agent's own sound events (to agents from 0.2.21); see Audio.

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
| SET   | interval, phase | change the report interval; `phase=` is the delay in ms before the next report, after which reports follow at the interval (agents from 0.3.2; the full interval when absent) |
| START | gen           | start the installed Lightning of generation `gen`: 3 = Bryce 7 and 7 Pro, 2 = Bryce 6.x, 1 = Bryce 5.x (3 when absent, as agents before 0.2.19 take it); reply ACK with `gen=`, `path=`, `version=` and `found=` (see Lightning lookup) |
| STOP  |               | terminate Lightning.exe                                |
| PING  |               | agents from 0.4.0: send HELLO again, then a REPORT with every value, and restart the report timer; 0.2.11 to 0.3.6: the REPORT alone; older: reply PONG |
| EXIT  |               | reply ACK, then end the agent process                  |
| RESTART |             | reply ACK, start a new copy of the agent, end this one |
| UPDATE |              | reply ACK, start the update helper, end this one (see Agent update) |
| RENAME | name          | set the computer name (takes effect at reboot); reply ACK with the name |
| PRIORITY | level       | set the priority class of Lightning.exe: RealTime, High, AboveNormal, Normal, BelowNormal or Low; reply ACK |
| REBOOT |               | reply ACK, then restart the node after 5 s, closing programs by force |
| SHUTDOWN |             | reply ACK, then shut the node down after 5 s, closing programs by force (no longer sent by Thunderhead, which uses POWER) |
| BATCH | kind, text, file, maximized, front | `kind=bat` or `kind=ps1`: write the text (hexadecimal digits of its UTF-8 bytes) to a batch file or PowerShell script and run it; `kind=exe`: run `file`, an executable already in the agent's shared folder; reply ACK when started and again when ended (see Remote file); agents from 0.2.7 (bat) and 0.3.4 (ps1, exe) |
| POWER | action        | run Windows' `shutdown.exe` and reply ACK with the action: `shutdown` (`-s`), `restart` (`-r`) or `coldrestart` (`-g`), each with `-f -t 66`, or `abort` (`-a`); agents from 0.2.6 |
| AUDIO | action        | set the master mute of the default playback device: `mute` or `unmute`; reply ACK with the state read back (see Audio); agents from 0.2.16 |
| TEAMVIEWER |          | start the installed TeamViewer, found through the registry or the Start menu, and bring its window to the front; reply ACK with where it was found (see TeamViewer); agents from 0.2.18 |
| SOUND | action, file, logon, hello, logoff, report, badip | `action=play`: play `file`, a name in the agent's `sounds` folder, on the node's playback device; `action=scheme`: take the file names of the agent's own three sound events; reply ACK with `action=` and `file=` (see Audio); agents from 0.2.21 |
| BRYCE | action, gen, scene, output, first, last | the full Bryce 3D on the node: `action=start` starts the installed Bryce of generation `gen` (3 = Bryce 7 and 7 Pro, 2 = 6.x, 1 = 5.x; 3 when absent), with `scene=` (a path inside the agent's shared folder, empty for none) as its argument, and brings its window to the front; `output=`, `first=` and `last=` describe the render job and are kept in the agent's job.txt, the output folder being created when it lies inside the shared folder; `action=stop` ends whichever Bryce.exe runs; `action=front` brings its window to the front; reply ACK with `action=`, `gen=`, `scene=`, `path=`, `version=` and `found=`, then a full REPORT (see Bryce 3D on the host); agents from 0.4.4 |
| SHARE | action, path, name | `action=create`: create the folder `path` (a full local path) on the node and share it as `name` at share level for everyone, keeping an existing share of that name that already points at the folder; needs administrator rights on the node; reply ACK with `action=`, `path=`, `name=` and `existed=` (see Create Remote Shared Folder); agents from 0.4.6 |
| RENDERHOST | set, render_bin | `set=yes|no` becomes the node's Bryce 3D Network Render Host setting, saved for the next start; `render_bin=` (agents from 0.4.5), when present, becomes the node's RENDER.BIN folder, created when the node is the host; reply ACK with `render_host=`, `render_bin=`, `render_bin_exists=` and `render_bin_in_share=` as now in force, then a full REPORT (see Bryce 3D Network Render Host); agents from 0.4.2 |
| RELAYSET | relay, relay_ip | set the agent's SMB1 relay values as its Settings dialog would (`relay=yes|no`, `relay_ip=`), save them and send HELLO again when they changed; reply ACK with the values in force (see SMB1 relay); agents from 0.3.1 |
| RELAY | op, job, dest, share, pattern | SMB1 relay work for the client at `dest`, each op on a thread so that an unreachable destination never stalls the agent's reports (probe on a thread from 0.3.2): `op=probe` answers whether `\\dest\share\` opens from the node; `op=push` copies what Thunderhead staged under `relay\<job>` in the node's own share to the root of that share; `op=pull` fetches the newest file matching `pattern` from it into `relay\<job>`; reply ACK with `op=`, `job=`, `dest=`, `files=`, `bytes=` and `file=` (see SMB1 relay); agents from 0.3.0 |
| MESSAGE | title, text, sound, timeout | open a message window on the node's screen with `title` in its caption and `text` (hexadecimal digits of its UTF-8 bytes), play `sound`, a name in the agent's `sounds` folder (none when empty), and close the window by itself after `timeout` seconds (0: when dismissed); reply ACK when opened and again when closed (see Display Message); agents from 0.3.3 |

*Exit LR* (LightningRod Control) warns once per checked online client before
its EXIT, naming the client and its address: closing Lightning Rod cannot be
undone from Thunderhead, the client stops responding, and control returns only
when Lightning Rod is started again at the client, through a remote desktop
session, or by restarting the client. *Yes, really End LR* sends the EXIT,
*Skip this Client* passes to the next checked client, *Cancel All* ends the
round; the status bar counts sent, skipped and not asked.

The agent replies to START, STOP, PRIORITY, EXIT, RESTART, UPDATE, RENAME, REBOOT,
SHUTDOWN, POWER, AUDIO, TEAMVIEWER, SOUND, RELAYSET, RENDERHOST, BRYCE, SHARE, RELAY, MESSAGE and BATCH with (RENAME adds `name=` with the name that was applied;
START adds `gen=`, `path=`, `version=` and `found=` as described under Lightning lookup;
PRIORITY adds `level=` as requested and `applied=` with the class Windows reports
afterwards, and answers `ok=no` when the two differ, as when RealTime is
requested without administrator rights and Windows grants High; POWER adds
`action=` and answers `ok=no` with the exit code of `shutdown.exe` when that
is not 0; BATCH and MESSAGE are answered twice, as described under Remote file
and Display Message):

```
ACK
cmd=START
ok=yes
error=
```

and to PING as the table says: with HELLO and a full REPORT from 0.4.0, with
the REPORT alone from 0.2.11, with `PONG` before that.

An agent of 0.4.0 or later that holds no farm.key carries out SET and PING
only; every other verb is answered `ACK ok=no error=not paired: ...` and
logged, until the file appears (see Authentication).

## Authentication

The farm's security is Windows file sharing: a host that can open
`\\node\LightningRod` with the credentials Windows demands is the host that may
command the node. Two mechanisms, both in force from 0.4.0, hold the
programs to that rule.

**farm.key.** Both programs keep the same secret, `farm.key`, a line of 64
hexadecimal digits (32 random bytes) beside the executable. Thunderhead
creates its own at its first start, from the system's random number source,
and logs the fact. An agent receives its copy when Thunderhead writes the
file into the node's shared folder, which Windows allows only to a host that
holds that share's credentials; the agent update stages the file beside
`LightningRod.new` as well, so an agent updated to 0.4.0 holds the key from
its first start. The file may also be copied by hand, and when Thunderhead
and an agent run from the same folder (the host's own agent) one file serves
both.

Every message that opens a session is signed with HMAC-SHA-256 over the
key. The agent puts a fresh challenge token (`challenge=`, 32 hexadecimal
digits, used once) in each ANNOUNCE; the OFFER answers with
`auth=HMAC(key, "OFFER" LF token LF host LF port)` and a token of its own;
the HELLO answers that with `auth=HMAC(key, "HELLO" LF offer-token LF mac)`.
Thunderhead keeps the tokens it has offered for ten minutes and accepts a
HELLO only against one of them, so a captured HELLO cannot be replayed; a
renewed HELLO on a connection already authenticated is not checked again.
An agent that holds the key ignores an OFFER that fails the check and logs
"OFFER from ... rejected" (at most once a minute), re-reading farm.key in
case the key was replaced on disk. Thunderhead refuses a HELLO that fails
the check, logs it, and closes the connection. Neither side ever sends the
key itself.

An agent of 0.4.0 or later without the file announces, connects to the
first host that offers, reports, and does nothing else: every verb but SET
and PING is answered `ACK ok=no` with `error=not paired: ...`. It re-reads
farm.key every five seconds; when the file appears it ends the session
("farm.key arrived; reconnecting to authenticate the host") and connects
again, this time only to a host that proves the key. The agent's window
shows the state in its first line ("host authenticated" or "host not
authenticated: commands refused until farm.key arrives") and in the
Authentication row of its table.

**The shared-folder gate.** Thunderhead sends no command to a node whose
shared folder it has not read: one gate sits in front of every message it can
send, and in front of every operation on the node's share as well, since
writing into the share is control of the node too: *Update LR* (the
agent file is staged in the share), *Copy Files or Folder to Client*, the
render job, *View Remote Log* and the Bryce host controls; a node reached
through an SMB1 relay needs its relay verified as well, since the relay does
the writing. A staging or copy that happens to succeed does not open the gate;
only the check does. Thunderhead takes nothing from such a node either: from 0.4.6 a
REPORT from such a node keeps its session alive and is otherwise discarded,
with one line in the event log, its live columns stay blank (what was shown
is dropped when a check fails), and the Client details pane says that
commands and reports are refused; the next report after the folder passes is
taken as usual, and the event log says so. The one message outside the gate
is SET, the report schedule, which instructs nothing. When a HELLO arrives Thunderhead
answers it with SET and starts a worker thread that lists the one path the
file transfers use: `\\<name>\<share_name>\` when the name resolves to the
node's address, `\\<ip>\<share_name>\` otherwise (freshly cloned nodes share a
name); there is no falling back from one to the other, since a folder that
opens by address but not by name would still fail every copy, which goes by
name. The check made for a connecting node (its HELLO, or its announcements
when it does not connect) first drops any connection this host still holds to
that share, so that a credential removed from Credential Manager, or a
session Explorer left open, cannot answer in place of a fresh logon; the
checks that follow reuse the connection. Every file operation of Thunderhead
(Copy Files or Folder, the render job, the agent update, the remote log, the
executable check of Run Remote File, Open Client's Shared Folder and the
RENDER.BIN check) goes through the path the gate verified. While the check is pending or has failed, the row is tinted
orange, the Client details pane says so under *Shared folder*, and every
command to the node is refused with a line in the event log ("... not sent
to NAME: its shared folder is not readable from this host (...). Use Open
Client's Shared Folder and enter the credentials Windows asks for"); the
status bar counts them as blocked. A check that passes writes farm.key into
the folder when it is missing or differs, logs "shared folder ... is
readable from this host", and releases the messages that used to follow
HELLO directly: the *All Lightning Rods Audio* switch, the SOUND scheme and,
for a relay, the RELAY probe. A failed check is repeated every minute, a
passed one every ten minutes; after *Open Client's Shared Folder* the check
runs every five seconds for two minutes, so that the row clears as soon as
the credentials typed into the Explorer prompt are in. A node that is
staged for an update has proved its share writable and passes the gate.

A node reached through an SMB1 relay cannot be opened from the host; the
relay's `RELAY op=probe` stands in for the check, sent once the relay itself
has passed the gate, and farm.key reaches such a node by a push through the
relay (`relay\<job>\farm.key`, then `RELAY op=push`).

A node that keeps announcing without connecting is shown as *Announcing* in
the Status column while its announcements arrive. After four of them, and
every ten minutes after that, Thunderhead logs that its farm.key may differ
from the host's and checks its share, which writes the host's key into it;
the agent re-reads the file at its next rejected OFFER and connects.

### Inventory tints

The Client Inventory's alternating gray rows are tinted to state a node's
condition, in both schemes, the strongest condition winning: red for a node
that is offline; orange for an online node whose shared folder the host has
not read (gate closed); yellow for an online node whose agent is older than
the agent file Thunderhead would push; metallic blue for the Bryce 3D Network
Render Host (flag or report); turquoise for a Windows 7 client set to act as
SMB1 relay. For one second after a node answers *Ping LR* its row is
painted solid blue-biased purple (R 50 %, G 0 %, B 66 %)
with white text, so that the answer is seen beyond the Last report column
going to 0 s. *Select Group* (List Control) selects by these tints: the
chosen entry, *Offline (Red)*, *Unauthenticated (Orange)*, *Outdated LR
(Yellow)*, *W7 Relay (Cyan)* or *Bryce 3D Net Render Host (Blue)*, checks
exactly the rows painted in that tint and clears every other check box,
whatever was checked before; the tint a row shows is the one it is selected
by, so an offline render host belongs to Offline. *Invert Selections* flips
every check box. The Last report column itself is rewritten only when its text
changes, and the list erases each visible row in its own shade rather than in
the background color, so the per-second update no longer flashes the cell
white; the column rules are drawn from the list's own column layout, not the
header window, so they stay on the column edges through a horizontal scroll.

## Bryce 3D Network Render Host

One client of the farm is the Bryce 3D Network Render Host, B3NDHR for short:
the machine that runs the full Bryce 3D and hands the frames out to the
Lightning clients. From 0.4.2 the role is held in three places that are kept
in step. Thunderhead's record is the sixth Info Flag of Client details,
*Bryce 3D Network Render Host (B3NDHR)*, saved as `render_host` in nodes.txt.
The agent's own setting is the two-position slider *Bryce 3D Network Render
Host (B3NDHR) mode* in its Settings dialog, saved as `render_host` in its
settings.txt and read at every start, so that the person at the node can set
it. The agent reports that setting as `render_host=yes|no` in HELLO and in
every full REPORT, and in a later REPORT when it changes, and Thunderhead
shows it in Client details as *Net Render Host: Yes* or *No*, adding "(the
Info Flag here says ...)" when the flag and the report disagree.

*Bryce 3D Net Render Host Toggle* (LightningRod Control, Configure Client) flips the flag of every
checked client, saves it, and sends `RENDERHOST set=yes|no` with the new value
to each checked online client whose agent is 0.4.2 or later (older agents are
passed over with a line in the event log; the shared-folder gate applies as to
every command). The agent sets its mode, saves it, answers ACK with
`render_host=`, and sends a full REPORT at once; the event log reads
"RENDERHOST ok on NAME: Net Render Host on". A client that is offline has only
its flag flipped; its agent learns the role when the toggle is sent again
while it is online, or from its own Settings dialog.

In the Client Inventory a row is tinted metallic blue while either the flag or
the latest report says the node is the render host; the tint ranks below
offline, gate-closed and outdated and above the relay tint (see Inventory
tints).

### RENDER.BIN

RENDER.BIN is the folder the host writes the frames to. Its default is
RENDER.BIN inside the agent's own folder, the shared one, so that it is
`C:\LightningRod\RENDER.BIN` on a node installed as intended and reachable
from the host as `\\node\LightningRod\RENDER.BIN`; the agent's Settings
dialog has a *RENDER.BIN folder* entry (a full path on the node; empty
restores the default), saved as `render_bin` in its settings.txt, and the
folder is created when the node is the render host and it is missing. The
agent sends `render_bin` and `render_bin_exists` in HELLO and in every full
REPORT, Client details shows them as *RENDER.BIN*, and its own window shows
them too.

When *Bryce 3D Net Render Host Toggle* switches a client on, a dialog shows that
client's RENDER.BIN as the agent reports it (or `C:\LightningRod\RENDER.BIN`
when the agent has not reported one), to confirm or change; *Skip* leaves the
client untouched. OK sends `RENDERHOST set=yes render_bin=<path>`: the agent
takes the folder, creates it if it is missing, and answers with
`render_bin=`, `render_bin_exists=` and `render_bin_in_share=`, the path below
its shared folder when the folder lies inside it. Thunderhead then checks
the folder by opening `\\node\<share_name>\<render_bin_in_share>\` in
Windows Explorer; a folder outside the shared folder cannot be reached from
the host and the event log says so, as it does when the folder could not be
created. Switching a client off asks nothing. *Render Job on Host* offers
the chosen host's RENDER.BIN as the output folder, following the host chosen
in its list.

## Bryce 3D on the host

The full Bryce 3D is what a Bryce 3D Network Render Host runs, in one of
three generations numbered as the Lightning generations are: 3 = Bryce 7 and
7 Pro, 2 = Bryce 6.x, 1 = Bryce 5.x. From 0.4.4 the agent looks each one up
at its start as it looks Lightning up (see Lightning lookup), in the same
order, and takes the first `Bryce.exe` that exists and whose version's first
number is the generation's: the product key under `HKLM\SOFTWARE\DAZ 3D`
whose name starts with `Bryce 7`, `Bryce 6` or `Bryce 5` (`Bryce 7.1` on a
7.1.0.109 install, `Bryce 7.0` and `Bryce 6.3` on the Windows XP export;
`Bryce 7.0 Content` is the preset library and is passed over), highest
version first and in both registry views, `Location` naming the folder and
`Version` the version; the Add/Remove Programs entry named the same way
(`InstallLocation`, `DisplayVersion`); the Start menu shortcut whose target
is a `Bryce.exe` (`DAZ 3D\Bryce7\Bryce 7.1.lnk`; the guide and wiki shortcuts
in the same folder point elsewhere), told apart by the file's version; and
the generation's default folders under each Program Files folder: `DAZ
3D\Bryce7` for generation 3, `DAZ 3D\Bryce` for generation 2 (the folder of
the XP export), and for generation 1, whose installer traces are not yet
verified on a node, `DAZ 3D\Bryce 5.5`, `DAZ\Bryce 5.5` and `Corel\Bryce 5`.
The result goes into HELLO as `bryce3_path`/`bryce3_version`,
`bryce2_path`/`bryce2_version` and `bryce1_path`/`bryce1_version`, and every
full REPORT says whether a Bryce.exe runs (`bryce=`) and its CPU share
(`bryce_cpu_pct=`); Client details shows them as *Bryce 7 & 7 Pro*, *Bryce
6.x*, *Bryce 5.x* and *Bryce 3D*.

*Start B3D* (Render Control, Bryce 3D) first asks which Bryce, with one radio
button per generation (Bryce 7 & 7 Pro, Bryce 6.x, Bryce 5.x), then sends
`BRYCE action=start gen=` to every online client that is flagged as Bryce 3D
Network Render Host or reports as one, whatever the check boxes say; *Stop
Bryce 3D* sends `action=stop`, which ends whichever Bryce.exe runs on each
host. With no such client the status bar says so. The agent starts the
generation's Bryce from its own folder, with the scene named in `scene=` as
its one argument when there is one (as a double-click on a .br7 does; the
line is `"<Bryce.exe>" "<agent folder>\<scene>"`, verified by hand on the
host), looks for Bryce's window for up to 60 s and brings it to the front as
it does for TeamViewer; `front` brings the window to the front again. Before every start the agent also writes the neutral gray image itself,
beside the scene it is about to open (the one named in `scene=`, or, with no
scene given, the scene of its last job from job.txt, since Bryce launches to
its previous state), at the scene's render resolution read from the file,
replacing any image there, and reports it in the ACK's `image=`; so the
image is in place whatever happened on Thunderhead's side. Before every
start the agent makes sure Bryce's preference *Image with
Scene Open/Save* is on, since without it Bryce begins a render proof the
moment it opens a scene: the preference is byte 0x21F0 of the file
`Bryce Prefs` beside Bryce.exe (16336 bytes on one Bryce 7.1 install, 14276
on another, the offset the same in both) (1 on, 0 off, found by comparing the
file with the preference off and on in Bryce 7.1), and the agent sets it in
that file and in the VirtualStore copy under the user's profile when Windows
keeps one there, while Bryce is not running; the ACK's `prefs=` says what
was found and done, and the event log repeats it. A start
with a scene replaces whatever Bryce is open: a running Bryce is ended first
and the ACK says `restarted=yes`; a start without a scene while a Bryce runs
is refused with `error=` naming the running file. The ACK carries `gen=`,
`restarted=`, `path=`, `version=` and `found=` as START does for Lightning,
and `error=` says when the generation is not installed. The event log reads "BRYCE start ok on
NAME: Bryce 7 & 7 Pro 7.1.0.109 at C:\...\Bryce.exe, found through ...;
scene jobs\x\x.br7".

*Render Job on Host* (Render Control, Bryce 3D) is the dialog for a render: the host
(a list of the online render hosts), the scene source, the scene file, an
optional texture folder, the output folder on the host (its RENDER.BIN, as
reported), the first and last frame, and which Bryce opens the scene (the
same three generations, Bryce 7 & 7 Pro by default). The scene source is
*Copy from source* or *Use existing job on host*. With *Copy from source*,
*Copy and Start Bryce* copies the scene and the texture folder to
`jobs\<scene name>\` in the host's shared folder through the same copy
machinery and progress window as *Copy Files or Folder to Client* (through
the relay for an SMB1 host); when the scene is already there, a question
offers *Overwrite* or *Cancel the copy* first, and a cancel is logged and
nothing is sent. It appends the job to `jobs.txt` beside Thunderhead.exe
(time, host, scene, textures, where it was copied, output folder, frames),
and, as soon as the copy has ended without a failure, sends the host `BRYCE
action=start gen= scene=jobs\<scene name>\<scene file> output= first=
last=`. When a scene is chosen (either source), Thunderhead reads its render
settings from the file: a Bryce scene is a `Bryce_6.0_File` container of
zlib streams, one of the last of which inflates to a 416-byte record holding
the document width and height (big-endian 32-bit at offsets 0 and 4) and the
render width and height (at 8 and 12, repeated as height and width at 0x18
and 0x1c; the same as the document size unless the render size is a
multiple, as in a 200 % render), the animation length in seconds (big-endian float at 0x3c, 0
for a still) and the frame rate (float at 0x40); the last frame Bryce shows
is length times rate (verified on 604 scenes of the farm: 30 s at 30 fps is
frames 0 to 900). The info line shows resolution, rate, length and frame
range, and empty frame fields take 0 and that last frame. When the job
starts, a neutral 50 % gray 24-bit BMP at the scene's resolution, the format
Bryce itself writes, is put beside the scene in the job folder under the
scene's name, replacing any image already there (a partly rendered frame
left beside the scene makes Bryce resume rendering it on load whatever the
preference says); with Bryce's preference *Image with Scene Open/Save* on,
Bryce shows that gray image when it opens the scene instead of starting a
render, which keeps the host idle until the network render is begun. The jobs.txt line records the resolution and what was done about the
image. With *Use existing job on host*, *Browse...* opens the host's jobs
folder through the share the gate verified, the chosen file must lie under
that share, nothing is copied, and *Start Bryce* sends BRYCE at once with
the file's path below the share as `scene=`; the texture folder does not
apply, and the host's share must be reachable from here (not through a
relay) for the browsing. The agent keeps
those values in `job.txt` beside itself and creates a relative output folder
inside its own folder, so that the person at the host can point Bryce's
render output at it; the network render itself is begun from Bryce's own
Render menu on the host, by hand or through TeamViewer, because Bryce offers
no outside way to start it. Bryce keeps texture paths as it saved them, so a
scene whose textures came from another machine asks for them once; the
texture folder copied beside the scene is where to point it.

## Report spacing

An agent times each report from the one it has just sent, so the moment of its
first report fixes its place in the interval for the whole session. Identical
units started together send HELLO within a second or two of each other and would
otherwise report in one bunch every interval. Thunderhead therefore deals every
node a slot: the SET that answers a HELLO carries `phase=`, the delay in
milliseconds before the agent's first report, chosen as the middle of the widest
gap between the reports Thunderhead expects from the other online nodes (the
whole interval when the node is the only one). A renewed HELLO is answered with
the slot the node already holds. When the interval changes in Settings, the SET
sent to every online node deals the slots out evenly, `interval / nodes` apart.
Thunderhead keeps the expected time of each node's next report from the SET it
sent and from every REPORT that arrives; agents before 0.3.2 ignore `phase=`
and are expected a full interval after the SET.

Slots dealt at HELLO still leave nodes in the same clock second now and then,
so every REPORT is also placed on arrival: Thunderhead counts the online nodes
whose latest REPORT or HELLO arrived in the same clock second (its own clock,
whole seconds). The first report in a second is left alone; the second, third
and fourth are nudged by 1, 2 and 3 s respectively; a fifth or later one is left
for the next interval to sort out. The nudge is given by waiting that many
seconds and then sending the node `SET interval= phase=<interval>`; the agent
times its next report from that SET, so the interval then running is longer by
the nudge and only that one, after which the node keeps the moved slot. One
nudge is pending per node at most, a node alone in its second is sent nothing,
and the event log records each nudge with the node's place in the second.
Because the SET itself re-times the agent, the nudge works with every agent
version. Nodes can only end up one per second when the interval has at least as
many seconds as there are nodes online.

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
dark_mode=0
th_audio=1
lr_audio=1
sound_scheme=custom
sound.Startup=fanfare.mp3
```

`agent_file` is the LightningRod.exe to push to nodes; empty means the one beside
Thunderhead.exe. `share_name` is the share on every node that maps to the
agent's folder. `dark_mode` is the Dark Mode switch of the Settings dialog, 1
for the dark scheme: dark panes, dialogs, log views and status bar with light
text, the Client Inventory's header light with black captions and its rows dark
gray with white text and white rules, the reverse of the light scheme.
`th_audio` and `lr_audio` are the *Thunderhead Audio* and *All Lightning Rods
Audio* switches of Settings, `sound_scheme` is `default` or `custom`, and each
`sound.<Category>` line assigns a file of the `sounds` folder to a category of
the custom scheme; see Audio. The three audio keys are absent until the
settings are first saved and then read as 1, 1 and `default`.

`farm.key` is the shared secret of Authentication: created at the first
start, 64 hexadecimal digits on one line, written to each client's shared
folder as that is found readable. Deleting it makes Thunderhead create a new
one at its next start, after which every paired client receives the new key
through its share before it connects again.

`sounds\` holds the sound files: the repository's `assets\sounds`, which the
build copies beside the executable, plus any file *Sound Schemes* copies in.

`jobs.txt` records every render job started with *Render Job on Host*, one
line each (see Bryce 3D on the host).

`nodes.txt`, one block per node, keyed by MAC; the user-edited fields are kept
here and survive re-imaging of the node:

```
[00-80-64-BF-49-77]
name=WIN7E0007
group=Rack 1
wol=yes
boot=yes
boot_alarm=06:30
autologin=yes
autostart_rod=yes
autostart_lightning=no
render_host=no
auto_shutdown=no
comment=fan noisy
```

`wol`, `boot`, `autologin`, `autostart_rod` and `autostart_lightning` are the
first five check boxes under the heading *Info Flags (does not change any setting or
function)* at the top of Client details: *Wake On LAN*, *Boot on Alarm*, *Auto
Login*, *Auto start Lightning Rod* and *Auto start Lightning*. They record what
the operator knows about the node and change nothing on it. `render_host` is
the sixth, *Bryce 3D Network Render Host (B3NDHR)*, on a row of its own; it is
likewise only a record when ticked by hand, but *Bryce 3D Net Render Host Toggle*
sets it and the agent's own setting together (see Bryce 3D Network Render
Host). `boot_alarm` is
the time of day in the picker beside *Boot on Alarm*, kept in 24-hour form and
shown with AM or PM; it is empty until the picker is first changed, when the
picker shows 06:00 AM, and is likewise only stored. `auto_shutdown` is the
*Auto Shutdown* check box column of the Client Inventory. It is only stored for
now; the shutdown daemon that will act on it is not designed yet. A Bryce 3D
Network Render Host is never shut down automatically: the flag is cleared the
moment a node becomes a Render Host by any route (the Info Flag, *Bryce 3D
Net Render Host Toggle*, the agent's own setting in HELLO or REPORT, or a nodes.txt
that says both when it is loaded), its Auto Shutdown cell is painted empty and
refuses the click, and Client details shows *No (never for a Net Render
Host)*.

From 0.5.0 the Control menu and the button pane share one layout: six
sections (Thunderhead Control, List Control, Node Control, LightningRod
Control, Comms, Render Control), each with its commands in the order of the
menu, and nested groups (Select Group, Command File, Utilities, Configure
Client, Bryce 3D) that are submenus in the menu and, in the pane, one button
of the section's colour, drawn with a double border to say that options lie
behind it, that opens a pop-up window of the nested buttons under itself; the pop-up closes on a choice or when anything else is clicked. Run Lightning is one item and one button that asks the generation in
a radio dialog, as Start B3D does.

`logs\thunderhead-YYYY-MM-DD.log` holds every event with a timestamp.
`logs\nodes\NAME-YYYY-MM-DD.log` holds every message to and from that node.

## Agent files

The agent lives in `C:\LightningRod` on each node, shared as `\\NODE\LightningRod`,
and writes `C:\LightningRod\LightningRod-YYYY-MM-DD.log`. Its sounds are in
`C:\LightningRod\sounds`, put there by the agent update, with `scheme.txt`,
the file names of its own sound events as the last SOUND scheme message set
them (see Audio). `settings.txt` beside it holds its Settings dialog
(`relay=yes|no`, `relay_ip=`, `render_host=yes|no`, the Bryce 3D Network
Render Host switch, and `render_bin=`, its RENDER.BIN folder, empty for the
default), `job.txt` holds the last render job the agent was
given with BRYCE (scene, output, first, last, started), `jobs\` the scenes
Thunderhead copied for render jobs, `farm.key` is the host's key as Thunderhead
wrote it through the share (see Authentication), and `relay\` holds the
staging folders of RELAY jobs while they run; it keeps no other state. The
intended place for the folder is `X:\LightningRod` on any drive with room,
shared under the one name, and Thunderhead, the agent and the programs that
will work with them are meant to live in that same folder; a folder under
Program Files is not meant to be shared. Its window has the menu
File (Exit), Control (Settings) and Help (About) and
shows the Thunderhead it reports to (name, IP, MAC), its own start time and
running time, every value it has sent, refreshed at each report, the
sounds folder with the number of files in it, and the SMB1 relay setting; closing
the window stops the agent. *Open Client's Shared Folder* in Thunderhead opens
`\\NAME\<share_name>\` of every checked client in Windows Explorer. *Copy files or
folder to Client* asks for files or one folder, then copies the selection to that
folder on every checked client, one client after another, in a progress window
of Thunderhead's own that opens the moment the copy starts (the Windows shell
copy holds its window back for about a second, which is longer than most copies
to a share take): the client being written, the file being written, a bar over
the bytes of the whole job, Cancel, and at the end the result, which stays until
Close. Existing files are overwritten; a share that refuses the connection gets
the standard credential prompt once; the event log has one line per client. A
client reached through an SMB1 relay is copied to through the relay, and its
shared folder cannot be opened from this host (see SMB1 relay).

### Create Remote Shared Folder

The third choice of *Copy Files or Folder to Client*, *Create Remote Shared
Folder...*, asks for a folder on the client as a full local path (for example
`D:\RENDER.BIN`) and a share name (the folder's last part until typed
otherwise), and sends `SHARE action=create path= name=` to every checked
online client whose agent is 0.4.6 or later, through the shared-folder gate
as every command. The agent creates the folder if it is missing and shares it
with NetShareAdd at share level for everyone, the folder's own permissions
still applying; a share of that name that already points at the folder is
kept and reported as existing, one that points elsewhere is an error, and
without administrator rights on the node Windows refuses with access denied,
which the ACK reports. On success the event log reads "SHARE create ok on
NAME: \\NAME\name -> D:\RENDER.BIN" and Thunderhead opens the new share in
Windows Explorer, which is also where credentials for it get typed.

## Agent update

Thunderhead reads the version resource of the agent file it has to push.
*Select Group*, *Outdated LR (Yellow)* (List Control) checks every client
whose row is painted yellow, that is online, verified and with a reported
`agent` version lower, and nothing else. *Update LR* (LightningRod Control)
updates each checked online client; the client is shown as Updating until its
new agent connects.

Thunderhead first copies the agent file to
`\\<node IP>\<share_name>\LightningRod.new` while the old agent is still
running, so a share that cannot be written is reported in the event log
before anything on the node is stopped. The copy goes by address rather than
name because freshly cloned nodes share a name; when the address is refused
and the name resolves to that address, `\\<node name>\<share_name>\` is
tried as well, since some nodes accept the host's credentials only under their
name. Only when the file is in place is UPDATE sent. With the agent file in
place, the files of Thunderhead's `sounds` folder are copied to `sounds\` under
the same share, those missing there or differing in size or write time, so
that the updated agent holds every sound the host can ask it to play; the
event log's staging line counts them.

When the share refuses the credentials Windows offers, as a freshly cloned
node does whose account or password is not the host's, Thunderhead shows the
standard Windows credential prompt for that share, the one Explorer shows for
a shared folder without stored credentials. What is typed connects the share,
is saved in the Credential Manager when *Remember my credentials* is ticked,
and the copy is tried once more; cancelling the prompt ends the update of
that node with a line in the event log. Several refusals at once are
prompted for one after another.

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

This path uses Windows file sharing and therefore covers the Windows 7 nodes.
A Windows XP node's share is reached through an SMB1 relay (see SMB1 relay):
the agent file and the sounds are staged under `relay\<job>\` in the relay
node's share, the relay copies them to the root of the XP node's share, and
UPDATE then goes to the XP node as to any other; its helper swaps the file in
by itself, so no finish phase runs over the share (an XP agent older than
0.2.5, which waits for `update.go`, cannot be updated this way). The event
log's staging line names the relay and the files it copied.

## SMB1 relay

A Windows XP node's file server speaks SMB1 only, which Windows 10 no longer
speaks once its SMB 1.0/CIFS client is removed; a Windows 7 node speaks both.
The agent's Settings dialog (Control, Settings) has *Act as SMB1 Relay* and,
once that is ticked, the IP address of the Windows XP client to relay for. The
values go to `settings.txt` beside the agent, HELLO is sent again at once with
`relay=yes` and `relay_ip=`, and from then on Thunderhead treats the client at
that address as relayed whenever the relay is online: the client's commands
still go over its own session, which needs no file sharing, but every share
operation of the client is done through the relay. The Client Details of the
relay show *Relay Node: Yes* and *Relay IP: ADDRESS (NAME); its share opens
from there*, or the error the relay met, from the `RELAY op=probe` Thunderhead
sends after each HELLO of a relay (an agent that does not act as a relay shows
*Relay Node: No*); the details of the client show *Reached through relay NAME*.
The relay node must be able to open `\\ADDRESS\<share_name>\` with its own
logon (the same local account name and password on both machines passes
through); no credentials travel in a message.

*Relay Settings* (LightningRod Control, Configure Client) sets the same values from
Thunderhead: one dialog per checked online Windows 7 client, in list order,
showing its name and address, a Relay On/Off switch with its current state and
the destination address, which is enabled only while the switch is on. OK
sends `RELAYSET relay=yes|no relay_ip=` when something changed, Skip leaves the
client as it is, Cancel All ends the pass. The agent applies the values with
the checks of its own dialog (a four-number address that is not its own),
saves them, answers ACK with the values in force and sends HELLO again. Checked
clients that are offline, not running Windows 7 (its HELLO `os` version 6.1),
or whose agent is older than 0.3.1 are passed over with a line in the event
log; the status bar counts dialogs, sent, skipped and passed over.

The operations and their messages:

* *Update LR*: Thunderhead writes `LightningRod.new` and the `sounds`
  folder to `\\RELAY\<share_name>\relay\<job>\`, then sends the relay

```
RELAY
op=push
job=th1234-5
dest=42.32.22.156
share=LightningRod
```

  The relay copies everything under `relay\<job>` to the root of
  `\\dest\share\`, folders included, deletes the staging folder, and
  acknowledges with `files=` and `bytes=`, or `ok=no` with `error=` at the
  first file that fails. Thunderhead then sends UPDATE to the client.
* *Copy Files or Folder to Client*: the selection goes to the same staging
  folder, with the progress window following the bytes, then `RELAY op=push`;
  the window reads "Relay NAME is copying to \\ADDRESS\share\ ..." while
  the relay works, and Cancel waits for that step, since the relay has no
  abort. A staging that fails before the push is removed again.
* *View Remote Log*: Thunderhead sends `RELAY op=pull` with
  `pattern=LightningRod-*.log`; the relay copies the newest matching file, by
  name, from the client's share into `relay\<job>\` and acknowledges with
  `file=`; Thunderhead reads it from the relay's share, deletes the staging
  folder, and the window's line names the relay. Refresh repeats the pull.
* *Open Client's Shared Folder* cannot open an SMB1 share from this host; the
  event log says so and names the relay.

The ACK of every RELAY carries `op=`, `job=`, `dest=`, `files=`, `bytes=` and,
for a pull, `file=`. `push` and `pull` run on a thread of the relay agent, so
its reports go on while a copy takes its time, and are acknowledged when they
end; Thunderhead's worker thread waits for that ACK (10 min for an update, 30
min for a copy, 2 min for a log) and reports a relay that does not answer. A
RELAY whose `dest=` is not the address set in the relay's Settings, or that
reaches an agent not set as a relay, is refused with `ok=no`. The relay agent
deletes whatever is left under `relay\` when it starts. Agents older than
0.3.0 know neither the Settings dialog nor RELAY, and a relay must run 0.3.0
or later to be used.

## Rename and reboot

*Rename Node* in Thunderhead opens one dialog per checked online client, showing
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
Thunderhead sends REBOOT only after a rename with the reboot option; restarts
asked for by the operator go through *Shutdown Node*.

## Shut down and abort

*Shutdown Node* is the one button for shutting down and restarting. It opens a
dialog with three radio buttons, OK and Cancel for the checked online
clients, which is also the confirmation: *Shutdown (/s)*, *Restart (/r)* and
*Cold restart (/g)*. Thunderhead sends POWER with `action=shutdown`, `restart`
or `coldrestart`, and the agent runs `shutdown.exe` from the system directory
with `-s`, `-r` or `-g`, always followed by `-f -t 66`: programs are closed by
force and the node acts 66 seconds later. *Abort Shutdown* sends POWER with
`action=abort` to the checked clients, which run `shutdown.exe -a`; that
cancels the action while the 66 seconds are still counting.

The agent waits for `shutdown.exe` (30 s at most) and reports its exit code,
which is a Win32 error code: 0 is `ok=yes`, anything else is `ok=no` with the
code and its text, for example 1116 when an abort finds no shutdown in
progress, or 1190 when a shutdown is already scheduled. `-g` (full shutdown
and restart, then restart of registered applications) does not exist on
Windows XP, where `coldrestart` is answered with `ok=no`. Agents older than
0.2.6 do not know POWER; Thunderhead does not send it to them and writes a
line to the event log instead.

## Remote file

*Run Remote File* (Node Control) opens a dialog whose *File type* radio buttons
choose *Batch (.bat)*, *PowerShell (.ps1)* or *Executable (.exe)*. For the
two script kinds the text box holds the file, typed, pasted, or loaded with
*Browse for file* (UTF-16 or UTF-8 by byte order mark, otherwise UTF-8 when
the bytes are valid UTF-8, otherwise the ANSI code page of the host). For an
executable the text box and *Browse for file* are grayed and the *Executable
in the client's shared folder* box names the file, as a name or a path
relative to that folder; the file must already be there, put there with *Copy
Files or Folder to Client*. The boxes, the radio and the two check boxes
*Start maximized* and *Bring to front* keep their content while Thunderhead
runs. *Execute Remote File* first shows a warning that nothing checks the
file for errors or harmful code, with *Yes, Execute* and *No, Cancel*; only
*Yes, Execute* sends it.

For a batch file or a script Thunderhead sends BATCH to every checked online
client with `kind=bat` or `kind=ps1` and `text=` holding the
UTF-8 bytes of the text as hexadecimal digits, two per byte, so that the line
ends survive the one-line field format, and with `maximized=` and `front=`
(`yes` or `no`) from the two check boxes. The text may be 30000 bytes long, which
keeps the message below the 65536 byte limit. The event log and the node log
record the size of the text, not the text.

For an executable Thunderhead first looks for `\\<ip>\<share_name>\<file>`
from the host, client by client (a client behind an SMB1 relay is not checked,
since its share is not reachable from the host). A client whose share lacks
the file is passed over with a line in the event log and a notice window of
Thunderhead's own, built like the agents' Display Message window: the text
"File does not exist on NAME: path. Use the 'Copy Files or Folder to Client'
button to place it, and then try again.", an OK button, the ordinary caption
buttons, and a line counting down the 10 seconds after which it closes by
itself. The other clients receive BATCH with `kind=exe`, `file=` the name as
entered, and the two check box fields. `kind=ps1` and `kind=exe` go only to
agents from 0.3.4; older agents are passed over with a line in the event log.

The agent runs a PowerShell script as `batch\batch-YYYYMMDD-HHMMSS.ps1`,
written in UTF-8 with a byte order mark, through
`powershell.exe -NoProfile -ExecutionPolicy Bypass -File <path>` in a console
window titled *Thunderhead script NAME.ps1*, otherwise exactly as a batch
file below. On a 64-bit Windows an agent from 0.4.0 starts both shells from
`%windir%\Sysnative`, the alias through which a 32-bit process reaches the
real System32, so that the script runs 64-bit and sees the real System32 and
registry; the 32-bit agent's own System32 is the redirected SysWOW64, whose
shells ran scripts 32-bit with the WOW6432Node registry in place of the real
one and silently missed 64-bit components. On a 32-bit Windows System32 is
used as before. The agent's log names the shell each script ran through. An executable is checked again on the node (it must be inside the
shared folder, with no drive letter, leading separator or `..`) and started
from its own folder as it is; *Start maximized* applies when the program
honours the request, *Bring to front* does not, since the agent does not know
the program's window title. The ACKs carry `kind=` from 0.3.4 and `file=` as
the path the agent ran, `batch\NAME.bat`, `batch\NAME.ps1` or the name of
the executable.

The agent writes the text to `batch\batch-YYYYMMDD-HHMMSS.bat` in its own
folder, with CR LF line ends and in the OEM code page, which is what `cmd.exe`
reads, and starts it as

```
cmd.exe /d /c ""C:\LightningRod\batch\NAME.bat""
```

in a console window of its own on the node's desktop, titled *Thunderhead batch
file NAME.bat*, as when the file is double-clicked: output shows in that
window, `pause` and prompts wait for a key at the node, and the window closes
when the batch file ends. With `maximized=yes` the window opens maximized. With
`front=yes` the agent puts it above the other windows with the keyboard focus;
without that, Windows opens a window started by a background program behind
the others, with only its taskbar button to show for it. For both the agent
finds the window by its title, whatever hosts it (the classic console host or
Windows Terminal), and looks for it for 30 s. Agents 0.2.7 and 0.2.8 ignore
both fields. The current directory is the agent's folder (the
shared one, where *Copy Files or Folder to Client* puts files) and the batch
file has the rights the agent has. The output is not recorded; a batch file
whose output is wanted on the host redirects it itself, for example to a file
in the shared folder. The batch file stays in the `batch` folder, which *Open
Client's Shared Folder* reaches as `\\NAME\<share_name>\batch`. Agent 0.2.7
ran batch files without a window, with input from `NUL` and the output in
`batch\NAME.log`.

The agent answers twice. The first ACK follows at once:

```
ACK
cmd=BATCH
stage=started
ok=yes
error=
file=batch-20261105-231500.bat
```

The second follows when `cmd.exe` has ended, with `stage=finished`, `exit=`
the exit code, `seconds=` the running time and `ok=yes` only for exit code 0.
A batch file that waits for a key therefore stays unfinished until someone
presses one at the node or closes the window.
The agent does not wait for the batch file: reports and other commands go on
while it runs, and several can run at once. A batch file still running when
the agent is stopped, restarted or updated keeps running, but its end is then
not reported. Agents older than 0.2.7 do not know BATCH; Thunderhead does not
send it to them and writes a line to the event log instead.

## Wake On LAN

*Send Wake On LAN Magic Packet* needs no agent, since its purpose is a client
that is switched off. For every checked client, online or not, Thunderhead
builds the magic packet from the MAC address the client is stored under (six
`FF` bytes, then the six bytes of the MAC sixteen times, 102 bytes) and sends
it as a UDP broadcast to port 9: once to the directed broadcast address of
every IPv4 interface of the host that is up, and once to 255.255.255.255. The
event log names the client, the MAC and the addresses. Nothing answers a magic
packet; the client going online is the only sign that it worked. The stored
`wol` flag of a client is not consulted.

The packet reaches clients on the subnets the host is attached to. A client
starts only when Wake on LAN is enabled in its firmware and its network adapter
is set to wake on a magic packet; on Windows 8 and 10 Fast Startup usually has
to be off as well, because an adapter often does not wake from the hybrid
shutdown.

## Ping

*Ping LR* (LightningRod Control) tells every checked client to report at once. Thunderhead sends PING
to each checked client it holds as online and writes "NAME is offline, not
pinged" for the others. An agent from 0.2.11 answers with a REPORT that
carries every value, not only the changed ones, and restarts its report timer
from that moment; the event log then reads "report from NAME N ms after
PING". An older agent answers PONG, which the event log notes as such with
the agent version. A client that sends neither within 5 s is marked offline
with "no report within 5 s of PING", as it would be after three missed
reports, and reconnects by itself when its agent is alive. The status bar
keeps the count: checked, sent, replied, no reply, offline.

*Network Ping* is the plain ICMP echo test, three requests of 32 bytes with
a 1 s timeout each, as `ping -n 3` sends, to every checked client whether
Thunderhead holds it as online or not. The address used is that of the
client's connection, else the one its last HELLO carried, else its name
resolved. All clients are pinged at the same time and the replies, timeouts
and round-trip summary of each appear in a text box as that client finishes,
followed by a count of clients that replied; OK closes the box. The event
log notes the clients pinged. No agent takes part, so this reaches a client
whose agent is not running.

## Log viewers

*View Local Log* opens the current Thunderhead event log,
`logs\thunderhead-YYYY-MM-DD.log`, in a window of its own. *View Remote Log*
opens one window per checked client with the agent's own log, which the agent
writes as `LightningRod-YYYY-MM-DD.log` beside itself, in the root of the
node's share: Thunderhead reads `\\IP\<share_name>\` under the address the
client last reported from, then `\\NAME\<share_name>\` when the name resolves
to that address, and takes the newest file by name. A client that is offline
is tried too, because its share may answer when its agent does not; the window
shows the error when nothing does. A share that refuses the credentials
Windows offers brings up the standard credential prompt, as for the update,
and the file is read once more when they are accepted. The windows are read-only text, scrolled
to the end when opened, with a Refresh button (or F5) that reads the file
again and Ctrl+A to select the text for copying. A file above 4 MB is shown
from its last 4 MB. The windows are independent of the main window and are
not updated as the log grows.

## Audio

*Mute Checked Clients* and *Unmute Checked Clients* send `AUDIO action=mute`
or `action=unmute` to every checked online client. The agent sets, rather
than toggles, the master mute of the node's default playback device: through
Core Audio (IAudioEndpointVolume on the default render endpoint) on Vista and
later, through the multimedia mixer API (the mute control of the first mixer's
speaker line) on XP, where Core Audio is not registered. The ACK carries
`action=`, `muted=yes|no` as read back from the device afterwards and
`method=Core Audio|mixer API`; `ok=no` with `error=` when neither API could
set the state, when the node has no playback device, or when the state read
back differs from the one asked for. The event log reads "AUDIO mute ok on
NAME: audio muted via Core Audio" or "AUDIO mute failed on NAME: ...".
Agents older than 0.2.16 do not know AUDIO; Thunderhead skips them with a line
in the event log, as for the other versioned commands.

The *All Lightning Rods Audio* switch of Settings sends the same command
without a click: `AUDIO action=unmute` while the switch is on and
`action=mute` while it is off, to every client as its HELLO arrives, and to
every online client when the switch is changed. The *Thunderhead Audio*
switch beside it silences Thunderhead's own sounds, described next, and the
*Play Here* buttons; neither switch is tied to the other.

### Sound files, categories and schemes

Both programs play `.wav` files through PlaySound, from a copy held in memory
after the first play so that an event sound starts without a disk read, and
`.mp3` files through the multimedia command interface (MCI, the MPEGVideo
device), which decodes in the process itself: no player program, no window,
and the device is closed the moment the sound has ended. A sound still
playing gives way to the next. The files live in `sounds\` beside each
executable. The 38 files of the repository's `assets\sounds`, named
`ThunderheadLightningRod <Category>.wav`, define the 38 categories and form
the default scheme, in which each category plays its own file. The custom
scheme, edited in the *Sound scheme* section at the bottom of Thunderhead's
Settings dialog, assigns another file of the folder to a category: *Browse* takes a `.wav` or `.mp3`, copies it into the
folder when it comes from elsewhere (a file bearing a default scheme name is
never replaced), and assigns it to the selected category; *Use Default* drops
the assignment; *Play* and a double click play the file; *Default* and
*Custom* choose the scheme in force. The scheme is saved in `settings.txt`
(`sound_scheme`, `sound.<Category>`) when Settings is closed with OK and, since
the agents' own events follow it, sent to every online agent as described
below.

### Sound events

| App | Event | Category (default file) |
|-----|-------|-------------------------|
| TH | App Opens | Startup |
| TH | App Receives HELLO from Client | Proximity Notification |
| TH | App Closed or Closes | Shutdown |
| TH | Client that was Online went Offline | Minimize |
| LR | App Opens | Logon |
| LR | App Sends HELLO to Host | Proximity Connection |
| LR | App Closed or Closes | Logoff Sound |
| LR | App Sends Report to Host | Navigation Start |
| LR | App Finds No Workable IP Address | Battery Low |

Thunderhead plays its four itself, the closing one to its end (5 s at most)
before the window goes. The agent plays Logon as its window appears, Proximity
Connection when Thunderhead's SET answers its HELLO, which is the same
exchange, Navigation Start after every REPORT it sends (the regular ones and
the one a PING asks for), Battery Low each time its start-up address check
reads an unworkable address (the first reading and each of the three re-reads
30 s apart, so four times before the network error box when no address ever
comes), and the Logoff Sound to its end (4 s at most) before
it stops for EXIT, RESTART, UPDATE or a closed window; its log has one line per
sound, with the file, or the reason it did not play, except that the report
sound is logged only when it fails.

The agent plays each event's file from its own `sounds` folder: the
category's default file, or the file that Thunderhead's scheme message named.
After every HELLO, and whenever the scheme is changed in Settings, Thunderhead
sends agents from 0.2.21

```
SOUND
action=scheme
logon=
hello=fanfare.mp3
logoff=
report=
badip=
```

with the file name assigned to the category of each of the agent's events,
empty for the default. The agent keeps the names in `sounds\scheme.txt` and
reads them at its next start, so that the Logon sound honours them too; a name
holding a path separator is treated as empty. The ACK carries `action=scheme`.

### Play Sound

*Play Sound* lists the `.wav` and `.mp3` files of Thunderhead's `sounds`
folder. *Play Here* (or a double click) plays the selected file on the host;
*Play to Clients* sends

```
SOUND
action=play
file=ThunderheadLightningRod Ding.wav
```

to every checked online client, which plays the file of that name from its
own `sounds` folder and answers at once, `ok=yes` when playback started, or
`ok=no` with `error=` when the name is not in the folder, holds a path
separator, or the device refused it. The event log reads "SOUND play ok on
NAME: file" or "SOUND play failed on NAME: ...". Agents older than 0.2.21 do
not know SOUND; Thunderhead skips them with a line in the event log, as for
the other versioned commands, and a node whose agent came by an older update
has no `sounds` folder until *Update LR* copies it.

## Display Message

*Display Message* (Comms) opens one dialog per checked online
client, in list order, naming the client. Its radio buttons are the 38 sound
categories and, last, *Silent message*; the text box takes the message text,
which may be left empty; the *Display* switch chooses *Until Dismissed* or
*Timed*, with the seconds beside it (1 to 86400). The choices carry over from
one dialog to the next. *Send* sends the message to that client, *Skip* leaves
the client without one, *Cancel All* ends the pass. Clients whose agent is
older than 0.3.3 are passed over with a line in the event log.

```
MESSAGE
title=Message from Thunderhead: Balloon
text=52656e646572696e672073746f707320617420313a3030
sound=ThunderheadLightningRod Balloon.wav
timeout=30
```

`title=` is "Message from Thunderhead: <Category>" for a sound category and
"Message from Thunderhead" for a silent message; `text=` holds the UTF-8 bytes
of the text as hexadecimal digits, as BATCH does, so that line ends survive the
one-line field format; `sound=` names the category's file in the client's own
`sounds` folder (the custom scheme's assignment when there is one) or is empty
for a silent message; `timeout=` is the seconds after which the window closes
by itself, 0 for until dismissed. The event log and the node log record the
title, the length of the text and the timing, not the text.

The agent plays the sound, if any, and opens a window of its own on the node's
screen, brought in front of the other windows: the title in its caption, the
text in a large font, an *OK* button, the Minimize, Maximize (Restore) and
Close buttons of an ordinary window, and a line reading "Closes by itself in
N s", counting down, or "Stays until dismissed". Enter presses OK and Escape
closes. Several messages may be open at once; the agent's reports and other
commands go on meanwhile. The agent answers twice: at once with `stage=opened`,
`title=`, `sound=`, `sound_error=` (empty when the sound played; the window
opens even when the sound did not) and `ok=yes`, or `ok=no` with `error=` when
the text is not hexadecimal or the title is missing; and when the window has
closed, with `stage=closed`, `closed=ok` (the OK button or Enter), `close` (the
Close button, Escape or Alt+F4) or `timeout`, and `seconds=` the time it was
open. The event log reads "MESSAGE "Message from Thunderhead: Balloon" shown
on NAME" and later "MESSAGE "..." closed on NAME with OK after 12 s". A window
still open when the agent stops, restarts or is updated closes with it, and
its end is then not reported.

## TeamViewer

*Start TeamViewer* sends `TEAMVIEWER` to every checked online client. The
agent looks for the installed TeamViewer without assuming a version or a
folder, in this order: the product key `HKLM\SOFTWARE\TeamViewer`
(`InstallationDirectory`, and the `VersionN` subkeys that releases 6 to 10
used), the Add/Remove Programs entries whose key name starts with `TeamViewer`
(`InstallLocation`, then the executable named by `DisplayIcon`, then the
folder of `UninstallString`), `App Paths\TeamViewer.exe`, the shortcuts of the
Start menu for all users and for the logged-on user whose name contains
TeamViewer (resolved through IShellLink), and `TeamViewer\TeamViewer.exe`
under each Program Files folder. The 64-bit and the 32-bit registry views are
both read, since TeamViewer 15 exists in both builds. The first path that
exists is started as a double-click on the shortcut would start it; when
TeamViewer is already running, the new copy hands over to the running one,
which shows its window, as TeamViewer itself arranges. The agent then looks
for the largest visible top-level window of a `TeamViewer.exe` process, for
up to 30 s, and brings it to the front in the way described under Remote
batch file. The ACK carries `path=` (the executable started), `found=` (the
source that named it, such as `registry HKLM\SOFTWARE\TeamViewer (32-bit view)`
or `Start menu shortcut TeamViewer.lnk (all users)`) and `running=yes|no`
(whether a `TeamViewer.exe` existed before the start); `ok=no` with `error=`
when no source names an existing executable or Windows refuses the start.
The event log reads "TEAMVIEWER ok on NAME: started C:\...\TeamViewer.exe,
found through ..." or "TEAMVIEWER failed on NAME: ...". A node whose
TeamViewer service is not running gets TeamViewer's own request for elevation
to start it, on the node's screen, before any window appears; the agent's
30 s search ends with "window not found" in its log when the prompt is still
waiting, and the window is left where TeamViewer opens it. Agents older than
0.2.18 do not know TEAMVIEWER; Thunderhead skips them with a line in the event
log, as for the other versioned commands.

## Lightning lookup

*Run Lightning* (Render Control) asks the generation with one radio per
Lightning, *3 (B7 & 7 Pro)*, *2 (B6.x)* and *1 (B5.x)* (the last disabled until
Bryce 5 is supported), and sends `START gen=3`, `gen=2` or `gen=1` to every
checked online client.
The agent does not assume a folder: it looks the installed Lightning of that
generation up, in this order, and takes the first `Lightning.exe` that exists:

1. the installer's product key under `HKLM\SOFTWARE`, read in the agent's own
   registry view and in the other one (a 32-bit installer writes
   `SOFTWARE\WOW6432Node` on 64-bit Windows, which is what the 32-bit agent
   reads without a view flag; on 32-bit Windows both views are the same key):
   `Location` names the folder and `Version` the installed version;
2. the Add/Remove Programs entry under `...\CurrentVersion\Uninstall`, in both
   views: `InstallLocation` names the folder and `DisplayVersion` the version;
3. the Start menu shortcut, for all users and then for the logged-on user,
   resolved through IShellLink and accepted only when it points at a
   `Lightning.exe`;
4. the default folder under each Program Files folder.

The file's own version resource is then read; when the installer's entry gave
no version, that is the version reported. A Bryce 7 Lightning is known to
carry 7.x there, so Lightning 3 accepts only a file whose first number is 7;
the numbering of the older files is not yet verified, so Lightning 2 accepts
any file except one whose first number is another generation's (7 or 5). A
file that fails is passed over for the next source, and when nothing else
names one the start is refused with `error=` naming the file and its
version, so that a folder holding another generation's Lightning is never
started under the wrong button.

Each DAZ installer writes a product key under `HKLM\SOFTWARE\DAZ 3D`
(`HKLM\SOFTWARE\WOW6432Node\DAZ 3D` on 64-bit Windows) named after the product
and its version, with `Location` and `Version`. For generation 3 the key is
`Bryce Lightning 7.0` (verified on 7.1.0.109), the Add/Remove Programs key is
named `Bryce Lightning 7.0 <version>`, the shortcut is
`DAZ 3D\Bryce Lightning\Bryce Lightning 7.0.lnk`, and the default folder is
`DAZ 3D\BryceLightning7`. For generation 2 the key is `Bryce Lightning 6.3`
(verified on the registry export of a 32-bit Windows XP node: version
6.3.0.84, Location `C:\Program Files\DAZ 3D\BryceLightning`); the agent tries
every key whose name starts with `Bryce Lightning 6`, highest version first,
so a 6.1 or 6.0 installer following the same pattern is found as well, and
the default folders tried are `DAZ 3D\BryceLightning` and
`DAZ 3D\Bryce Lightning`. The entries of generation 1 are not yet recorded;
until they are, the agent answers `START gen=1` with `ok=no` and an error
saying so, and Thunderhead keeps that control inactive. Thunderhead sends
`gen=2` and `gen=1` only to agents from 0.2.20, because older agents would
start the Bryce 7 Lightning in their place; the others are skipped with a
line in the event log.

The ACK carries `gen=`, `path=` (the file), `version=` and `found=` (the
source, such as `registry HKLM\SOFTWARE\WOW6432Node\DAZ 3D\Bryce Lightning 7.0\Location`
or `Start menu shortcut Bryce Lightning 7.0.lnk (all users)`), also when the
start fails because Lightning is already running, in which case `error=`
names the running file. The event log reads "START ok on NAME: Lightning 3
7.1.0.109 at C:\...\Lightning.exe, found through ..." or "START failed on
NAME: ...". When the agent restarts Lightning after acquiring a late address
(see Agent start-up), it restarts the copy that was running, whatever its
generation. The same lookup runs once at the agent's start and fills
`lightning3_path`, `lightning3_version`, `lightning2_path` and
`lightning2_version` in HELLO.

## Clock

*Set Client Date and Time* sends TIME to every checked online client with
`utc_ms=` the host's clock in milliseconds since 1970-01-01 00:00 UTC, `date=`
the host's local calendar date and `tolerance=180`. The agent compares its own
clock with those: when its local date is not `date`, or its UTC time is
`tolerance` seconds or more from `utc_ms` either way, it sets the system clock
to `utc_ms` (SetSystemTime, which needs the agent to run with administrator
rights) and then sends a full REPORT so that the corrected clock shows at
once; a clock closer than that is left alone. The ACK carries `set=yes` or
`no`, `delta_s=` the node's clock minus the host's in whole seconds before any
change, and `date=` the node's local date before any change; `ok=no` with
`error=` when the clock was wrong but could not be set. The event log reads
"TIME: NAME clock was 245 s behind (its date 2026-11-06), set to the host's
time", "TIME: NAME clock 2 s ahead, left alone" or "TIME: NAME clock 245 s
behind, not set: ...". The host's own clock is taken as right. Agents older
than 0.2.12 do not know TIME; Thunderhead skips them with a line in the event
log, as for the other versioned commands.
