# Rally 2 Recomp

Native Android port of the 1998 Model 3 arcade rally game (MAME set `srally2`).

No emulator: the original PowerPC game code is translated **on your device** into native ARM code (arm64 or 32-bit ARM), and the arcade hardware (graphics, sound, inputs) is reimplemented natively. The result runs at full speed on ordinary phones and Android TV boxes.

**No game code or data is included.** You need your own `srally2.zip`.

## Download

Get the latest APK from [Releases](../../releases). Current status: **beta**.

## Requirements

- Android 8.0 or newer
- OpenGL ES 3.2
- Recommended: a mid-range or better phone from the last 3-4 years. Older and low-end devices work with lower resolution (the default *Auto* setting adapts by itself).
- Your own `srally2.zip` (MAME ROM set)

## Getting started

1. Install the APK (allow installs from unknown sources if asked).
2. Open **Rally Recomp** and choose your `srally2.zip`.
   - On Android TV the app has its own file browser (USB sticks and internal storage). Grant file access when asked.
3. The first start checks the zip and prepares the game. This takes a few seconds and only happens once.
4. Insert coin, press start, race.

## Controls

**Touch**: on-screen steering, gas, brake, gears and view buttons. They hide when a gamepad is in use and come back when you touch the screen.

**Gamepad**: any USB or Bluetooth controller, hot-plugged at any time.

| Control | Pad |
|---|---|
| Steering | left stick (d-pad left/right also works) |
| Gas / Brake | right / left trigger (d-pad up/down also works) |
| View | A |
| Handbrake | X or B |
| Shift up / down | RB / LB |
| Start / Coin | Start / Back |

To rebind, put a `pad.ini` in the app's data folder (`Android/data/org.srally2/files/`):

```
steer=leftx
gas=righttrigger
brake=lefttrigger
view=a
handbrake=x,b
shiftup=rightshoulder
shiftdown=leftshoulder
start=start
coin=back
deadzone=0.10
```

**Phone as controller**: great with Android TV. Open the settings menu, select **Phone as controller (QR)** and scan the code with your phone. Both devices must be on the same Wi-Fi. Tilt the phone to steer and use the on-screen gas, brake and gear buttons. The phone vibrates with the force feedback.

## Force feedback

The game's original steering-wheel force feedback (impacts, jumps, road surface, load) is turned into vibration on your controller or, without one, on the phone itself. You can switch it off in the settings menu.

## Settings

- **Resolution**: *Auto* (adapts to keep full speed), *Native* (screen resolution), or a fixed 480p-2160p
- Widescreen
- Vibration on/off

## Problems

Open an [issue](../../issues) and attach the log file:

```
Android/data/org.srally2/files/srally2.log
```

Please include your device model and Android version.

## Building from source

See [android/README.md](android/README.md). The repository also contains the static recompiler (`lifter/`), the on-device translator (`xlat/`) and the maintainer tooling used to verify it (`dev/`).

## Legal

This project contains no ROM, no ROM dump and no code derived from one. The APK translates the code from the zip you provide, on your device, at run time. Only use a ROM set you are legally entitled to.

Not affiliated with or endorsed by the original game's rights holders. All trademarks belong to their respective owners.

Licensed under GPLv3 (see [LICENSE](LICENSE) and [NOTICE](NOTICE)).

Developed with the assistance of Claude (Anthropic).
