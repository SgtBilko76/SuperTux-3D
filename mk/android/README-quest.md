# SuperTux 3D for Meta Quest and PICO

This builds SuperTux as a standalone APK for the Meta Quest headsets
(Quest 2 / 3 / 3S / Pro) and for PICO headsets (PICO 4, 4 Ultra, Neo 3).
One APK covers both: the vendor-specific manifest entries are ignored by
the other runtime. The game is rendered stereoscopically through
OpenXR: the flat 2D scene is presented on a large virtual screen in front
of the player, and every drawing layer is placed at its own depth, so
backgrounds sit farther away than the tiles while the HUD and menus float
closer. Head movement gives real parallax between the layers.

The Touch controllers are mapped as follows:

| Controller input          | Game control                      |
|---------------------------|-----------------------------------|
| Left thumbstick           | Move (left/right/up/down)         |
| Right thumbstick          | Peek (look around)                |
| A / left trigger          | Jump (select in menus)            |
| B                         | Back (in menus)                   |
| X / right trigger         | Action (run, grab, shoot)         |
| Y                         | Item                              |
| Left grip / right grip    | Peek left / right                 |
| Menu button (left)        | Pause / open menu                 |
| Left thumbstick click     | Recenter the virtual screen       |
| Right thumbstick click    | Cheat menu (debug builds only)    |

On PICO the same layout is used, through the
`XR_BD_controller_interaction` bindings.

A paired bluetooth gamepad or keyboard keeps working alongside.

The APK also runs flat on regular Android devices: if no OpenXR runtime is
found the game logs a warning and starts in normal 2D mode.

## How it works

* `src/vr/vr_system.*` owns the OpenXR instance/session, the per-eye
  swapchains and the frame loop (`xrWaitFrame` / `xrEndFrame`). It is
  created in `Main` right after the OpenGL video system.
* `src/vr/vr_input.*` defines an OpenXR action set with bindings for the
  Touch controllers and feeds the game's `Controller`. The PICO profiles
  (`/interaction_profiles/bytedance/pico4_controller` and
  `pico_neo3_controller`) are suggested as well when the runtime
  advertises `XR_BD_controller_interaction`; the extension is requested
  only in that case, since asking for it on a Quest would fail
  `xrCreateInstance`.
* `src/video/layer_projection.hpp` maps a drawing layer to a depth plane.
  The mapping is linear in 1/depth: layer -300 (backgrounds) is twice as
  far as layer 0 (tiles), layer 600 (GUI) is at half the distance.
* `Compositor::render` composes the screen once per view, and
  `GLScreenRenderer` renders into the eye framebuffer with a per-layer
  perspective projection (`GL33CoreContext::set_layer`).
* The Android build now uses the OpenGL ES 3 renderer with the
  `data/shader/shader300es.*` shaders instead of the SDL renderer.

Tunables in the config file (`config` in the app's data dir, `video` section):

    vr_screen_distance   distance to the reference plane in meters (default 3.0)
    vr_screen_width      width of the virtual screen in meters (default 4.0)
    vr_depth_strength    layer separation, 0 = flat, 1 = default

## Building

Requirements: Android SDK (platform 35, build-tools 36), an NDK, JDK 17+,
vcpkg, git.

1. Install the dependencies with vcpkg for the Quest's ABI (arm64):

       export ANDROID_NDK_HOME=$HOME/Android/Sdk/ndk/<version>
       cd $VCPKG_ROOT
       ./vcpkg install --classic --triplet=arm64-android \
           fmt libogg libvorbis freetype physfs glm libpng zlib \
           sdl3 'sdl3-image[jpeg,png]' sdl3-ttf openal-soft openxr-loader
       find $VCPKG_ROOT/installed -name '*.pc' -exec sed -i -E 's/-lc\+\+//g' {} \;

   Add `curl` (and drop `-Pnetworking=false` below) if you want the add-on
   downloader.

2. Get the SDL Java glue matching the SDL version vcpkg built
   (`./tools/bootstrap-android-project.sh 3.4.16`, or copy
   `android-project/app/src/main/java/org/libsdl` from the SDL source in
   `$VCPKG_ROOT/buildtrees/sdl3/src/`), plus `gradlew` and
   `gradle/wrapper/gradle-wrapper.jar` from the same `android-project`.

3. Tell gradle where things are, in `mk/android/local.properties`:

       vcpkg_root=/path/to/vcpkg
       ndk_home=/path/to/Android/Sdk/ndk/<version>
       ndk_version=<version>
       openxr=true
       networking=false

4. Build the debug APK (signed with the debug key, installable on a
   headset in developer mode):

       cd mk/android
       ./gradlew assembleDebug -Pcpuarch=arm64-v8a

   `-Popenxr=true`, `-Pndk_version=...` and `-Pnetworking=false` can be
   passed on the command line instead of `local.properties`.

   Gradle compresses the 280 MB `data.zip` through the JVM temp dir; if
   `/tmp` is a small tmpfs, point it elsewhere:

       GRADLE_OPTS=-Djava.io.tmpdir=$HOME/tmp ./gradlew assembleDebug ... \
         "-Dorg.gradle.jvmargs=-Xmx1024m -Djava.io.tmpdir=$HOME/tmp"

   On machines with little RAM it is easier to build the native library
   outside gradle and let gradle only package it:

       cmake -S . -B build-quest -G Ninja \
         -DANDROID=ON -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-35 \
         -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release \
         -DVCPKG_MANIFEST_MODE=OFF -DENABLE_OPENGL=ON -DENABLE_OPENXR=ON \
         -DENABLE_NETWORKING=OFF
       cmake --build build-quest -j2
       mkdir -p mk/android/app/libs/arm64-v8a
       cp build-quest/libsupertux2.so mk/android/app/libs/arm64-v8a/
       cp $ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so \
          mk/android/app/libs/arm64-v8a/
       cd mk/android && ./gradlew assembleDebug -Pcpuarch=arm64-v8a -PEXCLUDE_NATIVE_LIBS=1

   (`ANDROID_NDK_HOME` and `VCPKG_ROOT` must be set for the CMake step.)

   Depending on the vcpkg version, SDL3's CMake config exports only
   `include/`, while the precompiled header includes `<SDL.h>`. If the
   build stops with `'SDL.h' file not found`, add the header directories
   to the configure line:

       -DCMAKE_CXX_FLAGS="-isystem $VCPKG_ROOT/installed/arm64-android/include/SDL3 \
         -isystem $VCPKG_ROOT/installed/arm64-android/include/SDL3_image \
         -isystem $VCPKG_ROOT/installed/arm64-android/include/SDL3_ttf"

5. Install on the headset (developer mode + USB debugging enabled):

       adb install -r app/build/outputs/apk/debug/app-debug.apk
       adb shell am start -n org.supertux.supertux2/.MainActivity

   The app shows up under *Library > Unknown Sources* on the headset.

## PICO notes

* PICO OS 5.9.0 or newer is required: only from that version does the
  PICO runtime work with the generic Khronos OpenXR loader that vcpkg
  builds. Older firmware needs PICO's own loader library instead.
* The manifest declares `pvr.app.type=vr` plus the
  `com.picovr.intent.category.VR` and
  `org.khronos.openxr.intent.category.IMMERSIVE_HMD` categories, which is
  what makes the PICO launcher start the app immersively rather than as a
  flat panel.
* Without the PICO bindings the game would fall back to
  `khr/simple_controller`, which has no thumbsticks and therefore no way
  to walk.

## Performance notes

* The GLES 3 path uses client-side vertex arrays instead of
  `glBufferData` per draw call (`GLVertexArrays::upload`). On the
  Quest's Adreno driver every buffer re-specification (and every
  `glMapBufferRange`) allocates GPU memory through an ioctl, which with a
  few hundred draws per eye per frame capped the game at ~24 FPS; with
  client arrays it runs at the full 72 FPS with ~8 ms frame time.
* Frame statistics can be read from the runtime with
  `adb logcat -s VrApi` (FPS, stale frames, App GPU time).
* `simpleperf record --app org.supertux.supertux2 -g` on the headset
  plus `simpleperf report --symfs` with the unstripped
  `build-quest/libsupertux2.so` gives a symbolized profile.

## Notes and limitations

* Split-screen multiplayer viewports are not adapted for VR; each
  player's viewport is drawn on the same virtual screen.
* The lightmap is applied as a screen-space layer at the tile depth, so
  light halos are slightly offset on far background layers.
* Screen-space effects (`fancy_gfx`: displacement, blur) are disabled in VR.
* Taking the headset off pauses the app; the OpenXR session is restarted
  when the app resumes.
* Use the Meta "Reset view" system action or the left thumbstick click to
  recenter the screen.
