# Android (experimental)

> **Status: builds, not yet run on a device.** The arm64 Player library,
> the Gradle project and `prismatix pack --target android` have produced
> debug and signed release APKs and an AAB from the `minimal` starter on a
> Windows host. Nothing has been installed on a phone or emulator yet, so
> runtime behaviour on Android (rendering, audio, lifecycle, touch,
> text-to-speech) is unverified. Please update this page after the first
> device run.

This page covers phases A0–A2 of the Android plan in the
[2026-09-30 release review](review/2026-09-30-release-review.md#7-android-export-評估).

## What is verified

| Area | State | Where |
| --- | --- | --- |
| arm64-android vcpkg dependencies | All 27 target ports build from a Windows host (NDK r27d) | `vcpkg/triplets/arm64-android-prismatix.cmake` |
| `libmain.so` | Links; stripped 19.6 MB; needs only FFmpeg and system libraries; 16 KB pages | `android-arm64` preset |
| Gradle project | `assembleDebug`, `assembleRelease`, `bundleRelease` succeed | `packages/cli/templates/android` |
| `prismatix pack --target android` | Builds the APK (and AAB when signing is configured) | [below](#build-an-apk) |
| Reading the package from the APK | Tested on desktop through SDL only | `Engine/IO/FileSource.h` |
| Background, foreground, low memory, terminate | Rules unit-tested on Windows only | `Engine/Platform/AppLifecycle.h`, `Engine/Runtime.cpp` |
| Touch gestures and `platform.touch` | Unit-tested with synthetic finger events | `Engine/Platform/TouchGestures.h`, [Touch input](#touch-input) |
| Text-to-speech (self-voicing) | Compiles; JNI to `android.speech.tts.TextToSpeech` | `Engine/Accessibility/SpeechService.cpp`, `PrismatiXSpeech.java` |
| TalkBack (screen reader tree) | Not supported | `Engine/Accessibility` |
| Play Asset Delivery | Not started | phase A3 |

## Prerequisites (host)

- Android SDK with `platforms;android-35` and `build-tools;35.0.0`, and
  `ANDROID_HOME` pointing at it. The template turns off AGP's automatic
  SDK downloads, so a missing package fails the build by name.
- Android NDK r27 or newer; set `ANDROID_NDK_HOME`.
- JDK 17 or newer in `JAVA_HOME`, and Gradle 8.9 or newer (tested with
  JDK 21 and Gradle 8.11.1). The first Gradle run downloads the Android
  Gradle Plugin from Google's Maven repository into `GRADLE_USER_HOME`.
- vcpkg (`VCPKG_ROOT`; Visual Studio's bundled copy works) and the desktop
  build of this repository: the Android build runs the host `shadercross`
  to compile built-in shaders.

## Build `libmain.so`

```sh
cmake --preset android-arm64
cmake --build --preset android-arm64
```

The preset chain-loads the NDK toolchain into vcpkg with the overlay
triplet `arm64-android-prismatix`: API 26, release-only, everything static
except FFmpeg, which is LGPL and ships as replaceable shared libraries.
The preset assumes a Windows host (`VCPKG_HOST_TRIPLET=x64-windows` and the
`shadercross.exe` path); on macOS or Linux, override both. To keep the
large Android trees off the system drive, add
`-B <dir> -DVCPKG_INSTALLED_DIR=<dir>/vcpkg_installed`.

Each Player directory is self-contained:

- `android/shipped/arm64-v8a/`: stripped `libmain.so`, the FFmpeg
  `lib*.so` files and `prismatix-player.json`. Pass this folder to the CLI.
- `android/shipped/symbols/arm64-v8a/libmain.so`: unstripped, for
  `ndk-stack`.
- `android/qa/...`: the QA Player with `PRISMATIX_E2E_*` hooks. The CLI
  refuses to ship it.

Notes from the first build:

- NDK r27's libc++ 18 needs `-fexperimental-library` for `std::jthread`
  and `std::stop_token`, and has no floating-point `std::from_chars`
  (the engine uses `px::ParseFiniteDouble`).
- vcpkg's `harfbuzz.pc` asks for `-lc++` (the shared C++ runtime); the
  build drops it because `libc++` is linked statically.
- Configure fails if the template's SDL Java glue
  (`app/src/main/java/org/libsdl/app`, copied from the SDL source release)
  differs from the native SDL version. When vcpkg updates SDL, copy the
  matching release's `android-project/app/src/main/java/org/` over it.

## Build an APK

Add the Android settings to `prismatix.json`:

```json
"distribution": {
  "android": {
    "applicationId": "com.example.firstlight",
    "versionCode": 3,
    "versionName": "1.2.0",
    "label": "First Light",
    "icon": "Assets/Branding/icon-512.png",
    "keystore": {
      "path": "../keys/upload.jks",
      "alias": "upload",
      "storePasswordEnv": "FIRSTLIGHT_STORE_PASSWORD",
      "keyPasswordEnv": "FIRSTLIGHT_KEY_PASSWORD"
    }
  }
}
```

Only `applicationId` is required. `versionName` defaults to the project
`version`, `label` to the project name, and the orientation follows the
resolution (landscape unless taller than wide). `icon` is a square PNG
(48–4096 px). The keystore names environment variables; passwords never go
in the file, and a `*pass*` key in `keystore` is rejected.

```sh
prismatix pack my-game --target android \
  --android-player out/build/android-arm64/android/shipped/arm64-v8a \
  [--artifact <built artifact>] [--gradle <gradle or its install dir>] \
  [--android-sdk <dir>] [--output <dir>] [--debug]
```

- Without a keystore (or with `--debug`) the result is a debug-signed
  `dist/<id>-android/<id>-debug.apk` for testing.
- With a keystore it is a signed `<id>.apk` plus `<id>.aab` for Play.
- `<output>/build/package` holds the packed game (reused when unchanged) and
  `<output>/build/gradle` the Gradle project (Gradle's caches survive
  between runs). The game goes into `assets/` stored uncompressed, laid out
  as on desktop (`Package/…`, `Content*.pdx`, `Notices/`); the desktop
  Player and its libraries are left out.

To do the same by hand, copy the template, put the `.so` files in
`app/src/main/jniLibs/arm64-v8a/`, the packed game in
`app/src/main/assets/`, optionally write `prismatix.properties` (keys:
`applicationId`, `versionCode`, `versionName`, `screenOrientation`,
`keystore`, `keyAlias`, `storePasswordEnv`, `keyPasswordEnv`), and run
`gradle assembleDebug`.

Install and watch the log on a device:

```sh
adb install -r dist/<id>-android/<id>-debug.apk
adb logcat -s SDL SDL/APP PrismatiX
```

### How the Player finds the game

`io::PackageFileSource()` is `SdlFileSource()` on Android. The Player opens
`Package/manifest.json`, every `*.pdx` shard and any VFS directory mount
through `SDL_IOFromFile`, which tries the app's internal storage first and
then the APK's `assets/`. Files that a later download places at the same
relative path in internal storage would be picked up first; nothing uses
this yet. Saves, settings, the profile and logs use `SDL_GetPrefPath`.
Signed single-file updates (`Package/updates`) are scanned with
`std::filesystem`, so they do not apply on Android yet. Keep APK assets
under about 1–2 GiB for side-loading; larger games need Play Asset
Delivery (phase A3).

### Lifecycle

The Runtime registers an SDL event watch. These rules are unit-tested on
Windows; they have not run on a phone.

- **Leaving the foreground:** the Player saves first, then all media pauses.
  The save goes to the autosave slot if a game is in progress, followed by
  the settings and profile, and the Player waits for the writes. Nothing is
  rendered until the app returns.
- **Returning:** only the audio tracks and videos the lifecycle paused
  resume. The frame clock restarts, and text density is recomputed for the
  new surface size.
- **Low memory:** drops textures not drawn in the last frame, font files no
  open font uses, and unreferenced decoded audio.
- **Terminating:** saves again only if the game ran since the last
  background save, then quits.
- `SDL_EVENT_RENDER_TARGETS_RESET` and `SDL_EVENT_RENDER_DEVICE_RESET`
  recreate the renderer's offscreen targets. A device reset also reloads
  file textures.

## Touch input

Fingers are turned into gestures. Each gesture fires the same command the
mouse, keyboard or gamepad already fires:

| Gesture | Same as | Default effect |
| --- | --- | --- |
| Tap | Left click | Advance the text, or activate the control under the finger |
| Long press (0.5 s) | `toggle-ui` (H) | Hide or show the message window |
| Two-finger tap | `menu` (Escape, gamepad B) | Whatever the project binds to `menu` in `inputActions` |
| Swipe up | `backlog` (B, wheel up) | Open the backlog |
| Drag | Held left button | Sliders and other draggable controls |

A tap acts when the finger lifts, so a swipe or long press never advances
the text as well. SDL's synthetic mouse copies of touches are ignored.
Mouse, keyboard and gamepad input is unchanged.

`platform.touch` is Player UI data (boolean), so TSX can show
touch-friendly variants:

```tsx
<Group name="TouchToolbar" visibleWhen="platform.touch" ... />
<Group name="HoverDock" visibleWhen="!platform.touch" ... />
```

The project setting `player.touchProfile` controls both:

| Value | Gestures | `platform.touch` |
| --- | --- | --- |
| `"auto"` (default) | on | `true` while the last pointer input was a finger |
| `"on"` | on | always `true`: preview touch layouts on desktop |
| `"off"` | off; the first finger acts as a plain mouse pointer | always `false` |

## Verification checklist for the first device run

1. The APK installs, the title screen appears, and logcat shows
   `Mounted archive 'Content.pdx' (… source=sdl)`.
2. A full branch of the `minimal` template plays to the end.
3. Pressing Home during dialogue saves to the autosave slot, audio stops,
   and returning resumes audio and the text.
4. Swiping the app away from recents, then relaunching, lets you load the
   autosave.
5. Video plays, pauses when backgrounded, and resumes.
6. Every gesture in the touch table works, and taps land on the control
   under the finger even when the game is letterboxed.
7. With self-voicing on, each dialogue line is spoken.

Record what broke and how you fixed it here.
