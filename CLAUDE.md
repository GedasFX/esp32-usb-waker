# esp32-usb-waker — working brief

ESP-IDF firmware for an ESP32-S3 that plugs into a PC as a USB keyboard and wakes it from
sleep over HTTP (USB remote wakeup). Endpoints, build and flash: `README.md`.

Keep this file current. No changelog — git log is the history.

## Where things live

- `main/main.c`: all of it: USB HID, Wi-Fi, the HTTP endpoints.
- `main/secrets.h`: Wi-Fi and the wake token, git-ignored; `secrets.h.example` is the template.
- `CMakePresets.json`: presets `default` and `dev`; `sdkconfig.dev` adds `DEV_MODE` and two
  OTA slots on top of `sdkconfig.defaults`.

## How to run and test

With ESP-IDF v6.1 activated: `idf.py --preset default build`, and `--preset dev` for the
`DEV_MODE` build. No automated tests: a change is checked on a board and a sleeping host.

## Invariants

- No auto light sleep: the S3's USB can't wake the chip, so it slept through the host's
  enumeration after a restart and Windows gave up on the device until a replug.
- The System Sleep key is held 500 ms: Windows ignores 50 ms. `/sleep` presses it again if
  the host hasn't suspended 5 s later: once its display has turned off, the first press
  doesn't sleep it (the cause is unproven: an F24 tap first didn't help).
- Every endpoint checks the token, `/status` too: nothing on the network learns whether the
  host sleeps.
- OTA and the version endpoint exist only in `DEV_MODE`: a deployed board has no update path.
- The target is pinned in `sdkconfig.defaults`: without it a fresh build picks esp32 and the
  TinyUSB component fails to resolve.

## Traps

- Once the firmware runs, the board has no serial port (the one USB PHY is the keyboard):
  reflash by holding BOOT while plugging in.
- `app_update` stays in `PRIV_REQUIRES` in both builds: ESP-IDF can't make a component
  requirement depend on a Kconfig option.

## Open items

1. The keyboard collection sends nothing (only System Control is used). Drop it, then check
   on the host that wake is still armed (`powercfg /devicequery wake_armed`) and run one
   sleep–wake round. If wake stops being armed, keep it with a one-line comment saying why.
