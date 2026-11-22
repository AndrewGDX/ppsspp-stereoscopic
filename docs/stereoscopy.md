# Stereoscopic 3D

Stereoscopic 3D for a normal display: both eyes are rendered, and presented side by side or top and
bottom for a 3D TV, projector or headset that takes such a signal. Settings > Graphics > Stereoscopy,
per game. Separate from VR (`Common/VR`), which shares the single-pass stereo rendering but takes its
views from the headset.

Works with Vulkan, Metal and D3D11 (D3D11.3, `VPAndRTArrayIndexFromAnyShaderFeedingRasterizer`). Not
with OpenGL, software rendering, or skip buffer effects.

## Rendering

`StereoscopyEnabled()` (`PresentationCommon.cpp`) sets `GPU_USE_STEREOSCOPY`, `GPU_USE_SINGLE_PASS_STEREO`
and `GPU_USE_FRAMEBUFFER_ARRAYS`. Every framebuffer then has two layers (`GetFramebufferLayers()`), a
layer per eye, and every draw renders both in one pass:

| Backend | How a draw reaches both layers | The eye in the shaders |
|---|---|---|
| Vulkan | Multiview render passes | `gl_ViewIndex` |
| Metal | An instance per layer, the vertex shader writes `[[render_target_array_index]]` (SPIRV-Cross's multiview emulation) | `gl_ViewIndex` (from the instance, `METAL_VIEW_MASK_INDEX`) |
| D3D11 | An instance per layer, the vertex shader writes `SV_RenderTargetArrayIndex` | `SV_InstanceID`, passed to the pixel shader in a varying |

Shaders that sample a framebuffer sample their eye's layer (`FS_BIT_STEREO`, `ShaderWriterFlags::FS_AUTO_STEREO`).
Copies copy both layers. CPU readbacks read the left eye.

On Metal and D3D11, a thin3d vertex shader gets a second version that picks the layer, used for a layered
target or when the fragment shader reads the eye. On D3D11, a texture slot gets a 2D or array view to match
what the pixel shader declares, and binding one layer of a stereo framebuffer for a 2D sampler copies it out.

Turning stereoscopy on or off recreates the framebuffers (`FramebufferManagerCommon::NotifyLayersChanged`),
so their contents are lost for a frame. The other settings are uniforms and take effect right away.

## The shift

After the vertex shader converts to clip space, each eye moves along x:

```
clip_x += eyeScale * (w - convergence)
```

`eyeScale` is minus the separation for the left eye and plus for the right, or 0 and twice the separation
with a dominant eye. Separation is the setting / 1000, in clip space (a share of the render target's half
width). For a convergence setting c (limited to 0..200), the convergence distance is
`c / 10 + 2980 * (c / 200)^4`, in the game's own w units (0..3000). The final per-eye shift is clamped
to ±0.2 NDC after dominant-eye scaling. At w = convergence the eyes agree (screen depth). Detected UI
uses the configured UI or Second Layer
depth exactly, without a contribution from its original Z: `uiDepth * w`.

## UI detection

Each draw gets a class on the CPU (`ClassifyStereoDraw` in `ShaderUniforms.cpp`), passed as uniforms, so
it costs no shader variants:

- **Flat** (not shifted): clears, 2D draws of framebuffers (post effects, compositing, already stereo), and
  with detection off, all through mode and orthographic draws.
- **UI**: through mode draws and orthographic projections (constant w).
- **Crosshair**: with Crosshair Depth not 0, a UI draw at most 64 pixels across whose screen bounding box
  lies within 64 pixels of the PSP screen's center horizontally and 48 vertically, in a render target at
  least 480x272, while a World draw happened in this host frame or the last (a 30 FPS game can draw a frame
  across two). `DecodeVerts` finds the box (`stereoCentered2D`), for orthographic draws through the
  world-view-projection and viewport. It's at the UI depth plus the Crosshair Depth (the sum clamped to
  ±100), so 0 leaves it with the UI. Peace Walker's aiming reticle is four dots, each an orthographic quad
  of a few pixels; its 64x64 blur buffers are what the render target size rule keeps out.
- **Second layer**: a through mode draw covering at least 90% of the render target, like a menu background
  (`stereoFullscreen2D`, from the software transform). In Advanced mode, also 3D draws without depth
  test and depth write. These Advanced-only draw detections follow Second Layer Parallax.
  Safe UI keeps its usual UI depth.
- **World**: everything else, shifted by depth. In Advanced mode, world vertices with clip w below
  UI Detection Threshold (in the game's w units) use the configured UI depth exactly, in the vertex
  shader.

## Presentation

`GetStereoscopyLayout()` lays each eye out in its own viewport: half modes squeeze each eye into half the
frame, full modes keep its aspect ratio (two 16:9 eyes side by side make 32:9, letterboxed). Each eye is
laid out like a mono image would be in the whole frame, so stretching, integer scaling and pillarboxing work
per eye. Swap eyes swaps the layers there. A mono source (a frame from RAM) is shown to both eyes.

Post shaders run on both eyes together: the eyes are first composited side by side (or top and bottom) at
render resolution, cropped and swapped, into `stereoComposite_`, the chain runs on that with its framebuffers
twice as wide (or tall), and each eye then shows its half. So a shader sees one image twice the size, and
effects that depend on the position in the frame (vignette) span both eyes, while ones that sample neighbours
only bleed across the seam by their radius. The overlay and Cardboard VR are off while stereoscopy is on.

Auto render resolution sizes each eye for its frame: the full modes halve it, the half modes don't (their eye
is squeezed along one axis only, and keeps the full resolution along the other).

## Settings overlay

`UI/StereoscopyOverlay.cpp` changes the settings in game, without pausing it. It opens and closes with the
`Stereoscopy overlay` hotkey (Emulator controls; Ctrl+Shift+F1, or L1+L2 on XInput and SDL pads) or Game
Settings > Stereoscopy Overlay in the desktop menus. L2 alone pauses, so it has to go down last. The hotkey
keeps its default in a `controls.ini` from before it existed (`KeepsDefaultIfMissing` in `KeyMap.cpp`).

- **Input.** The overlay takes the keys and axes mapped to the PSP's buttons (D-pad and left stick move and
  change, the confirm button cycles a choice, Triangle restores the default, the cancel button closes), plus
  the arrows, Enter and Escape. The control mapper still gets everything, as it has to see the hotkey, but
  EmuScreen drops button presses, sticks and virtual keys other than the hotkey while it's open, in the
  mapper's callbacks. Dropping them there rather than when queued vkeys are processed matters: Escape both
  closes the overlay and maps to Pause, and the overlay is already closed by the time the queue runs.
- **Changes** apply right away (the mode posts `GPU_CONFIG_CHANGED`, the rest are uniforms). The config is
  saved once, on close, if anything changed, to the game's ini when it has one. The header says which.
- **Drawing.** The panel background is 25% transparent. It's laid out for the whole screen in UI units, then drawn once per eye with that eye's
  viewport (`GetStereoscopyEyeViewports`), so the display's stretch of a half mode eye gives it the right
  aspect. Both eyes get the same image, which puts it at screen depth regardless of the UI depth. Touches are
  mapped back through the eye they land in. Without stereoscopy it's drawn once.
- **Position** (`StereoscopyOverlayPosition`, global): center left or right is a column, top or bottom center
  a strip with the settings in two columns. Everything scales down together if it doesn't fit.

## Performance

Each draw is one instanced draw call for both eyes, so the CPU cost is small: about 3% more per frame on a
Radeon Pro Vega 20 (Tekken 6, 4x). The GPU does all the work twice, every pixel of every layer, so GPU time
doubles (1.2 to 2.0 ms there). That's what limits it on integrated GPUs, which also share their power budget
with the CPU: on the Intel UHD 630 of the same Mac, GPU time went from 6.4 to 10.6 ms a frame, and the
emulator thread slowed down too, by 60% and more, doing the same work. With Buffer graphics commands set to
No buffer, the CPU and the GPU take turns (on Metal there's no render thread then, and each frame waits for
the GPU), so a frame costs their sum, and with vsync, one that misses 16.7 ms can wait for the next
vblank, which halves the frame rate. Buffering one or two frames lets them overlap. A lower render
resolution cuts the GPU time the most.

## Testing

- With separation 0, the left eye must match mono exactly. Headless screenshots are the left eye, so
  `--appendconfig` with `StereoscopyMode = 1` and `StereoscopySeparation = 0` against a mono run, on each
  backend, checks the whole layered path. The frame dumps too (`frametests.py --variants=...`).
- The right eye and the layout need the real app: a backbuffer screenshot through the WebSocket debugger
  (`wsdbg`'s `:screenshot`). Use a memstick of its own (`--memstick`), as the app saves appended settings.
  `--appendconfig` skips `[PostShaderList]`, so for post shaders, write that memstick's `ppsspp.ini` instead.
- `ShaderGenerators` has a stereoscopy pass (Vulkan GLSL, and HLSL on Windows).
- The debugger's screenshot is taken before the UI is drawn, so it doesn't show the settings overlay.
  `ScheduleScreenshot` with `ScreenshotType::Output` does, as it reads the backbuffer at the end of the frame.
  Feeding `NativeKey`/`NativeAxis` from a local hook in `EmuScreen::update` drives the hotkey through the mapper.

## Known issues

- **AMD GPUs in Intel Macs lose depth writes to layered targets.** On a Radeon Pro Vega 20 (macOS 15), 9 of
  the 43 frame dumps differ from mono with separation 0: a scattering of pixels (a different set each run)
  keeps its old depth after a clear-mode draw, and later depth tests fail (FlatOut loses the car). Metal
  and Vulkan through MoltenVK fail the same way, as did the developer option this replaced, while the
  Intel UHD 630 in the same Mac matches mono in all 43. Clearing each slice in a pass of its own, keeping
  depth loaded and stored, a depth format without stencil and turning off lossless compression didn't help.
  Games tested in play rendered correctly.
