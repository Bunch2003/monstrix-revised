# Monstrix Revised

Two little desk robots, **Monstrix** and **Nova**, with animated OLED eyes. Both run
one shared firmware on an ESP32-C3. This project grew out of the
[MONSTRIX robot](https://github.com/Belyiiii/monstrix-cod) by Igor Fedorov. It started
from his firmware and extends it a lot. See [CREDITS.md](CREDITS.md).

## What's new compared to the original

**Personality**
- A new animation engine with 40+ animations: wink, side-eye, double-take, sneeze,
  laugh, heart eyes, dizzy, fake sleep and more. Each one can play randomly, only at
  night, only while gaming, or only on request.
- Screen effects: text pop-ups, bursts, scans, steam, fireworks, spinning and a
  trophy pedestal.
- Sleepy and snoring animations with floating Z's.

**Day and night**
- Gets the time over Wi-Fi and works out sunset and sunrise for your location.
- Sleepy animations only happen at night. The screen dims late in the evening unless
  you're gaming.

**Web remote**
- Phone-friendly page at `http://<robot>.local` (can be added to the home screen).
- Live mirror of the robot's screen, buttons to play any animation, and an
  Auto/Day/Night switch.

**PlayStation gaming mode (optional)**
- Notices when your PlayStation account is online and switches to a gaming mood.
- Celebrates new trophies by tier (bronze, silver, gold, platinum).
- Says "GG" when the console goes offline. After long late-night sessions it gets a
  "tired gamer" look.
- Has a special hero-themed set of animations for when a particular game is running.
- You sign in through the web page. The sign-in token is stored on the robot itself,
  never in the code.

**Two robots, one firmware**
- Each board remembers whether it's Monstrix or Nova (name, web address, screen
  rotation), so the same file can be sent to both.
- Wireless (OTA) updates: upload from Arduino IDE over Wi-Fi, or through a
  password-protected `/update` page.
- Wi-Fi automatically reconnects, and the web page and `.local` address come back
  after a dropout.

**Nova shells**
- `hardware/` has a dragon shell (single color, plus a black and silver multi-color
  AMS version) and a cat shell. Both are remixes of the MONSTRIX shell with a sealed
  front.

## Hardware
- ESP32-C3 SuperMini
- 0.96" SSD1306 OLED (I2C address 0x3C): SDA → GPIO8, SCL → GPIO9, VCC → 3V3, GND → GND
- Optional MPU6050 gyro on the same I2C pins (set `USE_GYRO 1`)
- 3D-printed shell: the original MONSTRIX shell, or a Nova shell from `hardware/`

## Setup
1. Install Arduino IDE 2.x and the **esp32** boards package. Use these settings:
   - Board: *ESP32C3 Dev Module*
   - USB CDC On Boot: *Enabled*
   - Partition Scheme: *Minimal SPIFFS (1.9MB APP with OTA)*
2. In Library Manager, install **FluxGarage_RoboEyes**, **Adafruit SSD1306**,
   **Adafruit GFX** and **ArduinoJson** (v7).
3. In `firmware/monstrix_animations/`, copy `secrets.example.h` to `secrets.h` and
   fill in your Wi-Fi, location and update password. `secrets.h` is git-ignored, so
   it never gets committed.
4. For the first upload, use USB and set `WHICH_ROBOT` to 1 or 2. After that, upload
   over Wi-Fi: **Tools → Port → Network ports → monstrix / nova**.
5. Open the web remote at `http://monstrix.local` or `http://nova.local`.

Serial Monitor (115200 baud): type `?` to list keys that play each animation.

## License
- Firmware: **CC BY-NC 4.0** (non-commercial), as an adaptation of the original
  MONSTRIX firmware.
- 3D models: **CC BY-SA 4.0**, as adaptations of the MONSTRIX and LDR Little Robot
  shells.
- RoboEyes (GPL-3.0) is not included. Install it separately.

Unofficial fan project, not affiliated with Sony or Marvel. Details in
[LICENSE](LICENSE) and [CREDITS.md](CREDITS.md).
