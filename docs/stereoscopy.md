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
with a dominant eye. Separation is the setting / 3000, in clip space (a share of the render target's half
width). Convergence is in the game's own w units, which differ a lot between games. At w = convergence the
eyes agree (screen depth). UI goes at a fixed depth instead: `uiDepth * w`, at most the separation either way.

## UI detection

Each draw gets a class on the CPU (`ClassifyStereoDraw` in `ShaderUniforms.cpp`), passed as uniforms, so
it costs no shader variants:

- **Flat** (not shifted): clears, 2D draws of framebuffers (post effects, compositing, already stereo), and
  with detection off, all through mode and orthographic draws.
- **UI**: through mode draws and orthographic projections (constant w). Also 3D draws without depth test
  and depth write, in Advanced mode.
- **Second layer**: a through mode draw covering at least 90% of the render target, like a menu background
  (`stereoFullscreen2D`, from the software transform).
- **World**: everything else, shifted by depth. In Unsafe and Advanced modes, vertices nearer than the
  threshold (as a share of the convergence distance) go at the UI depth, in the vertex shader.

## Presentation

`GetStereoscopyLayout()` lays each eye out in its own viewport: half modes squeeze each eye into half the
frame, full modes keep its aspect ratio (two 16:9 eyes side by side make 32:9, letterboxed). Each eye is
laid out like a mono image would be in the whole frame, so stretching, integer scaling and pillarboxing work
per eye. Swap eyes swaps the layers there. A mono source (a frame from RAM) is shown to both eyes.

Post shaders, the overlay and Cardboard VR are off while stereoscopy is on.

## Testing

- With separation 0, the left eye must match mono exactly. Headless screenshots are the left eye, so
  `--appendconfig` with `StereoscopyMode = 1` and `StereoscopySeparation = 0` against a mono run, on each
  backend, checks the whole layered path. The frame dumps too (`frametests.py --variants=...`).
- The right eye and the layout need the real app: a backbuffer screenshot through the WebSocket debugger
  (`wsdbg`'s `:screenshot`). Use a memstick of its own (`--memstick`), as the app saves appended settings.
- `ShaderGenerators` has a stereoscopy pass (Vulkan GLSL, and HLSL on Windows).

## Known issues

- **AMD GPUs in Intel Macs lose depth writes to layered targets.** On a Radeon Pro Vega 20 (macOS 15), 9 of
  the 43 frame dumps differ from mono with separation 0: a scattering of pixels (a different set each run)
  keeps its old depth after a clear-mode draw, and later depth tests fail (FlatOut loses the car). Metal
  and Vulkan through MoltenVK fail the same way, as did the developer option this replaced, while the
  Intel UHD 630 in the same Mac matches mono in all 43. Clearing each slice in a pass of its own, keeping
  depth loaded and stored, a depth format without stencil and turning off lossless compression didn't help.
  Games tested in play rendered correctly.
