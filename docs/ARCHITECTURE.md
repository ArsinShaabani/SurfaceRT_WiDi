# Architecture

```
 Windows 10/11 PC (nothing installed)                 Surface RT 8.1 (ARM)
 ┌──────────────────────────────────────┐            ┌──────────────────────────────────┐
 │ Send-Screen.ps1                      │            │ widi_service.exe (Windows service)│
 │  ├─ WiDiNative.cs (compiled in RAM)  │            │   └─ keeps widi_sink.exe alive in │
 │  │   ├─ GDI capture + JPEG encode    │  TCP 47000 │      the interactive session      │
 │  │   ├─ CRC32 + protocol encoder     │ ─────────► │                                  │
 │  │   └─ SendInput (touch -> mouse)   │ ◄───────── │ widi_sink.exe                     │
 │  └─ adaptation loop (Stats)          │  Stats/    │  ├─ SinkEngine (core, portable)   │
 │                                      │  Input     │  │   ├─ TcpListener + SinkSession │
 │  discovery: UDP 47001 beacons  ◄─────┼────────────┼───┤   ├─ RateController            │
 └──────────────────────────────────────┘            │  │   └─ WIC decode -> Presenter  │
                                                     │  └─ D2D (D3D-backed) or GDI blit   │
                                                     └──────────────────────────────────┘
```

## Components

| Path | Role | Runs where |
|---|---|---|
| `include/widi/protocol.h`, `src/core/protocol.cpp` | wire format, CRC, payload codecs | everywhere |
| `include/widi/session.h`, `src/core/session.cpp` | framing, handshake, resync, validation | everywhere |
| `include/widi/sink_engine.h`, `src/core/sink_engine.cpp` | listener, discovery beacon, latest-frame pacing, stats cadence | everywhere |
| `include/widi/rate_control.h`, `src/core/rate_control.cpp` | deterministic adaptation policy | everywhere |
| `src/core/net_posix.cpp` / `src/platform/winrt8/net_win32.cpp` | sockets | dev host / RT |
| `src/platform/winrt8/image_decoder.*` | WIC JPEG -> BGRA8 | RT |
| `src/platform/winrt8/presenter.*` | Direct2D HWND render target, GDI fallback | RT |
| `src/platform/winrt8/sink_app.cpp` | full screen window, touch/mouse capture, HUD, power | RT |
| `src/platform/winrt8/widi_service.cpp` | SCM service, launches the UI in the console session | RT |
| `src/apps/console/*.cpp` | reference sink + synthetic sender for Linux | dev host |
| `tools/pc/Send-Screen.ps1`, `tools/pc/lib/WiDiNative.cs` | capture/encode/inject sender, compiled in memory | PC |
| `tools/dev/wire_check.py` | byte-exact port of the C# encoder for cross-language checks | dev host |
| `tests/test_main.cpp` | 22 test cases (~4,200 assertions): codecs, session, engine, sockets, adaptation, fuzzing | dev host |

## Threading model

- **Tablet:** one UI thread does everything: message pump, `engine.pump()`,
  decode, present, input. That is deliberate - Tegra 3 has four slow cores and
  no core to spare for a decode queue. A second thread would only add copies and
  lock contention.
- **Service:** supervisor loop with a 1 s tick, `CreateProcessAsUser` into the
  active console session, restart-on-exit with exponential backoff
  (5 s, 15 s), `SERVICE_CONTROL_SESSIONCHANGE` handling for logon/unlock.
- **PC:** single-threaded script; capture, encode and send are sequential and
  paced by a `Stopwatch`, feedback is drained non-blockingly between frames.

Because services cannot show UI, `widi_service.exe` never draws anything: it
only re-launches `widi_sink.exe` inside `winsta0\default` of the console session.

## Latency budget (what actually costs time)

| Stage | 1366x768 | Notes |
|---|---|---|
| GDI BitBlt capture on PC | 1-4 ms | whole virtual desktop |
| JPEG encode (PC, GDI+) | 8-18 ms | 24-bit, quality 60 |
| LAN transfer | 5-25 ms | ~60-100 KB/frame at 2.4 GHz |
| JPEG decode (Tegra 3, WIC) | 18-35 ms | the bottleneck; measured by the sink |
| D2D present | 2-6 ms | hardware scaled blit |
| **Total** | **~40-85 ms** | 12-20 fps sustained, 1 frame queued at most |

The loop is explicitly **latest-frame-wins**: if a new picture arrives while one
is still pending, the older one is discarded and counted as `dropped`. This
keeps glass-to-glass latency bounded instead of letting a queue grow (the usual
reason "wireless display" demos feel sluggish).

## Key design decisions

1. **JPEG, not H.264, in v1.** Baseline JPEG is decodable by WIC without a
   pipeline graph, has no keyframe/GOP latency and degrades gracefully. H.264
   (Media Foundation MFT, Tegra 3 has a hardware decoder) is the natural v2 - see
   `ROADMAP.md` - but it needs RTP-style loss handling, which is a much larger
   correctness surface than the app needs today.
2. **Raw BGRA is also allowed** (format 0). It is useless over Wi-Fi (4 MB per
   frame) but it makes the protocol testable on a loopback link without an
   encoder, which is how `tests/` exercises the engine.
3. **Direct2D, not D3D11 shaders.** `ID2D1HwndRenderTarget` is the shortest path
   to a hardware-accelerated, correctly scaled blit and works from feature level
   9_x upwards, which is what Tegra 3 exposes on RT. The GDI `StretchDIBits` path
   remains as a fallback if the render target cannot be created or is lost.
4. **No display driver, no kernel code, no filter driver.** Everything is
   user-mode, so the jailbreak is only needed to run unsigned ARM binaries - not
   to install kernel components (which RT would refuse anyway).
5. **Discovery by UDP broadcast, not mDNS.** The PC side is a PowerShell script
   that must stay dependency-free; broadcast is 40 lines and deterministic,
   while mDNS would need a responder/resolver on both ends for no benefit on a
   flat home LAN. `-SinkHost` always works when broadcast is filtered.
6. **Progressive JPEG is accepted** by WIC, so senders are not forced to emit
   baseline-only streams.

## Failure handling

| Failure | Behaviour |
|---|---|
| Sender disappears | `recv` returns 0 -> session reset, `Idle`, beaconing continues |
| Corrupt payload | CRC mismatch -> frame dropped, `protocol_errors++`, session survives |
| Garbage mid-stream | one-byte resync, session survives |
| Decode error | counted as dropped, sender notified through Stats, quality lowered |
| D2D device lost | target released, next frame re-creates it, GDI fallback if needed |
| Sink app crash | service restarts it within ~1 s |
| Session change / unlock | service re-checks the window and relaunches if needed |
| Port already in use | `SinkEngine::start` fails with a message written to `widi.log` |
