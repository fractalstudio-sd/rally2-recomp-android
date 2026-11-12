# srally2-recomp

Static recompilation of **Sega Rally 2** (arcade, Sega Model 3, `srally2`) for desktop and Android. The PowerPC 603e game code is lifted to portable C and the Model 3 hardware is reimplemented natively.

Goal: a native, platform-agnostic port in the style of the Model 2 Daytona USA recompilation projects. The game's CPU code becomes portable C, the hardware is reimplemented natively, and **there is no emulator in the loop**.

## Status

| Piece | State |
|---|---|
| ROM region builder (CRC-checked, no emulator) | done |
| Function discovery (Ghidra headless, static) | done |
| PPC 603e -> C lifter (integer, FPU, branches, CR, SPR, jump tables, exceptions) | done |
| Runtime: bus, MMIO, IRQs, decrementer, guest contexts | done |
| Video: TileGen 2D + Real3D (OpenGL 4.1 core), SDL2 window, Retina-aware | done |
| Audio: 68000 + SCSP, DSB2 MPEG | done (first pass: sound and music output) |
| Input: keyboard defaults, gamepad via SDL | done (basic) |
| Widescreen / native resolution / remapping UI | `-widescreen`, `-w/-h` work; polish planned |
| Android (arm64, GLES 3.2, touch controls, Mali + Adreno) | done - see `android/README.md` |
| On-device native translator (PPC -> AArch64 / ARM, `xlat/`) | done: release APKs carry no game code |

**Playable:** boots to attract mode, coin/start, menus and races run natively. The CPU code is static C, not interpreted. NVRAM (settings, rankings, bookkeeping) persists between runs. Test/Service mode work (on Android: on-screen TEST / SERVICE buttons).

## Build and play (Linux / macOS)

Needs python3, cmake, SDL2, a C/C++ compiler, JDK 21+, curl, unzip.
macOS: `xcode-select --install && brew install cmake sdl2 openjdk@21`.

```
scripts/setup.sh /path/to/srally2.zip   # ROM -> images -> lifted C (about 2 min, needs your own ROM)
scripts/build.sh                        # compile the native game
scripts/run.sh                          # play   (options: -native, -fullscreen, -ires N, -w N -h N, -43; F11 = fullscreen toggle)
```

Keys: `3` coin, `1` start, arrows steer/gas/brake, `Q W E R` gears, `A` view, `S` handbrake, `Esc` quit. A gamepad works too (left stick, D-pad, buttons 1/2/5-8).

## Options

`-st` single thread, `-mt` / `-nogpumt` / `-gpumt` force the threading mode. By default the thread layout is chosen once at startup from the core count
(PPC main board, sound board, drive board and the GL render thread run in parallel and are joined every frame, so audio pacing is the same as single-threaded).
`-ires N` internal render height, `-43` 4:3, `-nounlock` do not raise the bookkeeping counters (see below).

## Hidden cars

The Lancia Delta and Toyota Celica need >= 30 days of operation or >= 1000 games in the bookkeeping, then View + Handbrake held on the car select screen.
The runtime raises those two counters to 40 days / 1500 games only if they are lower, then they keep counting normally. `-nounlock` turns this off.

## Legal

* No ROM, ROM dump or code derived from a ROM is in this repo, nor in the release APKs (they translate the user's ROM on the device). Everything derived from **your own** `srally2.zip` is generated locally into `build/` and git-ignored.
* `dev/` holds maintainer-only tooling that uses [Supermodel](https://github.com/trzy/Supermodel) (GPLv3) as a test oracle for the lifter. Users never run it. The patch there is GPLv3, so the repository is GPLv3.
* Developed with the assistance of Claude (Anthropic). Commits made with it carry a `Co-Authored-By` trailer.

## Native translator (`xlat/`, release builds)

Release APKs do not contain lifted code. `-DM3_XLAT=ON` replaces the lifted C with a translator that runs on the device:
on first start of the game it reads `crom.bin` (built from **your** `srally2.zip` by the launcher), finds the functions
(`xlat/frontend.cpp`: vectors, pointer scan, prologue scan, fixpoint over calls and jump tables) and writes native code
for them straight into executable memory: AArch64 (`backend_a64.cpp`) on 64-bit devices, ARM A32 (`backend_a32.cpp`,
ARMv7 + VFPv3, VFPv4 FMA and hardware divide used when the CPU has them) on 32-bit ones. A routine only reachable through
a computed call is translated the first time it is called. Analysis + translation take well under a second on a phone.

* Same semantics as the lifter, one native function per guest function, guest registers kept in host registers where it
  pays (6 on AArch64, 4 on ARM), compare+branch fused, constant addresses resolved at translation time.
* Verified in lockstep (`dev/headless`, under qemu-user) against the lifted C reference: identical RAM every 100 frames
  for 4000 frames (boot, attract, menus, a race) on AArch64 and on ARM, with and without VFPv4 / divide instructions.
* The touch-menu routines (`runtime/app/menu_guest.c`) hook the original routines by address (`xlat/guest/hooks.def`).

## Layout

```
data/srally2.json     ROM layout metadata (file names, offsets, CRC32s) - no ROM content
tools/build_images.py ROM zip -> flat region images (CRC-checked)
ghidra/               headless scripts: discovery + function export
lifter/               PPC -> C translator and per-function generator
runtime/ppc_rt.h      CPU state + helpers used by lifted code (memory and environment come from the host)
scripts/setup.sh      the user pipeline
xlat/                 on-device translator: front end, AArch64 and ARM back ends, runtime, touch-menu hooks
dev/                  maintainers only: Supermodel trace patch, lockstep verifier, headless lockstep harness
```

## Accuracy notes

* fmadd/fmsub: the real 603e is fused; the default runtime is fused. The maintainer verifier builds with `-DPPC_NONFUSED_FMA` because the reference emulator is not.
* `divw` by zero / overflow mirrors the reference emulator (architecturally undefined). `dcbz` is a no-op.


## Asset packs (optional): dump, upscale, replace

Everything is off unless you pass `-dump-assets DIR` or `-pack DIR`.

```sh
./scripts/run.sh -dump-assets ./assets        # play (menus, HUD, tracks) - new assets are written as you go
python3 tools/upscale_assets.py ./assets ./pack -s 4          # PIL Lanczos (no extra deps: pip3 install pillow)
python3 tools/upscale_assets.py ./assets ./pack -s 4 -e realesrgan   # Real-ESRGAN / Upscayl CLI
python3 tools/upscale_assets.py ./assets ./pack -e cmd --cmd 'mytool {in} {out}'   # any generative upscaler
./scripts/run.sh -pack ./pack
```

| folder | content | replacement rules |
|---|---|---|
| `3d/<hash>.png` | every 3D texture (decoded RGBA, upright) | any size, same aspect; filtered, sampled with the original wrap mode |
| `2d_tiles/tile_<hash>.png` | 8x8 blocks of the 2D layers (menus, HUD, fonts) | exactly 8S x 8S, same S for all tiles; composed into an S-times layer |
| `2d_layers/layer_<hash>.png` | whole 496x384 2D layer (static screens) | any size; a layer wider than 1.5:1 (e.g. 1920x1080) is drawn across the full window = true 16:9 menus. Takes precedence over tiles |

Files are named by a content hash, so a replacement keeps working across runs; delete a file to fall back to the original.
Alpha is kept (`upscale_assets.py` upscales it separately). 2D assets are matched on the composed pixels, so text that is
drawn at a different position still matches tile-by-tile only when it sits on the 8-px grid.
Dumped and upscaled files are derived from the ROMs: keep them local, never commit or share them (`.gitignore` covers
`assets/` and `pack/`).


## Android

`android/` contains a Gradle/NDK project (OpenGL ES 3.2 build of the same renderer, hand-written fiber switch instead of
`ucontext`, multitouch on-screen controls). See [android/README.md](android/README.md). The touch overlay can also be tried
on the desktop with `-touch` (the mouse acts as a finger); the GLES renderer with `-DM3_GLES=ON` (needs Mesa/ANGLE GLES 3.2).

## Gamepad

Any USB or Bluetooth controller SDL can see works, hot-plugged at any time. Pads SDL has no mapping for get a generic one (Xbox-like order).

| Control | Pad |
|---|---|
| Steering | left stick (d-pad left/right also works) |
| Gas / Brake | right / left trigger (d-pad up/down also works) |
| View | A |
| Handbrake | X or B |
| Shift up / down | RB / LB |
| Start / Coin | Start / Back |
| Test / Service | hold Start + click left / right stick |

The touch overlay hides while a pad is in use and comes back when you touch the screen.

To rebind, put `pad.ini` in the app's data folder (`gamecontrollerdb.txt` there adds SDL mappings for odd pads):

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
test=guide
service=misc1
deadzone=0.10
```

Names are SDL controller names (`a b x y back guide start leftstick rightstick leftshoulder rightshoulder dpup dpdown dpleft dpright`, axes `leftx lefty rightx righty lefttrigger righttrigger`).

## 32-bit ARM (armeabi-v7a, Android TV)

The code builds for 32-bit ARM too (`CMAKE_SYSTEM_PROCESSOR=arm`): the fiber switch has an AAPCS version in `cpu_glue.cpp` and `runtime/emutls_arm.c` supplies emulated TLS for bionic before API 29. Build with the NEON feature off (`-mcpu=cortex_a9+vfp3-neon`; the NEON code paths of some compilers crash on SDL). The APK for TVs additionally declares `LEANBACK_LAUNCHER`. Expect it to be much slower than on a phone: TV SoCs have small CPUs.
