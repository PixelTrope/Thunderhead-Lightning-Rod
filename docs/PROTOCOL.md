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
mem_installed_mb=4096
mem_type=DDR3
disk_total_mb=3889
board=Dell Inc. 0XXXXX
chipset=AMD A55E Host Bridge
gpu=AMD Radeon HD 6310 Graphics
vram_mb=384
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
| SHUTDOWN |             | reply ACK, then shut the node down after 5 s, closing programs by force (no longer sent by Thunderhead, which uses POWER) |
| BATCH | text, maximized, front | write the text (hexadecimal digits of its UTF-8 bytes) to a batch file and run it; reply ACK when started and again when ended (see Remote batch file); agents from 0.2.7 |
| POWER | action        | run Windows' `shutdown.exe` and reply ACK with the action: `shutdown` (`-s`), `restart` (`-r`) or `coldrestart` (`-g`), each with `-f -t 66`, or `abort` (`-a`); agents from 0.2.6 |
| AUDIO | action        | set the master mute of the default playback device: `mute` or `unmute`; reply ACK with the state read back (see Audio); agents from 0.2.16 |

The agent replies to START, STOP, PRIORITY, EXIT, RESTART, UPDATE, RENAME, REBOOT,
SHUTDOWN, POWER, AUDIO and BATCH with (RENAME adds `name=` with the name that was applied;
PRIORITY adds `level=` as requested and `applied=` with the class Windows reports
afterwards, and answers `ok=no` when the two differ, as when RealTime is
requested without administrator rights and Windows grants High; POWER adds
`action=` and answers `ok=no` with the exit code of `shutdown.exe` when that
is not 0; BATCH is answered twice, as described under Remote batch file):

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

## Remote batch file

*Run Remote Batch File* opens a dialog with a text box for the batch file, which
can be typed, pasted, or loaded from a file with *Browse for file* (UTF-16 or
UTF-8 by byte order mark, otherwise UTF-8 when the bytes are valid UTF-8,
otherwise the ANSI code page of the host). The text box keeps its content
while Thunderhead runs, as do the two check boxes *Start maximized* and *Bring
to front*, which are set when Thunderhead starts. *Execute Remote Batch File* first shows a warning that
nothing checks the text for errors or harmful code, with *Yes, Execute* and
*No, Cancel*; only *Yes, Execute* sends it.

Thunderhead sends BATCH to every checked online client with `text=` holding the
UTF-8 bytes of the text as hexadecimal digits, two per byte, so that the line
ends survive the one-line field format, and with `maximized=` and `front=`
(`yes` or `no`) from the two check boxes. The text may be 30000 bytes long, which
keeps the message below the 65536 byte limit. The event log and the node log
record the size of the text, not the text.

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
shared one, where *Copy files or folder to Client* puts files) and the batch
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

*Lightning Rod Ping* tells every checked client to report at once. Thunderhead sends PING
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
`logs\thunderhead-YYYY-MM-DD.log`, in a window of its own. *View Remote Log(s)*
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
