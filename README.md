# esp32-usb-waker

Wakes a sleeping PC over Wi-Fi when Wake-on-LAN can't. An ESP32-S3 plugs into the PC as a
USB keyboard and runs a small HTTP server; `POST /wake` sends USB remote wakeup, which the PC
treats like a key press. A USB port stays powered in sleep, and Windows arms USB keyboards
for wake.

Built and tested on a Seeed XIAO ESP32-S3 with ESP-IDF v6.1, waking a Windows laptop from S3
sleep: USB is awake ~3 s after `/wake`, SSH answers ~1.5 s later.

## Endpoints

| Endpoint | Does |
|---|---|
| `GET /status` | `usb`: `awake`, `suspended` or `unmounted`; `remoteWakeup`: whether the host allows a wake |
| `POST /wake` | `Authorization: Bearer <token>`; USB remote wakeup, only while suspended and allowed |
| `POST /sleep` | same token; holds the HID System Sleep key for 500 ms |
| `POST /debug/update` | `DEV_MODE` only; same token; the app image as the body; writes it to the free OTA slot and restarts |
| `GET /debug/version` | `DEV_MODE` only; the running app's version |

The board sends `quest-waker` as its DHCP hostname; a router that puts DHCP names in its DNS
resolves it, so clients don't depend on the lease's address.

## Build and flash

1. Copy `main/secrets.h.example` to `main/secrets.h` and fill in the Wi-Fi (2.4 GHz) and a
   long random token. `secrets.h` is git-ignored.
2. Build with ESP-IDF v6.1:

   ```sh
   idf.py --preset default build
   idf.py --preset default merge-bin    # build/default/merged-binary.bin
   ```

3. Flash `merged-binary.bin` at `0x0`, with `idf.py -p <port> flash` or
   [esptool-js](https://espressif.github.io/esptool-js/) in Chrome. Once the firmware runs,
   the S3's only USB port is the keyboard, so there is no serial port: to flash again, hold
   the BOOT button (B on the XIAO) while plugging the board in.

| Preset | Builds into | Has |
|---|---|---|
| `default` | `build/default/` | one 1 MB app slot, no OTA |
| `dev` | `build/dev/` | `DEV_MODE` (`sdkconfig.dev`): the `/debug/` endpoints, two 1.7 MB OTA slots |

Switching presets changes the partition table, so it takes a cable flash. In `DEV_MODE`,
update over Wi-Fi with:

```sh
curl -H "Authorization: Bearer <token>" --data-binary @build/dev/quest-waker.bin http://<board>/debug/update
```

## Example: wake before SSH

On the machine you connect from, an `~/.ssh/config` block runs a script before `ssh laptop`:

```
Match originalhost laptop exec "powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:/Users/me/.ssh/wake-laptop.ps1"
```

The script wakes the laptop if its SSH port doesn't answer, waits for it, and always exits 0
so `ssh` goes on either way:

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

- **Windows ignores a short sleep key.** A 50 ms press does nothing; 500 ms sleeps the PC.
  With the display off, the first press only turns it on: call `/sleep` twice.
- **No auto light sleep.** The S3's USB can't wake the chip from light sleep. With it on, the
  board slept through the PC's enumeration after a restart, Windows gave up on it ("Device
  Descriptor Request Failed"), and it stayed unusable until replugged. It runs at 80 MHz with
  Wi-Fi max modem sleep instead; both still wake the PC every time.
- **The board can only wake a PC that suspended it.** Plugged in while the PC sleeps, or
  with the PC off or hibernating, it stays `unmounted` and `/wake` returns 409.
- **The token is compiled into the firmware.** Anyone holding the board can read it.

## License

MIT
