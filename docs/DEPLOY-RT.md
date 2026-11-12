# Installing and running on the tablet

## 1. Install

From an elevated PowerShell on the (jailbroken) Surface RT:

```
.\install\Install-WiDiService.ps1 -SourceDir C:\widi\build\winrt8
```

What it does:

1. copies `widi_sink.exe` and `widi_service.exe` to `%ProgramData%\SurfaceRT-WiDi`,
2. writes `widi.ini` with a default configuration (only if it does not exist),
3. adds firewall rules for TCP 47000 and UDP 47001,
4. registers `SurfaceRTWiDi` as an auto-start service running as LocalSystem,
   with restart-on-crash actions (5 s, then 15 s),
5. starts the service, which immediately launches the receiver UI.

Uninstall (also stops the service, deletes the rules and the folder):

```
.\install\Install-WiDiService.ps1 -Uninstall
```

## 2. `widi.ini` reference

`%ProgramData%\SurfaceRT-WiDi\widi.ini`

```ini
[sink]
Port=47000           ; TCP port the PC connects to
DiscoveryPort=47001   ; UDP port used for beacons
Name=Surface RT       ; name shown to the PC
ForwardInput=1        ; touch/mouse on the tablet controls the PC cursor
BroadcastBeacons=1    ; 0 = only manual IP connections
Windowed=0            ; 1 = run in a 1024x640 window (debugging)
```

Restart the service after editing (`sc stop SurfaceRTWiDi` / `sc start
SurfaceRTWiDi`).

## 3. Daily use

- After boot, **log in**. A service cannot draw in session 0, so the receiver
  window appears only once a user session exists. If you want the tablet to act
  as an always-on second screen, either leave it logged in (lock is fine -
  the service relaunches on unlock) or enable autologon.
- The receiver shows a HUD in the top-left corner: `waiting for PC - <ip> : 47000`
  before a connection, then `streaming - 20 fps - 24 ms - q55`.
- The screen is kept awake while the receiver runs
  (`SetThreadExecutionState`), and the mouse cursor is hidden while streaming.
- **To quit the UI:** tap the bottom-right 72x72 corner three times within three
  seconds (only possible while no PC is connected), or press Esc with a keyboard
  attached. The service will restart the UI unless the service is stopped.
- Real-time logs: `%ProgramData%\SurfaceRT-WiDi\widi.log` and
  `widi-service.log`.

## 4. Connecting from Windows 10/11

```
.\tools\pc\Send-Screen.ps1                 # auto-discovers the tablet
.\tools\pc\Send-Screen.ps1 -SinkHost 192.168.1.50
.\tools\pc\Send-Screen.ps1 -List           # show what is on the LAN
```

Nothing is installed: the helper class is compiled into a temporary in-memory
assembly by `Add-Type` and disappears with the process. See `PC-SENDER.md`.

## 5. Network setup that actually performs well

| Setting | Recommendation |
|---|---|
| Band | 5 GHz if your router has it, even though the RT itself is 2.4 GHz - keeps the *PC* upload and neighbours off the tablet's channel |
| Channel width | 20 MHz on 2.4 GHz (40 MHz in a crowded band causes retries) |
| Tablet power plan | Balanced; the receiver already forces the display on |
| Firewall | inbound TCP 47000 and UDP 47001 on the tablet (installer does it) |
| Isolation | disable "AP/client isolation" on the router, otherwise the PC cannot reach the tablet |
| Distance | same room; 1366x768 JPEG needs roughly 10-16 Mbit/s of clean airtime |

## 6. Verifying on the device

```
sc query SurfaceRTWiDi                  # service state
type %ProgramData%\SurfaceRT-WiDi\widi.log     # receiver log
netstat -an | findstr 47000             # listener present
type %ProgramData%\SurfaceRT-WiDi\widi.ini     # effective config
```

| Symptom | Check |
|---|---|
| No window after boot | nobody logged in (session 0), or the service is stopped |
| Window appears then closes | `widi.log` - usually "port already in use" |
| PC cannot connect | tablet IP changed (DHCP), firewall rule missing, AP isolation on |
| Connects but stays black | sender clamp: check `Send-Screen.ps1` output; or the JPEG stream is corrupt (sink logs "frame decode failed") |
| Picture but no touch | `ForwardInput=1`, and use `-NoInput` on the PC only when you mean it |
| Stutters after a minute | 2.4 GHz congestion; lower `-MaxWidth` (e.g. 1024) or `-Fps 15` |

## 7. Autologon (optional, for an always-on second screen)

An always-on wall-panel style setup needs a session, so autologon is required.
Use the local-only variant, not a Microsoft account password stored in the
registry, and restrict physical access to the device:

```
netplwiz      # uncheck "Users must enter a user name and password"
```

Windows RT additionally offers Settings > Accounts > Sign-in options >
*Require sign-in: Never* for the lock screen.
