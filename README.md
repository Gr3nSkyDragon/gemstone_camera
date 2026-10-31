# Gemstone Camera

An ESP32-S3 + OV2640 camera that learns to recognise gemstones. Show it each stone a few
times, then it names whatever you put under the lens. Everything runs on the ESP32. It makes
its own WiFi hotspot and serves a web page, so a phone or laptop is all you need to use it.

**Status: alpha.** Recognition currently uses *classic computer vision*: it measures colour,
colour variation and a hue histogram inside a box in the middle of the picture, then finds
the most similar stones it has seen (nearest neighbours). It keeps two separate memories:

- **Trained model**: built from a photo dataset (dozens of photos per stone) on a laptop.
- **Live demo**: taught on the spot from the web page, a few presses per stone, and wiped
  whenever you like without touching the trained model.

A neural network is the next step; this version then becomes the "colour-only baseline".

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
| D0   | GPIO 11 | | RST   | GPIO 21 |
| D2   | GPIO 8  | | D1    | GPIO 9  |
| D4   | GPIO 12 | | D3    | GPIO 10 |
| D6   | GPIO 17 | | D5    | GPIO 18 |
| PCLK | GPIO 13 | | D7    | GPIO 16 |
| PWDN | GPIO 14 | | FLASH | not connected |

The numbers printed next to the ESP32's pins are GPIO numbers, but they aren't in order
along the header, so double-check each wire. This is the ESP32-S3-EYE pin map, so stock
Arduino camera examples (`CAMERA_MODEL_ESP32S3_EYE`) also work with this wiring.
For a camera board that *does* have an XCLK pin, wire it to GPIO 15 and set
`Camera XCLK pin` to 15 in the settings menu.

**PWDN and RST must go to GPIOs 14 and 21, not GND and 3V3.** The OV2640 sometimes powers
up in a stuck state, especially after the board has been off for a while. With PWDN and
RST on GPIOs, the ESP32 power-cycles and resets the camera at start-up, and again if frames
ever stop arriving, so it recovers by itself. Hard-wired to GND/3V3, only unplugging the
board for a while recovers it.

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

### Identifying

1. Pick which memory to use at the top of the Identify card: **Trained model** or
   **Live demo**. The choice is remembered after a reboot.
2. Put a stone under the camera and press **Identify**. It shows the best match and how
   sure it is. Open **All scores** for a bar graph of every stone's score.
3. You can also press the board's **BOOT** button. The page shows and speaks that result
   too, and it's printed on the serial monitor, so this also works without a phone.

**Speech:** with **Speak results** ticked, the phone says the answer out loud ("Amethyst",
"Probably amethyst", or "I'm not sure. Maybe amethyst", depending on how sure it is).
Pick a voice from the list; **Say it again** repeats the last answer. The voice comes from
the phone itself, so turn its volume up. Most phones have offline voices, but on Android
check *Settings → Text-to-speech* if it stays silent without internet.

### Live demo training

1. Switch to **Live demo**, type a stone's name and press **Teach** 5-10 times, turning the
   stone a little between presses. Each press averages 3 frames.
2. Repeat for every stone. Also teach **`empty`**, with nothing in the box, so it doesn't
   guess a stone when there isn't one.
3. Remove one stone with the **x** on its tag, or everything with **Wipe live demo**. The
   trained model is never affected, so you can wipe, re-teach in front of an audience, and
   switch back to **Trained model** at any time.

Stones are stored on the ESP32 as 16 numbers each, not as photos. Both memories survive
reboots and re-flashing.

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

## Building the trained model

Once `dataset\` has a folder of photos for each stone (include an `empty` folder), and the
laptop is on the board's network:

```powershell
python tools\train.py
```

Every photo is sent to the board, which measures it exactly the way it measures the live
camera, saves the results, and switches Identify to **Trained model**. It takes a minute or
two. It replaces the previous trained model. Useful options:

- `--max 60`: use at most 60 photos per stone, picked evenly. The board holds 1000 in total.
- `--add`: add to the trained model instead of replacing it.
- `--only amethyst empty`: use only these folders (the other trained stones are dropped).
- `--host 192.168.1.42`: the board is on your home WiFi.

Take the photos in the same lighting and setup you'll use at the fair, because the trained
model is only as good as how closely its photos match what the camera sees on the day.

---

## Troubleshooting

| Problem | Fix |
|---|---|
| Camera fails after the board was off for a while | Make sure PWDN→GPIO 14 and RST→GPIO 21 (not GND/3V3). Then press **Restart camera** on the page, or the board's RST button. |
| `Camera not answering on SDA/SCL` | Check SDA→4, SCL→5, 3.3V, GND, PWDN→14, RST→21. A swapped SDA/SCL pair is the usual culprit. |
| `Sensor found but no frames arrive` | The 4 control wires are fine; check VSYNC, HREF, PCLK and D0-D7. |
| Striped, glitchy or half-grey pictures | Shorten or reseat the jumpers. If that doesn't help, run `.\build.ps1 -Menuconfig` and set **Sensor clock divider** to 1 or 2. |
| Page won't load | Make sure you're on the GemCam WiFi and the address is `http://`, not `https://`. Phones sometimes drop "no internet" networks, so rejoin. |
| `running scripts is disabled` | Run the `Set-ExecutionPolicy` line from the setup section. |
| `ESP-IDF not found` | Install ESP-IDF 5.2, or point to it: `.\build.ps1 -Idf D:\Espressif`. |
| `MSys/Mingw is not supported` | You're in Git Bash. Use PowerShell. |
| Identifies everything as the same stone | Teach more samples in the same lighting, turn off auto white balance and exposure, and teach `empty`. Check the right memory (Trained / Live) is selected. |
| No speech | Tick **Speak results**, turn the phone's media volume up, and try another voice. |

---

## How it's organised

| File | What it does |
|---|---|
| `main/camera.c` | Camera start-up and hard reset, pin map, sensor settings (saved to flash), wiring diagnosis |
| `main/gem.c` | Colour/texture features, nearest-neighbour scoring, the trained and live sample memories |
| `main/web.c`, `main/index.html` | The web page and its HTTP API |
| `main/main.c` | WiFi (hotspot or home network), BOOT button, start-up |
| `main/Kconfig.projbuild` | Settings shown in `-Menuconfig` |
| `partitions.csv` | Flash layout: 3 MB app and a separate area for taught stones and settings |
| `tools/collect.py` | Photo collector for building a training set |
| `tools/train.py` | Uploads the photo dataset to build the trained model |
| `build.ps1` | Build / flash / settings helper for Windows |

### HTTP API

| Request | Result |
|---|---|
| `GET /capture` | Current camera frame (JPEG); header `X-Result-Seq` counts results |
| `GET /status` | Camera state, settings, mode, both memories' labels (JSON) |
| `GET /control?var=awb&val=0` | Change a camera setting |
| `POST /camera/restart` | Power-cycle and restart the camera |
| `POST /identify` | Capture and identify with the current mode (JSON: label, confidence, every stone's score) |
| `GET /result` | The most recent result, including ones from the BOOT button |
| `POST /mode?set=trained` or `live` | Choose which memory Identify uses |
| `POST /teach?label=amethyst` | Capture and store a Live demo sample |
| `POST /forget?label=amethyst` or `?all=1` | Delete Live demo samples |
| `POST /train/clear`, `/train/add?label=x` (JPEG body), `/train/commit` | Rebuild the trained model |

## Licence

GPL-3.0. See [LICENSE](LICENSE).
