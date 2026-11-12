# Sega Rally 2 on Android (arm64, OpenGL ES 3.2, on-screen controls)

Same static-recompiled game as the desktop build; nothing from the ROMs is in this repository. You build the APK on
your computer from your own ROM dump, then copy the game data to the phone.

Needs a phone with OpenGL ES 3.2 and Android 8+ (tested target: Snapdragon 8-class). The game code is compiled for arm64.

## Install (end users)

1. Install the APK from the Releases page.
2. Open the app. The first screen asks for **your own** `srally2.zip` (Sega Rally 2, Export - MAME set). It is checked file by file (CRC32) and the game data is built on the phone.
3. Play. NVRAM (settings, rankings) is kept in the app's files folder. No ROM data is contained in the APK.

## Build (developers)

1. Desktop pipeline once (creates the lifted code from your ROM zip): `scripts/setup.sh /path/to/srally2.zip`
2. Install Android Studio (SDK, command-line tools, JDK), then:
   ```sh
   android/build.sh            # -> build/srally2.apk   (first run downloads SDL2 and the NDK, ~10 min)
   android/build.sh install    # same, then adb install -r  (USB debugging on)
   ```
   The lifted game code is compiled into the APK, so do not publish an APK you built unless you accept that (see Legal in the main README).
3. Optional: `android/push_images.sh` copies pre-built game images to the phone instead of using the on-phone builder.

## Controls

* Left: steer (`<` `>`), `VIEW` camera, `HANDBRAKE`.
* Right: `GAS`, `BRAKE`, `-` / `+` gear lever (N,1-4; leave at N if the game is set to automatic).
* Top: `COIN`, then `START`. Multi-touch works (steer + gas + brake together).
* Bluetooth/USB gamepads and keyboards work too (same default bindings as the desktop version).

## Options

Put them in `files/args.txt` (one line) - e.g. `-ires 720` (internal render height, default 1080; lower = faster),
`-43` (4:3 instead of widescreen). The game writes `files/srally2.log` (use it when reporting problems).

Asset packs (`-pack DIR`, see the main README) work too: put the pack under `files/` and add `-pack /sdcard/Android/data/org.srally2/files/pack` to args.txt.

## Notes

* The "release" build is the only sensible one: the lifted game code is far too slow unoptimised. It is signed with the
  debug key so it installs directly.
* SDL2 (zlib licence) is downloaded at build time; its Java glue is vendored in `app/src/main/java/org/libsdl/app`.
