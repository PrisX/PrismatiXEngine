# Android (experimental)

> **Status: unverified.** Nothing in this document has been built into an
> APK or run on an Android device yet. The engine code paths it relies on
> are unit-tested on Windows, but the toolchain, the Gradle project and the
> steps below are a first draft. Expect to fix things the first time you go
> through them, and please update this page when you do.

This page covers phases A0 (toolchain) and A1 (IO and app lifecycle) of the
Android plan in the [2026-09-30 release review](review/2026-09-30-release-review.md#7-android-export-評估).
There is no `prismatix build --target android` yet (phase A2), so every step
here is manual.

## What the engine already does for Android

| Area | State | Where |
| --- | --- | --- |
| Reading the package from the APK | Done, tested on desktop through SDL | `Engine/IO/FileSource.h` |
| Background, foreground, low memory, terminate | Done, rules unit-tested | `Engine/Platform/AppLifecycle.h`, `Engine/Runtime.cpp` |
| Touch gestures and `platform.touch` | Done; gestures unit-tested with synthetic finger events | `Engine/Platform/TouchGestures.h`, [Touch input](#touch-input) |
| Building `libmain.so` | CMake branch written, never run | `android-arm64` preset |
| Gradle project | Template written, never built | `packages/cli/templates/android` |
| Signing, `.aab`, Play Asset Delivery | Not started | phase A2/A3 |
| TalkBack, text-to-speech | Not supported; returns "unavailable" | `Engine/Accessibility` |

### Package files come from the APK

`io::PackageFileSource()` is `SdlFileSource()` on Android. The Player opens
`Package/manifest.json`, every `*.pdx` shard and any VFS directory mount
through `SDL_IOFromFile`. For a relative path, SDL tries the app's internal
storage first and then the APK's `assets/`. That means:

- The packaged game goes in `app/src/main/assets/`, with the same layout as a
  desktop distribution (`Package/…` and `Content*.pdx` at the root).
- The template stores `.pdx` and `.json` assets uncompressed
  (`noCompress`). The Player seeks inside shards, and a compressed asset
  would be inflated again on every backward seek.
- Files that a later download places at the same relative path in internal
  storage are picked up first. Nothing uses this yet.
- Saves, settings, the profile and logs use `SDL_GetPrefPath` and stay on
  the file system.
- Signed single-file updates (`Package/updates`) are scanned with
  `std::filesystem`, so they do not apply on Android yet.

### Lifecycle

The Runtime registers an SDL event watch. The rules below are unit-tested on
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

## Prerequisites (host)

- Android SDK with platform 35 and build-tools. Android NDK r27 or newer.
  Set `ANDROID_NDK_HOME`.
- JDK 17 and Gradle 8.9 or newer. Android Studio provides all of these.
- vcpkg with `VCPKG_ROOT` set. The same checkout that builds the desktop
  engine is fine.
- The desktop build of this repository. The Android build runs the host
  `shadercross` executable from vcpkg to compile built-in shaders.
- SDL 3.4.x **source** matching the vcpkg `sdl3` version (currently 3.4.10),
  for its Java files.

## Steps

### 1. Build `libmain.so`

```sh
cmake --preset android-arm64
cmake --build --preset android-arm64
```

The preset chain-loads the NDK toolchain into vcpkg and uses the overlay
triplet `vcpkg/triplets/arm64-android-prismatix.cmake`. That triplet targets
API 26 and links everything statically except FFmpeg, which is LGPL and
ships as shared libraries.

The preset points `PRISMATIX_SHADERCROSS_EXECUTABLE` at
`vcpkg_installed/x64-windows/tools/...`. On a macOS or Linux host, pass
`-DPRISMATIX_SHADERCROSS_EXECUTABLE=` with the host triplet's `shadercross`
instead.

The build writes:

- `out/build/android-arm64/android/shipped/arm64-v8a/libmain.so`: the
  shipped Player.
- `out/build/android-arm64/android/qa/arm64-v8a/libmain.so`: the QA Player
  with `PRISMATIX_E2E_*` hooks. Never ship it.

Only the Player is built for Android. The packager and the other tools stay
on the host, and `BUILD_TESTING` is off.

Parts of the build that are most likely to need work on the first try:

- vcpkg ports cross-compiling to `arm64-android`, especially `fribidi`
  (meson), `ffmpeg` and the `sdl3-mixer` codecs.
- `pkg-config` finding fribidi for the target, not the host.
- The static SDL3 link: extra system libraries may be needed besides
  `android` and `log`.

### 2. Start the Gradle project

Copy `packages/cli/templates/android` to a new folder, then:

1. Copy `android-project/app/src/main/java/org/libsdl/app/` from the SDL
   3.4.x source to `app/src/main/java/org/libsdl/app/`. The Java glue must
   match the native SDL version exactly.
2. Copy `libmain.so` and the FFmpeg shared libraries (`libavcodec.so`,
   `libavformat.so`, `libavutil.so`, `libswresample.so`, `libswscale.so`
   and any others under `vcpkg_installed/arm64-android-prismatix/lib`) to
   `app/src/main/jniLibs/arm64-v8a/`.
3. Set `applicationId`, `namespace` if you move the activity, `app_name`,
   and replace the launcher icon placeholders
   (`res/drawable/ic_launcher_foreground.xml`,
   `res/values/ic_launcher_background.xml`).
4. Run `gradle wrapper` once, or open the folder in Android Studio. The
   template does not include the wrapper jar.

`PrismatiXActivity` extends SDL's `SDLActivity` and loads only `main`. SDL3
is inside `libmain.so`, and Android 6+ resolves FFmpeg from the same
native library directory.

### 3. Add the game

Package the game for desktop as usual, then copy the distribution into
`app/src/main/assets/`, leaving out the Player executable and desktop
libraries:

```
app/src/main/assets/
  Package/manifest.json
  Package/...
  Content.pdx            (or Content-00.pdx, Content-01.pdx, …)
```

Keep an APK under about 1–2 GiB of assets for side-loading. Larger games
need Play Asset Delivery (phase A3).

### 4. Build and install

```sh
./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb logcat -s SDL SDL/APP PrismatiX
```

For a release build, create a keystore and add a `signingConfigs` block.
The template leaves signing to you.

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

1. `cmake --preset android-arm64` configures, and `libmain.so` links.
2. The APK installs, the title screen appears, and logcat shows
   `Mounted archive 'Content.pdx' (… source=sdl)`.
3. A full branch of the `minimal` template plays to the end.
4. Pressing Home during dialogue saves to the autosave slot, audio stops,
   and returning resumes audio and the text.
5. Swiping the app away from recents, then relaunching, lets you load the
   autosave.
6. Video plays, pauses when backgrounded, and resumes.
7. Every gesture in the touch table works, and taps land on the control
   under the finger even when the game is letterboxed.

Record what broke and how you fixed it here.
