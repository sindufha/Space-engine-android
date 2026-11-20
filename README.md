# Space Engine v0.1.0 — Android Port (SpaceEngine 0.991.49.2095)

A static x86-64-to-C recompilation and native Android (`arm64-v8a`) compatibility layer for **SpaceEngine 0.991.49.2095 beta**, featuring a Win32/WGL-to-Android/EGL/Mesa Zink (OpenGL 4.6 over Vulkan) runtime shim and a native Android Launcher APK (`v0.1.0`, `versionCode 8`).

> **Warning — No Game Files Included**: This repository and its GitHub Release APK contain **only** the recompiled compatibility runtime and Android launcher. **No SpaceEngine game files, `.pak` assets, or original Windows binaries (`SpaceEngine.exe`, DLLs) are included.** You must provide your own legitimate PC copy of **SpaceEngine `0.991.49.2095`** on your device.

---

## What You Need

1. **Android Device Requirements**
   - **CPU**: 64-bit ARM (`arm64-v8a`)
   - **OS**: Android 10 or newer (`minSdk 29`, `targetSdk 34`)
   - **GPU**: Vulkan 1.1+ and OpenGL ES 3.2+ support (OpenGL 4.6 runs over Vulkan via bundled Mesa Zink)
   - **Free Storage**: ~4.5 GB+ on **Internal Storage** for your SpaceEngine PC files
2. **Release APK**
   - Download `SpaceEngine-v0.1.0.apk` from the **Releases** section on GitHub (`v0.1.0`), or build it yourself using the build instructions below.
3. **SpaceEngine `0.991.49.2095` PC Files**
   - The exact matching `system/SpaceEngine.exe` (`9,062,400` bytes, SHA-256 `172154e9...aeb0d3` for v0.991.49.2095)
   - The root SpaceEngine game folder containing:
     - `data/` (including `data/shaders/Shaders.pak` and `data/catalogs/Catalogs.pak`)
     - `config/` (including `config/main-def.cfg`)
     - `system/folders.cfg`

---

## Install & Setup Tutorial

### Step 1: Download & Install the APK
1. Open the **Releases** page of this GitHub repository on your Android phone and download the latest release `.apk`.
2. Open the downloaded `.apk` to install **Space Engine** (grant *"Install unknown apps"* permission to your browser/file manager if prompted).
   - Alternatively, install from PC via ADB:
     ```powershell
     adb install -r SpaceEngine-v0.1.0.apk
     ```

### Step 2: Copy Your SpaceEngine Game Files to Internal Storage
1. On your PC, locate your **SpaceEngine `0.991.49.2095`** folder (the root directory containing `data/`, `config/`, `system/`, etc.).
2. Copy the entire folder over USB to your phone's **Internal Storage** (for example: `Internal Storage/SpaceEngine/`).
   - *Tip*: Keep the files in **Internal Storage** rather than a removable SD card or cloud drive so the file picker resolves direct filesystem paths reliably.

### Step 3: Grant Storage Permission
1. Launch the **Space Engine** app.
2. If the storage notice banner appears at the top, tap **Grant Storage Access** and enable **Allow access to manage all files** (Android 11+) or grant storage permission (Android 10) so the native engine can read the multi-GB `.pak` assets directly.

### Step 4: Select Executable & Game Folder
1. Under **1. Engine Executable (SpaceEngine.exe)**, tap **Select `.exe`** and browse to `<your SpaceEngine folder>/system/SpaceEngine.exe`.
   - Wait a moment while the launcher checks the PE32+ header and SHA-256 hash; the status text turns green when verified.
2. Under **2. Game Asset Directory**, tap **Select Game Files** and pick the **root** `<your SpaceEngine folder>` (the folder that contains `data/`, `config/`, and `system/` — do **not** pick `data/` itself).
   - Wait until the status confirms `Verified data/, config/, system/folders.cfg`.
   - *Note*: Both paths are remembered automatically for future launches. The launcher also saves a backup of your desktop `config/main-user.cfg` as `main-user.desktop.bak` (if writable) and writes mobile-tuned VRAM/cache/resolution settings before starting.

### Step 5: Launch the Game
1. Tap **Start**.
2. The app switches to full-screen **Landscape**, shows SpaceEngine's loading banner, and initializes OpenGL into the **Main Menu**.
   - *Note*: First-time loading and shader compilation can take a minute depending on your device's GPU/storage speed.

---

## How to Play (Controls Guide)

### Main Menu
- Tap directly on the screen to select options in SpaceEngine's **Main Menu** (such as **Planetarium** / **Continue**).

### Planetarium Touch Overlay & HUD
Once the planetarium finishes loading, the collapsible touch overlay appears automatically:

- **Move (Virtual Joystick — Bottom-Left)**: Drag the analog stick to fly forward/backward and strafe (`W` / `A` / `S` / `D`).
- **Camera Look / Orbit (Touch Drag)**: Drag on open space to rotate the camera (`Look` mode) or orbit around the selected object (`Orbit` mode).
- **Zoom (Two-Finger Pinch)**: Pinch in/out with two fingers to adjust zoom/speed.
- **Action Buttons (Bottom-Right 2×3 Grid)**:
  - **Find**: Opens SpaceEngine's object search (`F3`) and brings up the on-screen keyboard automatically.
  - **Keys**: Manually show or hide the Android soft keyboard.
  - **Go To**: Flies smoothly to the selected celestial body (`G`).
  - **Stop**: Halts camera/ship velocity (`Z`).
  - **Center**: Centers the selected object in view (`C`).
  - **Look / Orbit**: Toggles single-finger screen drag between free camera rotation (`Look`) and object orbiting (`Orbit`).
- **Time Controls (Bottom-Center)**:
  - **Time -** (`J`), **Pause** (`Space`), **Time +** (`L`) to slow down, pause/resume, or speed up time flow.
- **HUD Toggle (Top-Right)**: Tap **HUD** to collapse or expand all on-screen controls for an unobstructed view.

---

## Repository Structure

- **`android/`** — Android Gradle application module (`org.spaceengine.port`):
  - `app/build.gradle`, `build.gradle`, `settings.gradle`, `gradle.properties` — Gradle (`8.11.1`) / AGP (`8.5.2`) config (`compileSdk 34`, `minSdk 29`, `targetSdk 34`, NDK `27.1.12297006`, CMake `3.22.1`).
  - `app/src/main/java/org/spaceengine/port/` — `LauncherActivity.java`, `NativeEngineBridge.java`, `SplashSurfaceView.java`, `TouchControlsOverlayView.java`, `JoystickView.java`.
  - `app/src/main/java/org/libsdl/app/` — SDL2 Android glue (`SDLActivity.java`, `SDLAudioManager.java`, HID/controller helpers).
  - `app/src/main/cpp/` — `CMakeLists.txt` and `jni_bridge.c` building `libSpaceEngine.so`.
  - `app/src/main/jniLibs/arm64-v8a/` — Bundled Mesa Zink runtime libraries (`libEGL_mesa.so`, `libzink_dri.so`, `libglapi.so`, `libcutils.so`, `libc++_shared.so`).
  - `app/src/main/res/` — Portrait launcher UI (`launcher_bg.jpg`, watermark), touch HUD layout, strings, colors, and themes.
- **`recomp/`** — Statically lifted x86-64 engine C units (`recomp_funcs_0000.c`–`recomp_funcs_0058.c`, `recomp_funcs.h`), indirect dispatch/imports (`recomp_dispatch.c/h`, `recomp_imports.c`), Win64 CPU state (`cpu64.h`), MSVC FH3/FH4 exception handling (`eh64.c/h`), runtime PE32+/SHA-256 loader (`pe_loader.c/h`, `sha256.c/h`), and native thread host (`engine_host.c/h`).
- **`shims/`** — Win32/Win64 HLE compatibility layer (`hle_bridge.c/h`, `crt_intercepts.c`, `win32_kernel32.c`, `win32_misc.c`, `win32_user32_gdi32.c`, `minizip_ord.c`, `wgl_egl.c`, `shader_es3.c/h`, `gl46_core_direct.inc`, `gl46_procs_gen.inc`, `png_tiff_hle.c`, `sdl2_mixer_hle.c`) plus `shims/third_party/` (`SDL2`, `SDL-Mixer-X`, `libpng`, `libtiff`, `zlib`, `config/`).

---

## Building from Source

Requires **JDK 17**, **Android SDK Platform 34**, **NDK 27.1.12297006**, **CMake 3.22.1**, and **Gradle 8.11.1**:

```powershell
cd android
gradle assembleDebug
```

Output APK: `android/app/build/outputs/apk/debug/app-debug.apk`

---

## Third-Party Libraries & Licensing

- **SpaceEngine** (`SpaceEngine.exe`, `data/`, and original assets) is proprietary software owned by Cosmographic Software LLC and is **not** distributed in this repository.
- Open-source middleware compiled in `shims/third_party/` and `jniLibs/` includes **SDL2**, **SDL-Mixer-X**, **libpng**, **libtiff**, **zlib**, and **Mesa Zink**, governed by their respective open-source licenses.
- Porting shims, Android launcher sources, and runtime loader code follow the project's compatibility-layer license terms.
