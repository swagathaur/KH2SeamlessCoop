# Android (experimental)

An APK for arm64 Android handhelds (tested target: AYN Thor). Same game and
co-op as the PC build, without HD textures or movies. It joins PC hosts (and
can host) as long as both sides run the same version.

## Building (on the PC)

    powershell -ExecutionPolicy Bypass -File tools\android\build-apk.ps1

Needs the Android SDK at `C:\Android\sdk` (NDK, build-tools, a platform) and a
JDK. Output: `build\android\kh2coop.apk`. Logs: `logs\android-configure.log`,
`logs\android-build.log`. The APK contains the recompiled game: keep it to
yourself.

## On the device

1. Copy into a folder called `KH2` at the top of the internal storage:
   - the disc image (`.iso` from the PC's `Src` folder)
   - the PC's whole `game` folder
   - optionally `mc0` (and `mc1`) from the PC, to bring your saves
2. Install the APK (allow installing from unknown sources).
3. Start "KH2 Co-op". The first time, allow All files access in the screen
   that opens, then go back to the app.

Controls: the gamepad as on PC. Back (or Select+Start) opens the co-op menu;
it works with the D-pad/A or by touch. Text fields open an on-screen keypad.

Settings are in `KH2/kh2recomp.ini`; the log is `KH2/logs/play.log` (also in
logcat, tag `kh2`).

## GPU driver

Use Mesa's Turnip driver for Adreno: it is much faster here than the
Qualcomm driver (about 45 fps against 15 in a crowded Twilight Town street
on an AYN Thor). Download a Turnip package made for Android emulators (a .zip
with meta.json; tested: Turnip T28 by Mr_Purple_666), then in the menu:
GPU driver, Choose driver file..., pick the .zip, Close app, open it again.
"Use system driver" goes back. A driver that cannot run the renderer is
dropped automatically (the app closes; open it again).

## Performance notes

- The renderer runs on a thread of its own on Android (`render-thread = 1`,
  the default there). The guest stays at most two frames ahead of it.
- Turnip on Qualcomm's kernel driver: zero-timeout timestamp polls waited for
  the GPU; android_kgsl_hook.c patches the driver's ioctl() to answer them at
  once. paraLLEl-GS uses fences instead of timeline semaphores with Turnip.
- Adreno: one GPU queue for everything; the hierarchical binner is off (it left
  rows of tiles unrendered); the sampler-feedback pass is off (does not link on
  Qualcomm's driver).
- KH2 reads a 32x8 depth patch back every frame (the sun's lens flare); on
  Android the game gets the previous result and only every 8th read reaches
  the GPU.
- The log has GPU time per stage (`[gpu]`), frame steps (`[frame]`) and the
  driver hook's counts (`kh2-kgsl`) every 3 s. With USB debugging,
  `simpleperf record --app com.kh2coop.recomp` profiles it (the APK is
  profileable).
- Texture uploads (sceGsExecLoadImage) queue behind the DMA worker's VIF1/VU1
  work (patch 0038) instead of the EE waiting for it (a quarter of the EE's
  time in crowded scenes on the Thor).
- Movies are skipped: the Android build has no MPEG decoder yet.
