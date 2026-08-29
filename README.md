# SmallTV-ultra Unraid Display

Custom firmware for the **GeekMagic SmallTV / SmallTV-Ultra** (ESP-12F, 1.54" 240×240 ST7789) that polls [UnraidClaw](https://github.com/emaspa/unraidclaw) on your Unraid server and draws live system stats.

Also builds for the ESP32-C2 knockoff (`smalltv_c2`) and SmallTV Pro (`smalltv_pro`).

The Ultra is an ESP-12F, not an ESP32 — only the Pro / C2 boards are ESP32.

## Arduino IDE

Open this file, not the repo root:

`SmallTV_Unraid/SmallTV_Unraid.ino`

Board: **Generic ESP8266 Module**. Crystal **26 MHz**, CPU **80 MHz**, flash frequency **40 MHz**, flash size **4MB (FS:2MB OTA:~1019KB)**.

Libraries: TFT_eSPI (Bodmer), ArduinoJson 7.x (Benoit Blanchon), WiFiManager (tzapu). Point TFT_eSPI at `User_Setups/Setup_SmallTV_Ultra.h` from `User_Setup_Select.h` — do not replace the whole `User_Setup.h`.

## What it shows

UnraidClaw’s `/api/system/metrics` returns **memory + CPU load averages**, not a busy-% or GPU gauge. The screen shows:

- hostname + LIVE / WAIT
- CPU load 1m / 5m / 15m (bar normalised to thread count from `/api/system/info`)
- memory used / total
- array state + capacity
- Docker running / total
- VMs running / total

GPU utilisation is **not in the UnraidClaw API** today. The key needs `info:read`, `array:read`, `docker:read`, and `vms:read`.

## Build (PlatformIO)

```bash
pip install platformio
pio run -e smalltv_ultra          # ESP-12F SmallTV / SmallTV-Ultra
# pio run -e smalltv_c2           # ESP32-C2 / ESP8684
# pio run -e smalltv_pro          # SmallTV Pro (classic ESP32)
```

Flashable image: `.pio/build/smalltv_ultra/firmware.bin`

In the IDE: open this folder, pick env `smalltv_ultra`, Build.

## Flash (SmallTV-Ultra)

Stock Ultra OTA often rejects a full custom image with **Not Enough Space** (factory LittleFS leaves ~512 KB for the app).

**Two-step OTA** (same path as [smalltv-mod](https://github.com/giovi321/smalltv-mod)):

1. On the stock device open `http://<device-ip>/update` and upload [smalltv-mod-loader.bin](https://github.com/giovi321/smalltv-mod/releases).
2. Join the `SmallTV-Loader` AP, open `http://192.168.4.1/update`, upload `firmware.bin` from this project.

**UART fallback** (case open, ESP-12F pads: TX, RX, GND, GPIO0 held low at reset):

```bash
esptool.py --chip esp8266 --port /dev/ttyUSB0 write_flash 0x0 \
  .pio/build/smalltv_ultra/firmware.bin
```

## First boot / Settings

Nothing is hard-coded. Unraid address and API key are entered on the device and stored in flash.

1. Join the open AP **SmallTV-Unraid**.
2. The portal asks for 2.4 GHz Wi-Fi **and** Unraid IP, port, HTTPS, and API key.
3. After join, the screen shows the device IP. Open that IP in a browser for the **Settings** page.
4. Change host / key / brightness there any time. **Change Wi-Fi** reopens the setup AP.

UnraidClaw speaks **HTTPS with a self-signed cert** on port **9876**. Leave skip-TLS-verify on or the ESP8266 handshake fails.

## Hardware (Ultra)

| Signal | GPIO |
|--------|------|
| SPI CLK | 14 |
| SPI MOSI | 13 |
| DC | 0 |
| RST | 2 |
| CS | tied low (`-1`) |
| Backlight | 5, active-low |

If colours look inverted, `TFT_INVERSION_ON` is already set. If red/blue are swapped, remove `-D TFT_RGB_ORDER=1` from `platformio.ini`.
