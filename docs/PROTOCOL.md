# SurfaceRT-WiDi protocol v1

Binary, little-endian, one TCP connection per session (default port 47000).
Discovery uses one UDP datagram type (default port 47001). Everything is
length-prefixed and CRC-checked; no message is trusted before validation.

## Message framing

Every message, in both directions, is:

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | magic | `0x49444957` = bytes `W I D I` |
| 4 | 1 | version | `1` |
| 5 | 1 | type | see table below |
| 6 | 2 | flags | message specific, 0 in v1 |
| 8 | 4 | sequence | monotonic per sender, informational |
| 12 | 4 | length | payload bytes, max 16 MiB |
| 16 | 4 | crc32 | CRC-32 of the payload only (reflected, poly 0xEDB88320) |
| 20 | n | payload | type specific |

A receiver that sees an invalid header skips **one byte** and rescans, so a peer
that restarts mid-message cannot wedge the session (see
`test_session_resync_after_garbage`). A payload whose CRC does not match is
dropped and counted in `protocol_errors`.

| Type | Name | Direction | Purpose |
|---|---|---|---|
| 1 | Beacon | sink -> broadcast (UDP) | discovery only |
| 2 | Hello | sender -> sink | opens the session |
| 3 | HelloAck | sink -> sender | accept/reject + sink geometry |
| 4 | Frame | sender -> sink | one complete picture |
| 5 | Stats | sink -> sender | decode feedback (drives adaptation) |
| 6 | Input | sink -> sender | pointer/wheel remoting |
| 7/8 | Ping/Pong | both | keepalive, echoed verbatim |
| 9 | Bye | both | graceful teardown, payload[0] = reason |

## Discovery beacon (UDP, broadcast + loopback)

| Offset | Size | Field |
|---|---|---|
| 0 | 6 | magic `W I D I D S` |
| 6 | 1 | version |
| 7 | 2 | TCP port of the sink |
| 9 | 2 | screen width |
| 11 | 2 | screen height |
| 13 | 1 | capability bits |
| 14 | 1 | name length |
| 15 | n | UTF-8 name |

Emitted every 2 s. The sender also gets a copy on `127.0.0.1`, so a sender and a
sink on the same machine work during development.

## Hello / HelloAck

Hello payload: `u16 version | u16 maxWidth | u16 maxHeight | u8 maxFps | u8 caps |
 u8 nameLen | name`.

HelloAck payload: `u16 version | u16 screenW | u16 screenH | u32 sessionId |
 u8 flags (1 = accepted) | u8 reason | u8 nameLen | name`.

Capability bits: `0x01` JPEG, `0x02` raw BGRA, `0x04` input remoting,
`0x08` touch events. A version mismatch is answered with `flags = 0`,
`reason = 1` and the session closes.

Frames arriving before the handshake are ignored but not fatal - a sender that
crashes and restarts can complete its Hello on the same connection.

## Frame

Payload = 10-byte header + pixels:

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | width |
| 2 | 2 | height |
| 4 | 1 | format: 0 = BGRA8 rows, 1 = JPEG stream |
| 5 | 1 | flags (reserved) |
| 6 | 4 | timestamp ms (sender clock, informational) |
| 10 | n | pixels |

A BGRA frame must carry at least `width * height * 4` bytes. A JPEG frame is a
complete JFIF stream (baseline or progressive; the receiver decodes with WIC).

## Stats (the closed loop)

`u32 decoded | u32 dropped | u16 decodeMs*16 | u16 clientFps*16`, sent every
500 ms by the sink.

The sender uses it exactly like `RateController` does: budget = `1000/fps * 0.7`;
if measured decode time exceeds the budget, or the drop counter grew, it first
lowers frame rate (20 -> 16 -> 12), then scale, then JPEG quality; after enough
healthy windows it walks back up in the opposite order. Adaptation is therefore
symmetric on both ends and testable without a PC (`tests/test_main.cpp`).

## Input

`u8 kind | u8 flags | u16 x | u16 y | u32 data`

- Coordinates are normalised to `0..65535` across the desktop, so the sender can
  map them to its virtual screen without knowing the tablet's panel size.
- `kind`: 0 move, 1 button, 2 wheel, 3 key (reserved), 4 leave.
- `flags`: `0x01` left, `0x02` right, `0x04` middle, `0x08` down,
  `0x10` touch.
- Wheel: `data` is the signed delta in the low 32 bits.

Touch semantics: the tablet sends `flags = touch|left|down` while the finger is
in contact and `flags = touch|left` on release. The sender injects
`MOUSEEVENTF_ABSOLUTE|VIRTUALDESK` moves plus button transitions, so multi-monitor
setups map correctly.

## Session state machine (sink side)

```
Idle --Hello--> Streaming --Bye/EOF/error--> Closed
  |                  |
  +--bad version--> Closed (HelloAck: flags=0)
```

`SinkEngine` returns to `Idle` and waits for the next connection; a single sink
serves one sender at a time.

## Deliberate non-goals in v1

- No encryption/authentication: the protocol assumes a trusted home LAN. Run it
  on a network you control; see `docs/DEPLOY-RT.md` for the firewall rules and
  the (recommended) WPA2/WPA3 setup.
- No audio. Miracast carries LPCM/AC-3 over RTP; v1 sends video only. The RT's
  speakers can play local audio, and the PC keeps its own output.
- No HDCP, therefore no protected content (see `FEASIBILITY.md` section 5).
