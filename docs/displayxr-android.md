# DisplayXR on Android

PPSSPP can send its stereoscopic game output to DisplayXR's Android weave service. The separate
runtime handles panel calibration, eye tracking and weaving. PPSSPP presents the returned image in
its existing Android window. No Leia SDK or proprietary display plugin is included in PPSSPP.

This implementation targets 64-bit ARM Leia Android devices (including Lume Pad 2), Android 8.0/API 26
or newer, with buffered Vulkan hardware rendering, multiview and external-memory/semaphore support.
Detection requires the enabled OEM system package `com.leialoft.display.config`. Other DisplayXR
brands and graphics backends are not detected by this implementation. Regular Android devices retain
the existing output and receive no installation prompt. PPSSPP's overall minimum Android API stays 21.

The implementation has been built and its configuration policy tested, but physical output, tracking
and lifecycle recovery have not been verified on a Leia device. Treat it as experimental until the
hardware checklist below passes.

## Installation and settings

On an eligible device without DisplayXR, PPSSPP suggests installation once. **Installation guide**
opens the [official Android installer instructions](https://github.com/DisplayXR/displayxr-installer/blob/main/android-installer/README.md).
**Not now** dismisses the suggestion. The same guide remains in Graphics > Stereoscopy.
PPSSPP does not download or install another APK automatically.

Install and configure the Leia-capable DisplayXR runtime using that guide, including its first-run
setup and permissions. The expected package is
`org.freedesktop.monado.openxr_runtime.out_of_process`. The generic simulated-display runtime is
insufficient. Return to PPSSPP to trigger another background compatibility check.

With a compatible runtime and Vulkan device, **DisplayXR** appears first in **Stereoscopic 3D Mode**.
It becomes the global default once, tracked by `DisplayXRDefaultApplied` in the global `[Graphics]`
configuration. An explicit per-game mode is preserved, as are subsequent user selections. Existing
mode values 0–4 retain their meanings; DisplayXR uses value 5. Off, SBS and TAB remain selectable.
A previously selected DisplayXR mode stays visible if unavailable, so the user can retry it or switch
output. Its status and setup link are shown below the setting.

Camera permission is requested when a game first needs DisplayXR, with an explanation before the
Android permission dialog. Denial keeps the game in 2D. To try again, select DisplayXR again after
checking permissions. The runtime has its own setup and permission requirements.

Only active gameplay uses the physical 3D output. PPSSPP menus and paused screens use ordinary 2D.
Touch controls, OSD and the stereoscopy settings overlay are sent as a full-window mono overlay.
Existing separation, convergence, UI-depth and eye-swap controls still apply. The service's eye
tracking controls panel weaving; it does not replace PPSSPP's projection with head-tracked views.

While DisplayXR is selected and available, the Android surface uses native window resolution,
regardless of the saved hardware scaler setting. The saved value is retained for other modes.
The woven result is copied at the same pixel dimensions with no application-side rotation or scaling.
Window position, size and Android display ID are supplied to the runtime every submitted frame.

A runtime, buffer or session failure retains the saved mode and switches rendering to mono with an
OSD message. Resume, package installation/update, or explicit DisplayXR reselection retries setup.
Incompatible graphics settings leave DisplayXR unavailable. Persistent driver/device loss may still
require an app restart, as with ordinary Vulkan output.

## Rendering and synchronization

`android/jni/DisplayXR.cpp` dynamically loads the OpenXR loader only on eligible devices. A background
check verifies the vendor runtime and extension versions without creating a session or requesting
tracking at launch. On first use, the session also enumerates physical displays and rejects the
simulated-display plugin, and verifies that the runtime accepts PPSSPP's Vulkan physical device.

The final stereo composite is captured as half-SBS into a cached framebuffer. The normal backbuffer
is cleared to transparent for the mono UI. At presentation time, GPU copies transfer both images into
per-frame Android hardware buffers, then `xrWeaveSubmitDXR` returns a shared woven output. The runtime
owns calibration and interlacing; PPSSPP copies the woven pixels to its ordinary swapchain.

The submit acquire sync-file covers both input/overlay writes and the previous woven-output read on
the same graphics queue. The runtime release sync-file is imported as a temporary Vulkan semaphore
before reading the current output. Foreign queue-family ownership is transferred explicitly. Frame
fences protect imported buffers, command pools and capture framebuffers until their last GPU use.
This path uses GPU transfers, without per-frame CPU pixel readback or device-idle waits.

Android presentation callbacks travel with their submitted frame to the render thread. Activity
generations discard stale callbacks and probe results. Session teardown waits outstanding client
work and asks the panel to return to 2D. Render resources are destroyed before the Vulkan device.
Savestate formats and shader cache key layouts are unchanged.

## Dependencies and distribution

The Android Gradle dependency is Khronos's
[OpenXR loader 1.1.63](https://github.com/KhronosGroup/OpenXR-SDK-Source/releases/tag/release-1.1.63),
via `org.khronos.openxr:openxr_loader_for_android:1.1.63`. Its manifest minimum is overridden to retain
PPSSPP's API 21 compatibility; this integration only loads it on eligible API 26+ devices.

The two provisional DisplayXR extension headers are pinned to
[displayxr-runtime v2.70.6](https://github.com/DisplayXR/displayxr-runtime/tree/v2.70.6)
(commit `f80f3b7`), with provenance and the Apache-2.0 license in `android/jni/displayxr/`.
Runtime requirements are `XR_DXR_weave` version 20 or newer (Android acquire/release fences) and
`XR_DXR_display_info` version 22 or newer (physical-display enumeration). These extensions are still
provisional; future runtime changes may require updating the pinned headers and client together.

The PPSSPP APK can be distributed separately on GitHub: it contains the OpenXR loader and client,
not DisplayXR's runtime or Leia's proprietary libraries. Users install the runtime separately under
its distribution terms. Keep that separation when packaging releases. The APK carries the header and
loader license texts and notices under `assets/licenses/`.

## Validation

Host unit coverage (`PPSSPPUnitTest DisplayXRPolicy`) checks menu ordering, stable persisted values,
one-time global migration, explicit per-game overrides and later user choices. The ordinary Android
ABIs compile the optional bridge as a no-op outside arm64.

Before announcing working Lume Pad 2 support, test on real hardware using
[the Android device workflow](android-device-testing.md):

1. Install the APK and verify its version. On an ordinary phone, check launch, Vulkan/OpenGL gameplay,
   pause/resume, and SBS/TAB. Confirm that no DisplayXR prompt or camera request appears.
2. On the Leia device without a runtime, check the one-time suggestion and both buttons. Relaunch to
   confirm dismissal persists. Install the vendor runtime, finish setup, and return to PPSSPP.
3. Confirm DisplayXR is first and becomes the global default once. Test a pre-existing per-game Off,
   SBS or TAB override; change the global mode and relaunch to confirm the later choice persists.
4. Start a game, decline then grant camera permission via retry. Verify real depth, correct eye order,
   overlay alignment, touch input and readable mono UI. Move the viewer to check tracking.
5. Rotate, resize, enter multi-window, change immersive bars, and background/resume. Check interlace
   phase and native pixel alignment. Confirm menus/paused screens restore physical 2D.
6. Switch repeatedly between DisplayXR, Off, SBS and TAB, including the in-game overlay. Change the
   Android hardware scaler and verify its value is restored outside DisplayXR.
7. Stop/update/remove the runtime during gameplay, deny permissions, and use incompatible graphics
   settings. Check mono fallback, retained desired mode and successful retry without stuck frames.
8. Repeat activity recreation with Vulkan validation enabled; check buffer/fd lifetime, semaphore
   reuse, output ownership and retained activities. Compare frame pacing with ordinary stereo.

For diagnosing output on a device, two switches take effect on the next frame, set with
`adb shell setprop <name> 1` (and `0` to clear):

- `debug.ppsspp.dxr.passthrough`: show the half-SBS input PPSSPP submits instead of the woven output.
- `debug.ppsspp.dxr.nooverlay`: submit without the UI overlay.

DisplayXR's service captures what it receives when the file
`/storage/emulated/0/Android/data/org.freedesktop.monado.openxr_runtime.out_of_process/files/displayxr_atlas_trigger.org.ppsspp.ppsspp`
is created (`adb shell touch`), into the same directory. PPSSPP logs failing OpenXR calls under `DisplayXR:`
in logcat, and `Session created` and the woven output size when they happen.

The main regression risks are Vulkan external-buffer synchronization, device-specific hardware-buffer
formats, panel orientation/phase and activity/service lifetimes. Compile and policy tests cannot
establish that a particular vendor runtime produces correct physical 3D.
