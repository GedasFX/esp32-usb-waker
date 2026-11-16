# esp32-usb-waker

Wake a sleeping PC over Wi-Fi when Wake-on-LAN (waking it with a network packet) doesn't
work on it. An ESP32-S3 board plugs into the PC's USB port and acts as a keyboard. You send
it an HTTP request, and it wakes the PC the way a key press would.

It uses USB remote wakeup: when a PC goes to sleep, it pauses its USB devices and tells each
one whether it may wake the PC; a device that may can then signal "wake up". Windows allows
this for USB keyboards by default. The one thing the PC must do is keep the USB port powered
while it sleeps.

## Use it

Every request needs the token from `secrets.h` in an `Authorization: Bearer <token>` header;
without it, the board answers 401. That includes `/status`, so nobody else on your network
can tell when the PC sleeps.

```sh
curl -X POST -H "Authorization: Bearer <token>" http://<board>/wake
```

| Endpoint | Does |
|---|---|
| `GET /status` | `usb`: what the PC is doing (below); `remoteWakeup`: whether the PC lets the board wake it |
| `POST /wake` | wakes the PC: 202 when it sends the wake, 200 if the PC is already awake, 409 if it can't |
| `POST /sleep` | sleeps the PC with the System Sleep key (a standard keyboard key in HID, the USB standard for keyboards), held for 500 ms; answers within about 7 s; 409 if the PC isn't awake |

| `usb` | Means |
|---|---|
| `awake` | the PC is on and using the board |
| `suspended` | the PC is asleep; `/wake` works if `remoteWakeup` is `true` |
| `unmounted` | the PC is off or hibernating, or it never set the board up (see below) |

## Build and flash

1. Copy `main/secrets.h.example` to `main/secrets.h` (git-ignored) and fill in your Wi-Fi
   and a long random token. The Wi-Fi must be 2.4 GHz, and reachable from the machine that
   will call the board: a router's separate IoT network may be walled off from your LAN.
2. Build with ESP-IDF v6.1 (Espressif's SDK). `merge-bin` packs the bootloader, the
   partition table (the map of what goes where in flash) and the app into one file:

   ```sh
   idf.py --preset default build
   idf.py --preset default merge-bin    # build/default/merged-binary.bin
   ```

3. Erase the flash, then write `merged-binary.bin` at `0x0`, with `esptool` or
   [esptool-js](https://espressif.github.io/esptool-js/) in Chrome. The web page suggests
   `0x1000` (the older ESP32's address); change it. Replug the board to start the firmware.
4. Plug it into the PC while the PC is awake. The board names itself `esp32-usb-waker` when
   it asks the router for an address. If your router puts those names in its DNS,
   `http://esp32-usb-waker` keeps working when the address changes; if not, find the
   address in the router's client list and reserve it.
5. Check it: put the PC to sleep and call `/status`. It should say `suspended` with
   `remoteWakeup` `true`.

To flash again: once the firmware runs, the board's only USB port is the keyboard, so
there is no serial port to flash through. Hold the BOOT button (B on the XIAO) while you
plug the board in.

## Example: wake before SSH

If you reach the PC over SSH, the wake can run on its own. OpenSSH can run a command before
it connects: on the machine you connect from, this `~/.ssh/config` line runs a script
before `ssh laptop`:

```
Match originalhost laptop exec "powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:/Users/me/.ssh/wake-laptop.ps1"
```

The script does nothing if the laptop's SSH port answers. Otherwise it calls `/wake` and
waits up to 30 s for the port. It always exits 0, so `ssh` goes on either way. Set `$Ip`
and `$Waker`, and put the token in `$TokenFile`:

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
  awake, so until its next wake `/status` says `unmounted` and `/wake` returns 409. The same
  goes for a PC that is off or hibernating.
- **Don't turn on auto light sleep** (the chip pausing itself between jobs to save power).
  The S3's USB can't wake the chip from it. When the PC restarts and asks the board what it
  is, the board sleeps through it; Windows gives up ("Device Descriptor Request Failed"),
  and the board stays dead until you replug it. To save power, it runs at 80 MHz and lets
  Wi-Fi sleep between the router's beacons instead; both still wake the PC every time.
  Power use wasn't measured.
- **The token is compiled into the firmware.** Anyone holding the board can read it, so use
  a token you use nowhere else.

## Debug mode

For working on the firmware. The `dev` preset builds it with `DEV_MODE` (`sdkconfig.dev`):
two 1.7 MB app slots instead of one, and two endpoints, so a new build goes over Wi-Fi
(OTA, "over the air"). The board writes the new build into the free slot while it runs from
the other.

| Endpoint | Does |
|---|---|
| `POST /debug/update` | takes an app image as the body, writes it to the free slot and restarts into it |
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

3. When done, cable-flash the normal build with an erase: the two builds have different
   partition tables.

## License

MIT
