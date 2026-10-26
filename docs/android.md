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
