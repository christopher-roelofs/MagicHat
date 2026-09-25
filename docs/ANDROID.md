# Android test app

The Android app uses the existing SDL frontend and native MIPS/68k cores.
It targets ARM64 Android 8.0 or later. It is a sideloadable debug APK with
optimized native code; it does not require root, Termux, or a Linux desktop.

## Build and install

Requirements: Java 17, Android SDK platform 35/build tools, NDK
27.2.12479018, and CMake 3.22.1. Gradle 8.11.1
and Android Gradle Plugin 8.5.2 are pinned. The build script downloads SDL
2.32.10 from its official release site and checks its SHA-256.

```sh
# Defaults: SDK at ~/Android/Sdk.
./scripts/build_android.sh
# Or set ANDROID_HOME explicitly.
adb devices -l
adb -s DEVICE_SERIAL install -r out/android/magiccap-arm64-debug.apk
adb -s DEVICE_SERIAL shell am start -n org.magicrecomp.app/.LauncherActivity
```

For wireless debugging, pair using the address/port and code from **Pair
device with pairing code**, then connect to the separate address/port on
the main Wireless debugging screen:

```sh
adb pair IP:PAIRING_PORT
adb connect IP:CONNECTION_PORT
```

The app packages SDL and the emulator as Android shared libraries, with
libc++ linked into the emulator. No desktop glibc/musl libraries are used.
The NDK flexible-page-size setting enables 16-KiB alignment, including for
newer Android devices. ROMs are not bundled in the APK.

## Use

Open **Magic Cap**, choose **Import ROM**, then select a firmware file with
the system file picker. Tap an imported ROM and choose **Start** or **Fresh
temporary session**. Architecture/device detection uses the ROM contents.

Imports are copied into separate app-private folders. The original file is
unchanged; repeated imports cannot overwrite an existing ROM or its saved
state. `.state` uses the imported ROM's filename stem inside its folder.
Uninstalling the app removes its private files; this first version does not
have an export UI. Updating with `adb install -r` preserves them.

The emulator's control rail offers power, saved-state, panel rotation, and
close actions appropriate to the active device. Android Back closes the
current emulator session cleanly.
The strip reserves its own space above the guest panel so it does not obscure
guest touch targets. Device rotation resizes the panel automatically.

Save state writes a whole-machine snapshot for DataRover and supported 68k
devices. Each imported ROM keeps its own state; temporary sessions still
permit explicitly requested saves.

Switching apps or locking the display pauses emulation, releases held input,
clears queued audio, and saves the whole machine on the emulator thread.
Returning resumes execution without trying to catch up for background time.
This is the save that matters most here: Android can take the process at any
point afterwards without asking again, and the next start picks up from it.
An abrupt kill before the save completes can lose changes since the previous
one.
Closing through the app controls saves before its emulator process exits.
Each new session uses a separate native process to reset existing CLI globals.

Select **View session log** in the ROM list to inspect startup/errors. The
last 64 KiB of `session.log` are displayed; the full file is app-private.
For debug builds, files can also be inspected using `adb shell run-as
org.magicrecomp.app`.

## CPU engine

The app passes `--cpu-engine auto`, which uses the native MIPS engine
where the device allows an
executable mapping and the interpreter where it does not; the session log's
`cpu:` line says which was used. The 68k machines have one engine and accept
the option without changing behavior. On the SM-P610 (Android API 36) the
native engine's code arena uses the dual `memfd` mapping, so no page is ever
writable and executable; a device whose policy refuses an executable file
mapping falls back to one anonymous mapping whose window is made writable
only while a block is emitted. `MRC_JIT_SINGLE_MAP=1` selects that fallback
on a host that does not need it.

DataRover on the SM-P610 went from 0.724x real time (interpreter, before)
to 0.996x, which is the pacing limit rather than the engine's: 99.7% of
slots ran natively over 2.8 billion of them.

The 68k machines use the block engine described in
[M68K_CORE.md](M68K_CORE.md), which `--cpu-engine auto` selects here as
it does on the desktop. It has an AArch64 backend, so these devices get
native code and not only the block layer: 39% off the wall clock on an
Orange Pi 5, with the whole of RAM byte-identical to a reference run at
the end of six hundred million instructions. The Envoy uses this same
project-owned CPU32 core; both interpreter and JIT maintain its modeled
cycle count for battery RAM.

## Initial scope

Display, touch, audio, host battery status, retained memory and ROM detection
reuse the existing implementations. Networking/libslirp is disabled in this
first build. Serial console/PC Link host connections and memory-card selection
have no Android UI yet. No broad storage, microphone or network permissions
are requested.

References: [SDL Android integration](https://wiki.libsdl.org/SDL2/README-android)
and [Android native page sizes](https://developer.android.com/guide/practices/page-sizes).

## Validation on the first tablet

Tested on an SM-P610 running Android API 36, connected with wireless ADB.
This session covered PIC-2000 and DataRover only; it was not a validation of
the other supported 68k ROMs on this tablet:

- Installed the APK and imported both ROMs through Android's file picker.
- PIC-2000 reached calibration and the Hallway; interactive touch/drawing worked.
- DataRover reached the Desk; interactive input, audio initialization and panel
  rotation worked. The user reported better MIPS speed than 68k on this device.
- PIC backgrounding wrote its retained-memory payload, reduced CPU use to near
  idle, and resumed the same guest. (Recorded before saved states replaced
  retained memory; the same path now writes the whole machine.)
- DataRover saved and reloaded a state, and toggled power off/on.
- Android Back opens one controls dialog; Continue dismisses it. Both keyboard
  Back events and the modern callback are handled without stacking dialogs.
- All 26 desktop regression tests passed, including shared display rotation,
  background checkpointing and renderer reset tests.
- APK signature and 16-KiB ZIP alignment checks passed. Native ELF load segments
  are aligned to 16 KiB. The physical tablet uses 4-KiB pages; execution on a
  16-KiB-page device has not yet been tested.

The final test leaves DataRover running at the Desk. Build/test logs and
screenshots are in `out/android/`; these are local artifacts, not app assets.
