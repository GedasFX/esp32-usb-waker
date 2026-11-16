# esp32-usb-waker

Wake-on-LAN doesn't work on every PC. This firmware takes another route: an ESP32-S3 plugs
into the PC's USB port as a keyboard, and an HTTP request to the board over Wi-Fi wakes the
PC the way a key press would.

It relies on USB remote wakeup: before a PC sleeps, it tells each USB device whether it may
wake it, and Windows allows keyboards by default. The PC only has to keep its USB ports
powered while it sleeps.

Tested with a Seeed XIAO ESP32-S3 and ESP-IDF v6.1 on a Windows laptop asleep with its lid
closed: its USB was awake about 3 s after the request, and SSH answered about 1.3 s later.

## Usage

Every request needs the token from `secrets.h` in an `Authorization: Bearer <token>` header;
without it, the board answers 401.

```sh
curl -X POST -H "Authorization: Bearer <token>" http://<board>/wake
```

| Endpoint | Does |
|---|---|
| `GET /status` | `usb`: what the PC is doing (below); `remoteWakeup`: whether the PC lets the board wake it |
| `POST /wake` | 202 when it sends the wake, 200 if the PC is already awake, 409 if it can't |
| `POST /sleep` | presses the System Sleep key for 500 ms; answers within about 7 s; 409 if the PC isn't awake |

| `usb` | Means |
|---|---|
| `awake` | the PC is on and using the board |
| `suspended` | the PC is asleep; `/wake` works if `remoteWakeup` is `true` |
| `unmounted` | the PC is off or hibernating, or it never set the board up (see below) |

## Build and flash

1. Copy `main/secrets.h.example` to `main/secrets.h` and fill in your Wi-Fi and a long
   random token. The Wi-Fi must be 2.4 GHz and reachable from the machine that calls the
   board; a router's IoT network may be walled off from your LAN.
2. Build with ESP-IDF v6.1. `merge-bin` packs the bootloader, partition table and app into
   one file:

   ```sh
   idf.py --preset default build
   idf.py --preset default merge-bin    # build/default/merged-binary.bin
   ```

3. Erase the flash, then write `merged-binary.bin` at `0x0` with `esptool` or
   [esptool-js](https://espressif.github.io/esptool-js/) in Chrome; esptool-js suggests
   `0x1000`, the older ESP32's address. Replug the board to start the firmware.
4. Plug it into the PC while the PC is awake. The board asks the router for an address as
   `esp32-usb-waker`: if your router puts such names in its DNS, use
   `http://esp32-usb-waker`; if not, reserve the board's address in the router.
5. Check it: put the PC to sleep; `/status` should say `suspended` with `remoteWakeup`
   `true`.

To flash again, hold BOOT (B on the XIAO) while you plug the board in: the running
firmware's only USB port is the keyboard, not a serial port.

## Example: wake before SSH

If you reach the PC over SSH, OpenSSH can wake it first: on the machine you connect from,
this `~/.ssh/config` line runs a script before `ssh laptop`:

```
Match originalhost laptop exec "powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:/Users/me/.ssh/wake-laptop.ps1"
```

If the laptop's SSH port doesn't answer, the script calls `/wake` and waits up to 30 s for
it. It always exits 0, so `ssh` goes on either way. Set `$Ip` and `$Waker`, and put the
token in `$TokenFile`:

```powershell
param([string]$Ip = "<laptop>", [int]$Port = 22, [string]$Waker = "http://<board>",
      [string]$TokenFile = "$HOME\.ssh\waker.token", [int]$WaitSec = 30)

function Test-Up([int]$ms) {
    $client = New-Object System.Net.Sockets.TcpClient
    try { $client.BeginConnect($Ip, $Port, $null, $null).AsyncWaitHandle.WaitOne($ms) -and $client.Connected }
    catch { $false } finally { $client.Close() }
}

if (Test-Up 1000) { exit 0 }
$token = (Get-Content -Raw $TokenFile).Trim()
try { Invoke-WebRequest -UseBasicParsing -Method Post -Headers @{ Authorization = "Bearer $token" } "$Waker/wake" | Out-Null }
catch { [Console]::Error.WriteLine("the waker failed: $($_.Exception.Message)"); exit 0 }

$deadline = (Get-Date).AddSeconds($WaitSec)
while ((Get-Date) -lt $deadline -and -not (Test-Up 1500)) { Start-Sleep -Seconds 1 }
exit 0
```

## What to know

- **Plugged in during sleep, it can't wake the PC.** The PC sets up USB devices only while
  awake, so until its next wake `/status` says `unmounted` and `/wake` returns 409. Nor can
  it wake a PC that is off or hibernating.
- **Don't turn on auto light sleep** (the chip sleeping between tasks): the S3's USB can't
  wake the chip from it. The board then sleeps through the PC asking what it is after a
  restart; Windows gives up ("Device Descriptor Request Failed") until you replug the board.
  To save power, the firmware runs at 80 MHz and lets Wi-Fi sleep between the router's
  beacons instead; both still wake the PC every time. Power use wasn't measured.
- **The token is compiled into the firmware.** Anyone holding the board can read it, so
  don't reuse it.

## Debug mode

For working on the firmware. The `dev` preset builds with `DEV_MODE` (`sdkconfig.dev`): two
1.7 MB app slots instead of one, and two endpoints, so new builds go over Wi-Fi. The board
writes each into the free slot while it runs from the other.

| Endpoint | Does |
|---|---|
| `POST /debug/update` | writes the app image in the body to the free slot and restarts into it |
| `GET /debug/version` | the running app's version (`git describe` at build time) |

Anyone with the token can replace the firmware, so put the normal build back before you leave the board in use.

1. Build and cable-flash it once, as above, from `build/dev/`:

   ```sh
   idf.py --preset dev build
   idf.py --preset dev merge-bin    # build/dev/merged-binary.bin
   ```

2. After each change, build and send the app (not the merged image):

   ```sh
   idf.py --preset dev build
   curl -H "Authorization: Bearer <token>" --data-binary @build/dev/esp32-usb-waker.bin http://<board>/debug/update
   ```

3. When done, erase the flash and cable-flash the normal build: the partition tables
   differ.

## License

MIT
