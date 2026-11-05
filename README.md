# Gemstone Camera

An ESP32-S3 + OV2640 camera that recognizes gemstones and names whatever you put under the
lens. Everything runs on the ESP32. It makes its own WiFi hotspot and serves a web page, so
a phone or laptop is all you need to use it.

It identifies stones two ways, switched with the buttons under the camera preview:

- **Trained model**: a neural network trained online with [Edge Impulse](https://edgeimpulse.com)
  from a photo dataset, then built into the firmware. It runs on the ESP32 itself in about
  0.2 seconds, with no internet. Current model (v3): aventurine, citrine, jasper,
  lapis lazuli, quartz.
- **Live demo**: *classic computer vision*. It measures color, color variation and a hue
  histogram in the middle of the picture, and matches them against stones taught on the spot
  from the web page, a few presses each. Wipe and re-teach it whenever you like; the trained
  model is never touched.

Comparing the two on the same stones is the science-fair experiment: what does a neural
network get right that color alone can't?

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

The matching is all about color, so consistency matters more than anything else.

- Use the same light every time. A small box lit by white LEDs is ideal; avoid daylight
  that changes through the day.
- Keep the camera at a fixed height, with the stone filling the dashed box. The box shows
  what the current mode looks at: the center square for the trained model, the middle half
  for the live demo.
- The lens comes focused far away. For close-ups, gently twist the lens to refocus. It's
  usually held by a dab of glue that needs cracking first.
- Under **Camera settings**, once the picture looks right, turn off **Auto white balance**
  (pick a preset instead), then turn off **Auto exposure** and **Auto gain** and set them by
  hand. Otherwise the camera "corrects" every stone toward gray. Settings are remembered
  after a reboot.

### Identifying

The camera preview, the mode buttons and **Identify** stay at the top of the screen on a
phone (or in the left column on a laptop), so the stone stays in view while you use them.

1. Pick **Trained model** or **Live demo** under the preview. The choice is remembered after
   a reboot.
2. Put a stone in the dashed box and press **Identify** (purple). It shows the best match and
   how sure it is. Open **All scores** for a bar graph of every stone's score.
3. You can also press the board's **BOOT** button. The page shows and speaks that result
   too, and it's printed on the serial monitor, so this also works without a phone.

**Speech:** with **Speak results** ticked, the phone says the answer out loud ("Amethyst",
"Probably amethyst", or "I'm not sure. Maybe amethyst", depending on how sure it is).
Pick a voice from the list; **Say it again** repeats the last answer. The voice comes from
the phone itself, so turn its volume up. Most phones have offline voices, but on Android
check *Settings → Text-to-speech* if it stays silent without internet.

### Live demo training

1. Switch to **Live demo**. A name box and a green **Teach** button appear above Identify.
   Type a stone's name and press **Teach** 5-10 times, turning the stone a little between
   presses. Each press averages 3 frames.
2. Repeat for every stone.
3. Remove one stone with the **x** on its tag, or everything with **Wipe live demo**. The
   trained model is never affected, so you can wipe, re-teach in front of an audience, and
   switch back to **Trained model** at any time.

Taught stones are stored on the ESP32 as 16 numbers each, not as photos. They survive
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

## Checking the model on the board

`tools/classify.py` uploads saved photos to the board, which runs each one through exactly
the same decode, crop, resize and model as a live camera frame. If the board gets the same
answers as Edge Impulse Studio for the same photos, the firmware is working and any
problem is in what the camera sees.

```powershell
python tools\classify.py dataset\jasper\jasper_0012.jpg   # one photo, every score
python tools\classify.py dataset -n 20                     # 20 per stone, plus a confusion matrix
python tools\classify.py dataset -n 5 --save-input seen    # also save what the model saw
```

Each folder's name is taken as the right answer (`lapis_lazuli` matches `lapis lazuli`).
`--save-input` writes the 96x96 image the model actually received as a `.bmp`, so you can
check its colors and cropping by eye. The same thing works without Python:
`curl --data-binary @photo.jpg http://192.168.4.1/classify` returns the scores, and
adding `?view=input` to the URL returns the image.

## Training and updating the AI model

The trained model is trained in Edge Impulse Studio (free; it runs in the cloud, so the
laptop only needs a browser) and built into the firmware.

1. **Collect photos** with `collect.py` as above: about 50-100 per stone, in the same
   lighting and setup you'll use at the fair.
2. **Train** in Edge Impulse Studio: upload each folder under *Data acquisition* with the
   stone's name as its label, then create an impulse with **96x96** images, an *Image*
   block set to **RGB**, and *Transfer learning*. Train it and check the accuracy.
3. **Export**: on *Deployment*, choose **C++ library**, keep the **Quantized (int8)** model
   and the **EON Compiler** on, and build. You get a zip.
4. **Put it in the firmware**: delete `components\ei_model\model\` and unzip the new export
   in its place, so `components\ei_model\model\` holds `edge-impulse-sdk`,
   `model-parameters` and `tflite-model`. Then flash:

   ```powershell
   .\build.ps1 -Flash auto
   ```

   The start-up log shows the model's name, version and stones, then a test run
   (`AI model test 2: sees 'citrine' (98%) in 400 ms`). The web page picks up the new stone
   names by itself.

   The model always names one of its stones, so it assumes there's a stone in the box.

Stone names come from your Edge Impulse labels. Keep them short; spaces are fine
("lapis lazuli").

---

## Troubleshooting

| Problem | Fix |
|---|---|
| Camera fails after the board was off for a while | Make sure PWDN→GPIO 14 and RST→GPIO 21 (not GND/3V3). Then press **Restart camera** on the page, or the board's RST button. |
| `Camera not answering on SDA/SCL` | Check SDA→4, SCL→5, 3.3V, GND, PWDN→14, RST→21. A swapped SDA/SCL pair is the usual culprit. |
| `Sensor found but no frames arrive` | The 4 control wires are fine; check VSYNC, HREF, PCLK and D0-D7. |
| Striped, glitchy or half-gray pictures | Shorten or reseat the jumpers. If that doesn't help, run `.\build.ps1 -Menuconfig` and set **Sensor clock divider** to 1 or 2. |
| Page won't load | Make sure you're on the GemCam WiFi and the address is `http://`, not `https://`. Phones sometimes drop "no internet" networks, so rejoin. |
| `running scripts is disabled` | Run the `Set-ExecutionPolicy` line from the setup section. |
| `ESP-IDF not found` | Install ESP-IDF 5.2, or point to it: `.\build.ps1 -Idf D:\Espressif`. |
| `MSys/Mingw is not supported` | You're in Git Bash. Use PowerShell. |
| Identifies everything as the same stone | Check the right mode (Trained / Live) is selected. Live demo: teach more samples in the same lighting, and turn off auto white balance and exposure. Trained model: retrain with photos taken in your fair setup. |
| Build fails with `internal compiler error` in the Edge Impulse code | The model component must be built with `-Os`; keep `components/ei_model/CMakeLists.txt` as it is when replacing the model. |
| No speech | Tick **Speak results**, turn the phone's media volume up, and try another voice. |

---

## How it's organized

| File | What it does |
|---|---|
| `main/camera.c` | Camera start-up and hard reset, pin map, sensor settings (saved to flash), wiring diagnosis |
| `main/gem.c` | Both modes: runs the AI model, and the color/texture features and nearest-neighbor scoring of the live demo |
| `main/web.c`, `main/index.html` | The web page and its HTTP API |
| `main/main.c` | WiFi (hotspot or home network), BOOT button, start-up |
| `main/Kconfig.projbuild` | Settings shown in `-Menuconfig` |
| `partitions.csv` | Flash layout: 3 MB app and a separate area for taught stones and settings |
| `components/ei_model/` | The Edge Impulse model (`model/`, as exported) and a small C wrapper (`gem_ai.cpp`) |
| `tools/collect.py` | Photo collector for building a training set |
| `build.ps1` | Build / flash / settings helper for Windows |

### HTTP API

| Request | Result |
|---|---|
| `GET /capture` | Current camera frame (JPEG); header `X-Result-Seq` counts results |
| `GET /status` | Camera state, settings, mode, the AI model's stones, the live demo's stones (JSON) |
| `GET /control?var=awb&val=0` | Change a camera setting |
| `POST /camera/restart` | Power-cycle and restart the camera |
| `POST /identify` | Capture and identify with the current mode (JSON: label, confidence, every stone's score) |
| `GET /result` | The most recent result, including ones from the BOOT button |
| `POST /mode?set=trained` or `live` | Choose the trained AI model or the live demo |
| `POST /teach?label=amethyst` | Capture and store a Live demo sample |
| `POST /forget?label=amethyst` or `?all=1` | Delete Live demo samples |

## License

GPL-3.0. See [LICENSE](LICENSE). The Edge Impulse SDK in `components/ei_model/model/` has its
own licenses (Apache 2.0 and BSD), included in that folder.
