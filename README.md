# Gemstone Camera

An ESP32-S3 + OV2640 camera that learns to recognise gemstones. Show it each stone a few
times, then it names whatever you put under the lens. Everything runs on the ESP32. It makes
its own WiFi hotspot and serves a web page, so a phone or laptop is all you need to use it.

**Status: alpha.** Recognition currently uses *classic computer vision*: it measures colour,
colour variation and a hue histogram inside a box in the middle of the picture, then finds
the most similar stones you've taught it (k-nearest neighbours). A trained neural network is
the next step. This version then becomes the "colour-only baseline" to compare against.

---

## Hardware

- ESP32-S3 dev board with 8 MB PSRAM (developed on an **N16R8**)
- OV2640 camera on a 2x9-pin adapter board (ALIENTEK-style, with its own clock oscillator)
- 18 female-to-female jumper wires, **10 cm or shorter**

### Wiring

Laid out in the same order as the camera board's two header rows. The camera board has its
own clock, so there is no XCLK wire.

| Top row | ESP32-S3 | | Bottom row | ESP32-S3 |
|---|---|---|---|---|
| GND  | GND     | | 3.3V  | 3V3 (**not** 5V) |
| SCL  | GPIO 5  | | VSYNC | GPIO 6  |
| SDA  | GPIO 4  | | HREF  | GPIO 7  |
| D0   | GPIO 11 | | RST   | 3V3     |
| D2   | GPIO 8  | | D1    | GPIO 9  |
| D4   | GPIO 12 | | D3    | GPIO 10 |
| D6   | GPIO 17 | | D5    | GPIO 18 |
| PCLK | GPIO 13 | | D7    | GPIO 16 |
| PWDN | GND     | | FLASH | not connected |

The numbers printed next to the ESP32's pins are GPIO numbers, but they aren't in order
along the header, so double-check each wire. This is the ESP32-S3-EYE pin map, so stock
Arduino camera examples (`CAMERA_MODEL_ESP32S3_EYE`) also work with this wiring.
For a camera board that *does* have an XCLK pin, wire it to GPIO 15 and set
`Camera XCLK pin` to 15 in the settings menu.

---

## Setting up a Windows laptop (once)

1. **Install Git** from <https://git-scm.com/download/win> (default options are fine).
2. **Install ESP-IDF 5.2** with Espressif's Windows installer:
   <https://dl.espressif.com/dl/esp-idf/>. Choose a **v5.2.x** release (not 5.3+ or 6.x)
   and keep the default install folder (`C:\Espressif`). It brings its own Python and
   compilers, so nothing else is needed. The download is large, about 1-2 GB.
3. **Get the code.** Open PowerShell where you want the project folder and run:
   ```powershell
   git clone https://github.com/Gr3nSkyDragon/gemstone_camera.git
   cd gemstone_camera
   ```
4. **Allow the build script to run.** Windows blocks scripts by default. This is a
   one-time change that applies to your user account only:
   ```powershell
   Set-ExecutionPolicy -Scope CurrentUser RemoteSigned
   ```

Use **PowerShell**, not Git Bash; ESP-IDF refuses to run under Git Bash.

## Building and flashing

Plug the board in with a USB-C cable. Either USB port works. Then, from the project folder:

```powershell
.\build.ps1 -Flash auto
```

This builds the firmware, finds the board, flashes it, and opens the serial monitor so you
can watch it start up. Press **Ctrl+]** to close the monitor.

- The **first build takes several minutes** and needs internet; it downloads the camera
  driver. After that, rebuilds take well under a minute.
- `.\build.ps1` on its own just builds. `-Flash COM8` picks a port yourself.
  `-NoMonitor` skips the serial monitor.
- If flashing can't connect, hold the board's **BOOT** button, tap **RST**, release
  **BOOT**, and run the command again.

A healthy start-up log includes:

```
camera: Detected OV2640 camera
camera: first frame: 640x480, ... bytes
main: hotspot "GemCam" (password "gemstone1") -> join it, then open http://192.168.4.1/
```

If the camera fails, the log (and the web page) say which wires to check.

## Using it

1. On your phone or laptop, join the WiFi network **GemCam** (password `gemstone1`). Your
   device may warn there's no internet; stay connected anyway.
2. Open **<http://192.168.4.1/>** in a browser. You'll see a live preview.

The web page lives inside the firmware on the ESP32. Nothing is loaded from the internet.

### Before teaching: lighting and camera settings

The matching is all about colour, so consistency matters more than anything else.

- Use the same light every time. A small box lit by white LEDs is ideal; avoid daylight
  that changes through the day.
- Keep the camera at a fixed height, with the stone filling the dashed box.
- The lens comes focused far away. For close-ups, gently twist the lens to refocus. It's
  usually held by a dab of glue that needs cracking first.
- Under **Camera settings**, once the picture looks right, turn off **Auto white balance**
  (pick a preset instead), then turn off **Auto exposure** and **Auto gain** and set them by
  hand. Otherwise the camera "corrects" every stone towards grey. Settings are remembered
  after a reboot.

### Teaching and identifying

1. Type a stone's name and press **Teach** 5-10 times, turning the stone a little between
   presses. Each press averages 3 frames.
2. Repeat for every stone. Also teach **`empty`**, with nothing in the box, so it doesn't
   guess a stone when there isn't one.
3. Put a stone under the camera and press **Identify**. It shows the best match, how sure
   it is, and its second guess.
4. You can also press the board's **BOOT** button to identify. The result is printed on the
   serial monitor, so this works without a phone.

Taught stones are stored on the ESP32 as 16 numbers each, not as photos. They survive
reboots and re-flashing. Remove one with the **x** on its tag, or all with
**Forget everything**.

### Using your home WiFi instead of the hotspot

```powershell
.\build.ps1 -Menuconfig
```

Go to **Gemstone Cam** and fill in your network name and password. Press **S** to save and
**Q** to quit, then flash again. The serial log prints the board's address (for example
`http://192.168.1.42/`). If your network can't be found, at the fair for instance, it falls
back to the GemCam hotspot by itself.

Your WiFi password is stored in `sdkconfig`, which is **not** uploaded to GitHub
(see `.gitignore`).

---

## Collecting photos for training a model

The ESP32 doesn't store photos. You download them to whatever device is viewing the page.

- **One at a time:** the **Save photo** button downloads the current frame, named after
  whatever is in the stone-name box (`amethyst_001.jpg`, ...).
- **In bulk, from a laptop:** with the laptop on the same network as the board, run:

  ```powershell
  python tools\collect.py amethyst -n 60
  ```

  This saves 60 photos into `dataset\amethyst\` while you slowly turn the stone. Run it
  once per stone. It needs Python 3 from <https://www.python.org/downloads/> (tick "Add
  python.exe to PATH" when installing), with no extra packages. Use `--host 192.168.1.42`
  if the board is on your home WiFi, and `-i 1.5` to slow it down. Re-running the same
  stone adds to the folder rather than overwriting it.

The `dataset` folder is laid out the way Edge Impulse expects for image classification:
one folder per label.

---

## Troubleshooting

| Problem | Fix |
|---|---|
| `Camera not answering on SDA/SCL` | Check SDA→4, SCL→5, 3.3V, GND, PWDN→GND, RST→3V3. A swapped SDA/SCL pair is the usual culprit. |
| `Sensor found but no frames arrive` | The 4 control wires are fine; check VSYNC, HREF, PCLK and D0-D7. |
| Striped, glitchy or half-grey pictures | Shorten or reseat the jumpers. If that doesn't help, run `.\build.ps1 -Menuconfig` and set **Sensor clock divider** to 1 or 2. |
| Page won't load | Make sure you're on the GemCam WiFi and the address is `http://`, not `https://`. Phones sometimes drop "no internet" networks, so rejoin. |
| `running scripts is disabled` | Run the `Set-ExecutionPolicy` line from the setup section. |
| `ESP-IDF not found` | Install ESP-IDF 5.2, or point to it: `.\build.ps1 -Idf D:\Espressif`. |
| `MSys/Mingw is not supported` | You're in Git Bash. Use PowerShell. |
| Identifies everything as the same stone | Teach more samples in the same lighting, turn off auto white balance and exposure, and teach `empty`. |

---

## How it's organised

| File | What it does |
|---|---|
| `main/camera.c` | Camera start-up, pin map, sensor settings (saved to flash), wiring diagnosis |
| `main/gem.c` | Colour/texture features, k-nearest-neighbour matching, sample storage |
| `main/web.c`, `main/index.html` | The web page and its HTTP API |
| `main/main.c` | WiFi (hotspot or home network), BOOT button, start-up |
| `main/Kconfig.projbuild` | Settings shown in `-Menuconfig` |
| `partitions.csv` | Flash layout: 3 MB app and a separate area for taught stones and settings |
| `tools/collect.py` | Photo collector for building a training set |
| `build.ps1` | Build / flash / settings helper for Windows |

### HTTP API

| Request | Result |
|---|---|
| `GET /capture` | Current camera frame (JPEG) |
| `GET /status` | Camera state, settings, taught labels (JSON) |
| `GET /control?var=awb&val=0` | Change a camera setting |
| `POST /identify` | Capture and identify (JSON: label, confidence, runner-up) |
| `POST /teach?label=amethyst` | Capture and store a sample |
| `POST /forget?label=amethyst` or `?all=1` | Delete samples |

## Licence

GPL-3.0. See [LICENSE](LICENSE).
