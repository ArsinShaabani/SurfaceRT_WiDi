# Roadmap and known limitations

## Delivered in v1.0

- Receiver stack for Windows RT 8.1 / ARM: engine, session, discovery, adaptive
  rate control, WIC decode, Direct2D presentation with GDI fallback.
- Windows service host with session-aware launching and crash recovery.
- Zero-install PC sender with live adaptation and touch-to-cursor remoting.
- 23 test cases / ~4,200 assertions over the portable core (codecs, framing,
  resync,
  bounded buffering, engine pacing, real sockets, adaptation policy,
  random-input fuzzing); clean under AddressSanitizer + UBSan.
- Cross-language wire validation (`tools/dev/wire_check.py`).

## Next steps, in the order they pay off

1. **H.264 via Media Foundation (Tegra 3 hardware decoder).** Biggest latency and
   bandwidth win: ~4-8 Mbit/s instead of 10-16, and decode drops from ~25 ms to
   under 10 ms. Needs: `H264VideoStreamSettings`, an MFT decoder fed with
   Annex-B access units, per-frame framing over the same message channel, plus
   explicit IDR-request handling on loss.
2. **Tile/dirty-region updates.** A 64x64 tile hash on the PC side; send only
   changed tiles as separate JPEGs (protocol already tolerates multiple frames).
   Cuts bandwidth 5-10x for typical desktop work at the cost of complexity.
3. **Audio (LPCM over the same TCP stream)** so the tablet can be used as a
   speaker too - independent of video framing.
4. **DTLS + PIN pairing**, mirroring what MS-MICE calls `SECURITY_HANDSHAKE` and
   `SECURITY_OPTIONS`, so the link is safe on shared networks.
5. **Extended desktop via a PC-side virtual display driver (IddCx).** This is the
   only way to get *extend* rather than *mirror*, and it breaks the "nothing
   installed" rule - hence optional, and documented separately.
6. **Wake-on-LAN / quick reconnect**: remember the last PC and reconnect
   automatically when the tablet starts.
7. **HUD polish**: connection quality meter, Wi-Fi signal from the miniport
   statistics, one-tap QR pairing.

## Known limitations (by design, documented honestly)

| Limitation | Reason |
|---|---|
| Not a Miracast sink; does not appear in `Win+K` | impossible on RT 8.1 (`FEASIBILITY.md`) |
| Mirror, not extend | extending needs a display driver on the PC |
| No HDCP, so protected video is black | no HDCP on Windows RT |
| 1366x768-ish quality ceiling | Tegra 3 JPEG decode throughput |
| Unencrypted v1 protocol | trusted-LAN assumption; DTLS on the roadmap |
| Requires a logged-in session on the tablet | Windows services cannot draw in session 0 |
| Requires the community jailbreak | Windows RT code integrity |
| No keyboard remoting yet | only pointer + wheel are implemented in v1 |
