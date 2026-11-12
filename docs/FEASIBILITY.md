# Feasibility: can a Surface RT be a "Wireless display" target for Win+K?

Short answer: **no, not with the stock Windows RT 8.1 stack and not with any
application that Microsoft allows to run on it.** The reasons are architectural,
not a matter of effort. This document records the evidence, because it defines
why SurfaceRT-WiDi is built the way it is.

## 1. What the two sides of Miracast actually require

A Miracast session has a discovery phase and a transport phase
([Wi-Fi Direct](https://www.wi-fi.org/discover-wi-fi/wi-fi-direct) +
[WFD/RTSP](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-mice/940d808c-97f8-418e-a8a9-c471dc0d21bb)):

| Phase | Sink (the tablet) must be able to... | Available on RT 8.1 / ARM? |
|---|---|---|
| Wi-Fi Direct discovery | put **WFD + WSC information elements into its own Beacon / Probe-Response frames** (i.e. act as a P2P Group Owner or P2P Device) | **No.** Apps get no control over 802.11 management frames. |
| WFD capability exchange | answer WFD M1 with M2 (RTSP, port 7236) | Technically yes (a TCP/RTSP server is just sockets), but unreachable: nobody can find the sink without the IEs above. |
| Session/crypto | WPA2-PSK (or WPS) P2P group formation, optional HDCP 2.x for protected content | **No.** WFD group formation is done by the WLAN miniport; RT's driver exposes no such entry point, and HDCP is absent entirely. |
| Media | MPEG-TS over RTP, H.264 decode, render | Partly: Media Foundation + Tegra 3's H.264 decoder exist, but they are only useful *after* a session exists. |

In other words: the hard blocker is the first row. A Miracast sink is a **Wi-Fi
Direct peer**, and Windows RT 8.1 gives applications no way to become one.

## 2. "But Windows 10 can project to another PC, why not RT?"

Because that capability is *not* an app. Microsoft shipped it as an
IHV/OEM-facing Wi-Fi driver API set together with the August 2014 Windows 8.1
update. The published description is explicit:

> *"Miracast Receive - exposes a set of Wi-Fi direct APIs for Independent
> Hardware Vendor (IHV) drivers or OEM drivers to develop Windows 32-bit
> applications that run on all supported **x86-based or x64-based** versions of
> Windows 8.1, enabling the computer as a Miracast receiver."*
> - Microsoft, August 2014 update notes (quoted by
>   [borncity](https://borncity.com/blog/2014/11/10/how-to-turn-a-windows-8-1-machine-into-a-miracast-receiver/))

Three things follow, all of which exclude the Surface RT:

1. **x86/x64 only** - the API set was never produced for ARM.
2. **IHV/OEM only** - it requires a WLAN driver that implements the WFD sink
   role; third parties (AirServer, Connect app) ship by cooperating with an
   Intel/Broadcom/Qualcomm driver, not by talking to the OS alone.
3. **Windows RT has no such driver hook at all**, and RT accepts only
   Microsoft-signed binaries.

Windows 10/11's own "Projecting to this PC" feature has the same requirement:
Settings > System > Projection > *Optional features > Wireless Display* installs
the WFD sink component, and it is unavailable on ARM RT builds.

## 3. "Can we avoid Wi-Fi Direct with Miracast over Infrastructure (MICE)?"

This was the most promising path and it does **not** work, for a documented
reason. MS-MICE still starts with Wi-Fi Direct discovery:

> *"The Miracast over Infrastructure session starts with peer to peer (P2P)
> device discovery ... The Source sends Probe Request frames and listens for
> Probe Response frames and Beacon frames. ... If the Source receives a Beacon or
> Probe Response that contains a WSC IE Vendor Extension attribute, the Source
> checks the Capability attribute for Miracast over Infrastructure support."*
> - [MS-MICE 3 Protocol Details](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-mice/940d808c-97f8-418e-a8a9-c471dc0d21bb)

Only *after* that does the source resolve the sink's host name over mDNS and
connect to TCP 7250. Microsoft's implementation guidance repeats the same first
requirement for sink vendors:

> *"It is required that every Beacon and Probe Response the Receiver sends MUST
> include the Vendor Extension attribute."*
> - [Implementing Wireless Projection over an existing Wi-Fi network](https://learn.microsoft.com/en-us/windows-hardware/design/device-experiences/wireless-projection-implementing-over-existing-network)

Publishing an mDNS record (`<name>._display._tcp.local` on port 7250) is not
enough: `Win+K` never looks for a sink that has not first appeared as a Wi-Fi
Direct P2P device in its scan results.

## 4. What *is* possible (and what this repository does)

| Option | PC side needs | Extended desktop? | Verdict |
|---|---|---|---|
| Native Miracast sink on RT | nothing (Win+K) | yes | **Impossible** (sections 1-3) |
| Virtual display driver (Indirect Display, IddCx) + our own protocol | driver install | yes | Not "nothing on the PC", and IddCx does not exist on RT; **rejected by the requirement** |
| **This project: LAN wireless display** - GDI/DXGI capture + JPEG + TCP, zero install on the PC | a PowerShell script (compiled in memory, nothing persisted) | mirrors the desktop (extend would need a display driver) | **Implemented and tested** |
| Built-in Remote Desktop (mstsc on RT, RDP host on Win10/11 Pro) | enable RDP (no install) | no (session takeover) | documented fallback, higher latency |

SurfaceRT-WiDi therefore implements the third row properly: a real receiver
stack (protocol, adaptive rate control, decode, GPU presentation, service
hosting) on the tablet, and a **zero-install** sender on Windows 10/11 that uses
only components already present in the OS.

## 5. Hardware envelope we design for

First-generation Surface RT (confirmed specs): Tegra 3 (4x Cortex-A9 @ 1.3 GHz,
ARMv7-A), 2 GB RAM, 1366x768 panel, 802.11 a/b/g/n
([Wikipedia](https://en.wikipedia.org/wiki/Surface_(2012_tablet))).

Practical consequences encoded in this project:

- **No 1080p JPEG at 30 fps.** A 1080p baseline JPEG costs 30-60 ms on a single
  Cortex-A9 core; we therefore stream 1366x768 or less by default, adapt
  quality/frame rate from measured decode time (see `RateController`), and drop
  stale frames instead of queuing them.
- **~20-40 Mbit/s usable link** (single-stream, shared 2.4 GHz band). At
  1366x768 / q60 / 20 fps each frame is roughly 60-100 KB, i.e. 10-16 Mbit/s -
  fits with headroom for input remoting and background traffic.
- **2 GB RAM** means no unlimited frame queues: the engine keeps exactly one
  pending frame and one "last presented" frame.
- **No HDCP**: protected video (Netflix etc.) will show black in the mirrored
  image. That is a property of the approach, not a bug in the code.
