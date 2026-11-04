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

Thunderhead replies with the report interval in seconds (multiple of 5):

```
SET
interval=5
```

The agent then sends a REPORT every `interval` seconds. Only values that change
during a session are in it; everything fixed for the session is in HELLO and
is never repeated, and Thunderhead never polls:

```
REPORT
cpu_pct=12
mem_free_mb=2900
disk_free_mb=1040
uptime_s=12345
lightning=running
lightning_cpu_pct=97
rendering=yes
```

`lightning` is `running` or `stopped`. `rendering` is `yes` when Lightning.exe
owns an established TCP connection (a job from the Bryce host), otherwise `no`.
`lightning_cpu_pct` is Lightning's share of the whole machine over the last
interval.

Thunderhead may send at any time:

| Verb  | Fields        | Agent action                                           |
|-------|---------------|--------------------------------------------------------|
| SET   | interval      | change the report interval                             |
| START |               | start `C:\Program Files\DAZ 3D\BryceLightning7\Lightning.exe` |
| STOP  |               | terminate Lightning.exe                                |
| PING  |               | reply PONG                                             |

The agent replies to START and STOP with:

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
interval=5
tcp_port=42002
```

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
comment=fan noisy
```

`logs\thunderhead-YYYY-MM-DD.log` holds every event with a timestamp.
`logs\nodes\NAME-YYYY-MM-DD.log` holds every message to and from that node.

## Agent files

The agent lives in `C:\LANCMD` on each node and writes
`C:\LANCMD\LightningRod-YYYY-MM-DD.log`. It keeps no other state.
