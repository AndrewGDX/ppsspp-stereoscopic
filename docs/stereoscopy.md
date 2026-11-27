# Stereoscopic 3D

Stereoscopic 3D for a normal display: both eyes are rendered, and presented side by side or top and
bottom, or interleaved by row or column, for a 3D TV, projector or headset that takes such a signal.
Settings > Graphics > Stereoscopy, per game. Separate from VR (`Common/VR`), which shares the single-pass stereo rendering but takes its
views from the headset.

Android also has an optional [DisplayXR output](displayxr-android.md) for glasses-free 3D on
DisplayXR-supported devices. It requires Vulkan and a separately installed runtime.

Works with OpenGL, OpenGL ES, Vulkan, Metal and D3D11 (D3D11.3,
`VPAndRTArrayIndexFromAnyShaderFeedingRasterizer`). Requires buffered hardware rendering.

## Rendering

`StereoscopyEnabled()` (`PresentationCommon.cpp`) enables a framebuffer layer per eye
(`GetFramebufferLayers()`). Vulkan, Metal and D3D11 set `GPU_USE_SINGLE_PASS_STEREO` and
`GPU_USE_FRAMEBUFFER_ARRAYS` and submit one draw for both layers. OpenGL gives each eye a 2D framebuffer and
runs each render pass once per eye (see [OpenGL](#opengl)).

| Backend | How a draw reaches both layers | The eye in the shaders |
|---|---|---|
| Vulkan | Multiview render passes | `gl_ViewIndex` |
| Metal | An instance per layer, the vertex shader writes `[[render_target_array_index]]` (SPIRV-Cross's multiview emulation) | `gl_ViewIndex` (from the instance, `METAL_VIEW_MASK_INDEX`) |
| D3D11 | An instance per layer, the vertex shader writes `SV_RenderTargetArrayIndex` | `SV_InstanceID`, passed to the pixel shader in a varying |
| OpenGL / OpenGL ES | The queue runner runs the render pass again for the right eye's framebuffer | `u_stereoEye`, a uniform set when the program is bound |

Shaders that sample a framebuffer sample their eye's layer (`FS_BIT_STEREO`, `ShaderWriterFlags::FS_AUTO_STEREO`).
Copies copy both layers. CPU readbacks read the left eye.

On Metal and D3D11, a thin3d vertex shader gets a second version that picks the layer, used for a layered
target or when the fragment shader reads the eye. On D3D11, a texture slot gets a 2D or array view to match
what the pixel shader declares, and binding one layer of a stereo framebuffer for a 2D sampler copies it out.

### OpenGL

GL has no common way for one draw to reach both eyes. `OVR_multiview2` is GLES only (and not on every
GPU), and a vertex shader can only pick the layer with `ARB_shader_viewport_layer_array` or
`AMD_vertex_shader_layer`, which macOS's GL 4.1 doesn't have. So `GLQueueRunner::PerformRenderPass` runs a
stereo pass's commands twice, into the left eye's framebuffer and then the right's, and the target changes
once per eye. Changing it for every draw made a frame 3.6 times as slow on a Radeon Pro Vega 20 (the driver
ends and starts a pass on each change), and tiled GPUs would store and load the whole target each time.

The right eye's run has to start from the state the left eye's did. GL keeps state from pass to pass, and
the callers count on that: `TextureCacheGLES` doesn't bind a texture again, and `ShaderManagerGLES` keeps its
program and only sets the uniforms that changed. So:

- The render manager starts a stereo pass by binding the program and textures that are bound
  (`boundProgram_`, `boundTextures_`).
- Once there's been a stereo pass, the queue runner saves every uniform it sets in its program
  (`GLRProgram::uniformValues`), and the left eye's run notes what it overwrites, for the right eye's run to
  set back. Setting every uniform again in each pass did the same, and cost about 15% in Peace Walker.
- A framebuffer bound with all layers reads the eye's own texture, and layer 1 the right eye's.

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
  across two), and while the center isn't busy: fewer than 4 other 2D draws (not full screen) reaching
  within 120x80 pixels of it, in this host frame so far and in the last one that drew something
  (`stereoNearCenter2D`), so a menu drawn after its icons counts too. Peace Walker's HUD has none there,
  its inventory about 30, with icons and text in the crosshair's area. `DecodeVerts` finds the box
  (`stereoBounds2D`), for orthographic draws through the world-view-projection and viewport, from the
  draws in the batch that cover an area: Peace Walker draws each part of a rifle's crosshair with five
  rectangles at 0,0 whose vertex data is all zero, merged into its batch, which made the crosshair's own
  parts count as busy. It's at the UI depth plus the Crosshair Depth (the sum clamped to
  ±100), so 0 leaves it with the UI. Peace Walker's aiming reticle is four dots, each an orthographic quad
  of a few pixels; its 64x64 blur buffers are what the render target size rule keeps out.
- **Probe** and **probe copy** (`ClassifyStereoProbe`): lens flares measure how much of the sun is hidden
  with a probe. Peace Walker copies the 8x8 pixels at the sun into a scratch buffer, draws black and then
  white over them with a depth test at the sun's depth, copies the result out and shrinks it to a pixel
  (the flare's brightness, and a glare over the whole frame), and puts the saved pixels back. Shifted like
  UI, the squares showed beside the restored pixels, which aren't shifted, and the eyes measured the wrong
  pixels. So a through mode copy of at most 64x64 texels out of a framebuffer at least 480x272, pixel for
  pixel, records the area (`stereoProbeAreas`, for the host frame). Midnight Club keeps its bloom as a
  64x38 image in the corner of a full size framebuffer and stretches it over the screen: shifting where
  that samples moved the bloom by the parallax times the stretch. A 2D draw exactly over a recorded area (within a pixel)
  is a probe, at the far distance (UI depth 1), and a copy from inside one samples it shifted by the same
  amount without moving (`u_stereoUI.z`: the texture coordinates move by half the clip space shift, as the
  source's width in texels is the width the clip space spans twice). Each eye then probes, measures and
  restores where it sees the sun. The far distance is where suns are (Peace Walker's is 98% of the way);
  a probe for something nearer measures beside it by the difference in parallax.
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
per eye. Row-Interleaved and Column-Interleaved use the full frame for each eye, selecting alternating physical
output pixels in the final presentation shader. Even rows (from the top) or columns (from the left)
show the left eye; Swap Eyes reverses this. The pattern is anchored to the output, including when the
game is letterboxed, and follows output resolution rather than render resolution. Display scaling
after presentation can disturb the pattern; use the display's native resolution.

Swap eyes swaps the layers there. A mono source (a frame from RAM) is shown to both eyes.

Post shaders run on both eyes together: the eyes are first composited side by side (or top and bottom) at
render resolution, cropped and swapped, into `stereoComposite_` (side by side for interleaved output).
The chain runs on that with its framebuffers twice as wide (or tall), and each eye then shows its half. So a shader sees one image twice the size, and
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
- **Selection** stays in RAM across overlay closes and game changes until the emulator exits. A disabled
  remembered row moves to the next enabled row. It is never saved to config. In interleaved modes the
  panel is drawn once at full size, giving both eyes the same screen-depth image.
- **Position** (`StereoscopyOverlayPosition`, global): center left or right is a column, top or bottom center
  a strip with the settings in two columns. Everything scales down together if it doesn't fit.

## Performance

On the single-pass backends each draw is one draw call for both eyes, so the CPU cost is small: about 3%
more per frame on a Radeon Pro Vega 20 (Tekken 6, 4x). The GPU does all the work twice, every pixel of every
layer, so GPU time doubles (1.2 to 2.0 ms there). That's what limits it on integrated GPUs, which also share their power budget
with the CPU: on the Intel UHD 630 of the same Mac, GPU time went from 6.4 to 10.6 ms a frame, and the
emulator thread slowed down too, by 60% and more, doing the same work. With Buffer graphics commands set to
No buffer, the CPU and the GPU take turns (on Metal there's no render thread then, and each frame waits for
the GPU), so a frame costs their sum, and with vsync, one that misses 16.7 ms can wait for the next
vblank, which halves the frame rate. Buffering one or two frames lets them overlap. A lower render
resolution cuts the GPU time the most.

OpenGL makes every draw twice, and the GL driver's time per draw is what its render thread spends most of
(`glDrawRangeElements`). On the Vega 20 at 4x, running 9 emulated seconds unthrottled took 3.4 seconds instead
of 3.1 in Tekken 6. Peace Walker, with more draws and passes, took about 40% longer (in runs alternated with
mono on a busy machine).

## Testing

- With separation 0, the left eye must match mono exactly. Headless screenshots are the left eye, so
  `--appendconfig` with `StereoscopyMode = 1` and `StereoscopySeparation = 0` against a mono run, on each
  backend, checks the whole layered path. The frame dumps too (`frametests.py --variants=...`).
- The right eye and the layout need the real app: a backbuffer screenshot through the WebSocket debugger
  (`wsdbg`'s `:screenshot`). Use a memstick of its own (`--memstick`), as the app saves appended settings.
  `--appendconfig` skips `[PostShaderList]`, so for post shaders, write that memstick's `ppsspp.ini` instead.
- On OpenGL, the right eye of every frame dump can be checked with a local change that makes
  `GLQueueRunner::PerformReadback` read `rightEye`: with separation 0 it matches mono exactly. Without the
  uniform undo above, 3 of the 43 differ (OutRun's water, and two Valkyria Chronicles scenes).
- `ShaderGenerators` has a stereoscopy pass (OpenGL and Vulkan GLSL, and HLSL on Windows).
- The debugger's screenshot is taken before the UI is drawn, so it doesn't show the settings overlay.
  `ScheduleScreenshot` with `ScreenshotType::Output` does, as it reads the backbuffer at the end of the frame.
  Feeding `NativeKey`/`NativeAxis` from a local hook in `EmuScreen::update` drives the hotkey through the mapper.

## Known issues

- **AMD GPUs in Intel Macs corrupt single-sampled layered depth.** On a Radeon Pro Vega 20
  (macOS 15), Metal and Vulkan through MoltenVK leave holes in draws that use depth, even with
  separation 0. In 9 of 43 frame dumps, a scattering of pixels (a different set each run) keeps its
  old depth after a clear-mode draw, so later depth tests fail (FlatOut loses the car). The earlier
  developer option showed the same problem. The Intel UHD 630 in the same Mac matches mono in all
  43 dumps.

  In play, Midnight Club: L.A. Remix at native (1x) rendering resolution stretches these holes
  through its low-resolution effects into large blocks. The same frame renders correctly in mono,
  on the Intel UHD 630, or with 2x MSAA. Clearing each slice in a separate pass, keeping depth loaded
  and stored, using a depth format without stencil, disabling lossless compression, writing fragment
  depth explicitly, and padding the framebuffer height did not fix the corruption.

  A tested workaround forced at least 2x MSAA for Midnight Club in stereo at 1x resolution on AMD
  Macs. A driver bug flag (`BROKEN_SINGLE_SAMPLE_LAYERED_DEPTH`) and per-game `StereoMSAA` compat
  flag selected it without changing the saved MSAA setting. The effective sample count was checked
  each frame, and changing it recreated the framebuffers. Small effect targets stayed on the usual
  single-sampled path (`ShouldMultisample`). This avoided the visible corruption, at the cost of GPU
  time, memory, and antialiasing the scene. It was limited to that game because forcing MSAA for all
  stereo games introduced an effects glitch in Final Fantasy Type-0 on Metal.

  The automatic workaround and its flags are not included. The AMD Mac issue remains open;
  selecting 2x MSAA manually avoided it in the tested Midnight Club scene, but can change effects
  in other games.
