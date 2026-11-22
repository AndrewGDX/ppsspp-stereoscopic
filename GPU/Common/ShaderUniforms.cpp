#include <algorithm>
#include <cmath>
#include <limits>

#include "ShaderUniforms.h"
#include "Common/System/Display.h"
#include "Common/Data/Convert/SmallDataConvert.h"
#include "Common/Math/lin/matrix4x4.h"
#include "Common/Math/math_util.h"
#include "Common/Math/CrossSIMD.h"
#include "Common/Math/lin/vec3.h"
#include "Common/TimeUtil.h"
#include "Common/VR/PPSSPPVR.h"
#include "Core/Config.h"
#include "GPU/GPUState.h"
#include "GPU/Common/FramebufferManagerCommon.h"
#include "GPU/Common/GPUStateUtils.h"
#include "GPU/Common/TransformCommon.h"
#include "GPU/Math3D.h"

using namespace Lin;

void UpdateRotation(float rotMatrix[4], bool useBufferedRendering) {
	if (!gstate_c.Use(GPU_USE_PRE_ROTATION) || useBufferedRendering) {
		rotMatrix[0] = 1.0f;
		rotMatrix[1] = 0.0f;
		rotMatrix[2] = 0.0f;
		rotMatrix[3] = 1.0f;
	} else {
		const DisplayRotation rotation = g_display.rotation;
		// The others are ROTATE_90 and so on.
		rotMatrix[0] = rotation == DisplayRotation::ROTATE_0 ? 1.0 : (rotation == DisplayRotation::ROTATE_180 ? -1.0 : 0.0);
		rotMatrix[1] = rotation == DisplayRotation::ROTATE_90 ? 1.0 : (rotation == DisplayRotation::ROTATE_270 ? -1.0 : 0.0);
		rotMatrix[2] = rotation == DisplayRotation::ROTATE_270 ? 1.0 : (rotation == DisplayRotation::ROTATE_90 ? -1.0 : 0.0);
		rotMatrix[3] = rotation == DisplayRotation::ROTATE_0 ? 1.0 : (rotation == DisplayRotation::ROTATE_180 ? -1.0 : 0.0);
	}
}

// The part of a video texture the frame fills: as much as was written there (see
// TextureCacheCommon::ApplyTextureFinish), or 480x272 if that isn't known. The VSH's UMD Video
// player converts 480 rows, and its strips wrap around a texture they fill the width of.
int VideoFrameWidth() {
	return gstate_c.videoColumns > 0 ? gstate_c.videoColumns : 480;
}

int VideoFrameHeight() {
	return gstate_c.videoRows > 0 ? gstate_c.videoRows : 272;
}

bool VideoClampsS() {
	return gstate.getTextureWidth(0) > VideoFrameWidth();
}

bool VideoClampsT() {
	return gstate.getTextureHeight(0) > VideoFrameHeight();
}

void CalcTexClamp(float texClamp[4], float texClampOffset[2]) {
	const float invW = 1.0f / (float)gstate_c.curTextureWidth;
	const float invH = 1.0f / (float)gstate_c.curTextureHeight;
	int w = gstate.getTextureWidth(0);
	int h = gstate.getTextureHeight(0);
	if (gstate_c.textureIsVideo) {
		// Only the video frame itself, whatever is in memory around it may be garbage.
		w = std::min(w, VideoFrameWidth());
		h = std::min(h, VideoFrameHeight());
	}

	// First wrap xy, then half texel xy (for clamp.)
	texClamp[0] = (float)w * invW;
	texClamp[1] = (float)h * invH;
	texClamp[2] = invW * 0.5f;
	texClamp[3] = invH * 0.5f;
	texClampOffset[0] = gstate_c.curTextureXOffset * invW;
	texClampOffset[1] = gstate_c.curTextureYOffset * invH;
}

static void UpdateStereoUniforms(UB_VS_FS_Base *ub);

void BaseUpdateUniforms(UB_VS_FS_Base *ub, uint64_t dirtyUniforms, bool useBufferedRendering, bool pixelMapped) {
	if (dirtyUniforms & DIRTY_TEXENV) {
		Uint8x3ToFloat3(ub->texEnvColor, gstate.texenvcolor);
	}
	if (dirtyUniforms & DIRTY_ALPHACOLORREF) {
		ub->alphaColorRef = gstate.getColorTestRef() | ((gstate.getAlphaTestRef() & gstate.getAlphaTestMask()) << 24);
	}
	if (dirtyUniforms & DIRTY_ALPHACOLORMASK) {
		ub->colorTestMask = gstate.getColorTestMask() | (gstate.getAlphaTestMask() << 24);
	}
	if (dirtyUniforms & DIRTY_FOGCOLOR) {
		Uint8x3ToFloat3(ub->fogColor, gstate.fogcolor);
	}
	if (dirtyUniforms & DIRTY_SHADERBLEND) {
		Uint8x3ToFloat3(ub->blendFixA, gstate.getFixA());
		Uint8x3ToFloat3(ub->blendFixB, gstate.getFixB());
	}
	if (dirtyUniforms & DIRTY_TEXCLAMP) {
		CalcTexClamp(ub->texClamp, ub->texClampOffset);
	}

	if (dirtyUniforms & DIRTY_MIPBIAS) {
		float mipBias = (float)gstate.getTexLevelOffset16() * (1.0 / 16.0f);
		ub->mipBias = (mipBias + 0.5f) / (float)(gstate.getTextureMaxLevel() + 1);
		// For GPU_USE_SHADER_LOD_BIAS. The sampler applies the bias only in auto mode, see GetSamplingParams.
		ub->texLodBias = gstate.getTexLevelMode() == GE_TEXLEVEL_MODE_AUTO ? mipBias : 0.0f;
	}

	if (dirtyUniforms & DIRTY_PROJMATRIX) {
		CopyMatrix4x4(ub->proj, gstate.projMatrix);
		ub->rotation = useBufferedRendering ? 0 : (float)g_display.rotation;
	}

	if (dirtyUniforms & DIRTY_FRAMEBUFFER_DIM) {
		ub->xywh[0] = (float)gstate_c.curRTOffsetX;
		ub->xywh[1] = (float)gstate_c.curRTOffsetY;
		ub->xywh[2] = (float)(2.0 / gstate_c.curRTWidth);  // intentionally do double precision here.
		ub->xywh[3] = (float)(2.0 / gstate_c.curRTHeight);

		ub->rotation = useBufferedRendering ? 0 : (float)g_display.rotation;
	}

	if (dirtyUniforms & DIRTY_RASTER_OFFSET) {
		ub->rasterOffset[0] = gstate.getOffsetX();
		ub->rasterOffset[1] = gstate.getOffsetY();
		ub->minZmaxZ[0] = (float)gstate.getDepthRangeMin();
		ub->minZmaxZ[1] = (float)gstate.getDepthRangeMax();
		// test sine wave
		// ub->minZmaxZ[0] = (sin(time_now_d()) * 0.5f + 0.5f) * 65536.0;
	}

	if (dirtyUniforms & DIRTY_VIEWPORT_UNIFORMS) {
		Vec4F32 vpScale = Vec4F32::LoadF24x4(&gstate.viewportxscale);
		Vec4F32 vpOffset = Vec4F32::LoadF24x4(&gstate.viewportxcenter);
		vpScale.Store(ub->vpScale);
		vpOffset.Store(ub->vpOffset);
		if (gstate_c.tolerantDepthEqual) {
			// A multiple of the 1/8 the vertex shader snaps Z to, so it moves every vertex the same.
			ub->vpOffset[2] += tolerantDepthEqualOffset;
		}
		ub->NaN = std::numeric_limits<float>::quiet_NaN();  // Used in the shader for range culling.
	}

	// Transform
	if (dirtyUniforms & DIRTY_WORLDMATRIX) {
		// TODO: We could change the shader to directly read these "malformed" matrices, but we'd
		// be doing the matrix multiplication manually.
		ConvertMatrix4x3To3x4Transposed(ub->world, gstate.worldMatrix);
	}
	if (dirtyUniforms & DIRTY_VIEWMATRIX) {
		ConvertMatrix4x3To3x4Transposed(ub->view, gstate.viewMatrix);
	}
	if (dirtyUniforms & DIRTY_TEXMATRIX) {
		ConvertMatrix4x3To3x4Transposed(ub->tex, gstate.tgenMatrix);
	}

	if (dirtyUniforms & DIRTY_FOGCOEF) {
		UpdateFogCoef(gstate, ub->fogCoef);
	}

	if (dirtyUniforms & DIRTY_TEX_ALPHA_MUL) {
		bool doTextureAlpha = gstate.isTextureAlphaUsed();
		if (gstate_c.textureSolidAlpha && gstate.getTextureFunction() != GE_TEXFUNC_REPLACE) {
			doTextureAlpha = false;
		}
		ub->texNoAlpha = doTextureAlpha ? 0.0f : 1.0f;
		ub->texMul = gstate.isColorDoublingEnabled() ? 2.0f : 1.0f;
	}

	if (dirtyUniforms & DIRTY_STENCILREPLACEVALUE) {
		ub->stencilReplaceValue = (float)gstate.getStencilTestRef() * (1.0 / 255.0);
	}

	// Note - this one is not in lighting but in transformCommon as it has uses beyond lighting
	if (dirtyUniforms & DIRTY_MATAMBIENTALPHA) {
		Uint8x3ToFloat4_AlphaUint8(ub->matAmbient, gstate.materialambient, gstate.getMaterialAmbientA());
	}

	if (dirtyUniforms & DIRTY_COLORWRITEMASK) {
		ub->colorWriteMask = ~((gstate.pmska << 24) | (gstate.pmskc & 0xFFFFFF));
	}

	// Texturing
	if (dirtyUniforms & DIRTY_UVSCALEOFFSET) {
		UpdateUVScaleOff(gstate, ub->uvScaleOffset);
	}

	if (dirtyUniforms & DIRTY_DEPAL) {
		ub->depal_mask_shift_off_fmt = PackDepalBits(pixelMapped);
	}

	if (dirtyUniforms & DIRTY_STEREO) {
		UpdateStereoUniforms(ub);
	}
}

bool GuessVRDrawingHUD(bool is2D, bool flatScreen) {

	bool hud = true;
	//HUD shouldn't be modified in nonVR mode
	if (IsBigScreenVRMode()) hud = false;
	//HUD can be disabled in settings
	else if (!g_Config.bRescaleHUD) hud = false;
	//HUD cannot be rendered in flatscreen
	else if (flatScreen) hud = false;
	//HUD has to be 2D
	else if (!is2D) hud = false;
	//HUD has to be blended
	else if (!gstate.isAlphaBlendEnabled()) hud = false;
#ifdef __LIBRETRO__
	// Untextured transition fades must cover the view, not be scaled with the HUD.
	else if (!gstate.isTextureMapEnabled()) hud = false;
#endif
	//HUD cannot be rendered with clear color mask
	else if (gstate.isClearModeColorMask()) hud = false;
	//HUD cannot be rendered with depth color mask
	else if (gstate.isClearModeDepthMask()) hud = false;
	//HUD texture has to contain alpha channel
	else if (!gstate.isTextureAlphaUsed()) hud = false;
	//HUD texture cannot be in 5551 format
	else if (gstate.getTextureFormat() == GETextureFormat::GE_TFMT_5551) hud = false;
	//HUD texture cannot be in CLUT16 format
	else if (gstate.getTextureFormat() == GETextureFormat::GE_TFMT_CLUT16) hud = false;
	//HUD texture cannot be in CLUT32 format
	else if (gstate.getTextureFormat() == GETextureFormat::GE_TFMT_CLUT32) hud = false;
	//HUD cannot have full texture alpha
	else if (gstate_c.textureSolidAlpha && gstate.getTextureFormat() != GETextureFormat::GE_TFMT_CLUT4) hud = false;
	//HUD must have full vertex alpha
	else if (!gstate_c.vertexFullAlpha && gstate.getDepthTestFunction() == GE_COMP_NEVER) hud = false;
	//HUD cannot render FB screenshot
	else if (gstate_c.curTextureHeight % 68 <= 1) hud = false;
	//HUD cannot be rendered with add function
	else if (gstate.getTextureFunction() == GETexFunc::GE_TEXFUNC_ADD) hud = false;
	//HUD cannot be rendered with replace function
	else if (gstate.getTextureFunction() == GETexFunc::GE_TEXFUNC_REPLACE) hud = false;
	//HUD cannot be rendered with full clear color mask
	else if ((gstate.getClearModeColorMask() == 0xFFFFFF) && (gstate.getColorMask() == 0xFFFFFF)) hud = false;

	return hud;
}

void VRUpdateUniforms(UB_VS_FS_Base *base, UB_VS_VR *vr, uint64_t dirtyUniforms) {
	// Also counts the 3D draws that decide whether the scene is 3D, so once per draw.
	const bool is2D = Is2DVRObject(gstate.projMatrix, gstate.isModeThrough());
	const bool flatScreen = IsFlatVRScene();

	if (GuessVRDrawingHUD(is2D, flatScreen)) {
		float aspect = 480.0f / 272.0f * (IsImmersiveVRMode() ? 0.5f : 1.0f);
		vr->scaleX = g_Config.fHeadUpDisplayScale * aspect;
		vr->scaleY = g_Config.fHeadUpDisplayScale;
	} else {
		vr->scaleX = 1.0f;
		vr->scaleY = 1.0f;
	}

	if (dirtyUniforms & DIRTY_FOGCOLOR) {
		// For the skyplane hack's clear.
		SetVRCompat(VR_COMPAT_FOG_COLOR, gstate.fogcolor);
	}

	// Every draw, not only when the game changes the matrices: the result also depends on whether this
	// draw is 2D and on the head pose. GL gets away with dirty flags because its uniforms are per program
	// (2D and 3D draws rarely share one), while this buffer is shared by all draws.
	{
		if (flatScreen || is2D) {
			memcpy(vr->projLens, gstate.projMatrix, sizeof(vr->projLens));
		} else {
			UpdateVRProjection(gstate.projMatrix, vr->projLens);
		}
		UpdateVRParams(gstate.projMatrix);
	}

	{
		float leftEyeView[16];
		float rightEyeView[16];
		ConvertMatrix4x3To4x4Transposed(leftEyeView, gstate.viewMatrix);
		ConvertMatrix4x3To4x4Transposed(rightEyeView, gstate.viewMatrix);
		if (!is2D) {
			UpdateVRView(leftEyeView, rightEyeView);
		}
		// The head pose is a rotation and a translation, so the last row stays 0 0 0 1 and the
		// first three are the 3x4 the shaders take.
		memcpy(base->view, leftEyeView, sizeof(base->view));
		memcpy(vr->viewRight, rightEyeView, sizeof(vr->viewRight));
	}
}

static bool SameBounds(const StereoBounds2D &a, const StereoBounds2D &b) {
	return fabsf(a.x1 - b.x1) <= 1.0f && fabsf(a.y1 - b.y1) <= 1.0f && fabsf(a.x2 - b.x2) <= 1.0f && fabsf(a.y2 - b.y2) <= 1.0f;
}

static bool InsideBounds(const StereoBounds2D &inner, const StereoBounds2D &outer) {
	return inner.x1 >= outer.x1 - 1.0f && inner.y1 >= outer.y1 - 1.0f && inner.x2 <= outer.x2 + 1.0f && inner.y2 <= outer.y2 + 1.0f;
}

// A lens flare finds out how much of the sun is hidden with a probe. Peace Walker copies an 8x8 area at the sun
// out of the frame, draws black and then white over it with a depth test at the sun's depth, copies the result
// out to measure it, and puts the saved pixels back. Each eye has to do all of that where it sees the sun, or the
// probe shows (the copies aren't shifted) and measures the wrong pixels. So a small area that a 2D draw copies
// out of a framebuffer is remembered, 2D draws exactly over it go to the far distance (PROBE), and copies out of
// it sample it shifted the same way (PROBE_COPY). Returns WORLD for neither.
static StereoDrawClass ClassifyStereoProbe(bool through) {
	// Larger copies are effects of the whole frame, like bloom.
	const float MAX_SIZE = 64.0f;

	const u32 target = gstate.getFrameBufRawAddress();
	for (int i = 0; i < gstate_c.numStereoProbeAreas; i++) {
		const StereoProbeArea &area = gstate_c.stereoProbeAreas[i];
		if (area.fbAddress == target && SameBounds(gstate_c.stereoBounds2D, area.bounds)) {
			return StereoDrawClass::PROBE;
		}
	}

	// A copy from another framebuffer. A texture that starts inside one has an offset we don't map back.
	if (!through || !gstate_c.stereoHasUVBounds2D || !gstate.isTextureMapEnabled() || !gstate_c.textureIsFramebuffer || gstate_c.curTextureXOffset != 0 || gstate_c.curTextureYOffset != 0) {
		return StereoDrawClass::WORLD;
	}
	// Probes sample the scene. Small framebuffers are effect buffers (Peace Walker's 64x64 blur ones), which
	// are copied whole.
	if (gstate_c.curTextureWidth < 480 || gstate_c.curTextureHeight < 272) {
		return StereoDrawClass::WORLD;
	}
	const u32 source = gstate.getTextureAddress(0) & 0x1FFFF0;
	const StereoBounds2D &uv = gstate_c.stereoUVBounds2D;
	if (source == target || uv.x2 - uv.x1 > MAX_SIZE || uv.y2 - uv.y1 > MAX_SIZE || uv.x2 <= uv.x1 || uv.y2 <= uv.y1) {
		return StereoDrawClass::WORLD;
	}
	// Copied pixel for pixel. Midnight Club keeps its bloom as a 64x38 image in the corner of a full size
	// framebuffer and stretches it over the screen, which isn't a probe.
	const StereoBounds2D &pos = gstate_c.stereoBounds2D;
	if (fabsf((pos.x2 - pos.x1) - (uv.x2 - uv.x1)) > 1.0f || fabsf((pos.y2 - pos.y1) - (uv.y2 - uv.y1)) > 1.0f) {
		return StereoDrawClass::WORLD;
	}
	for (int i = 0; i < gstate_c.numStereoProbeAreas; i++) {
		const StereoProbeArea &area = gstate_c.stereoProbeAreas[i];
		if (area.fbAddress == source && InsideBounds(uv, area.bounds)) {
			return StereoDrawClass::PROBE_COPY;
		}
	}
	if (gstate_c.numStereoProbeAreas < (int)ARRAY_SIZE(gstate_c.stereoProbeAreas)) {
		gstate_c.stereoProbeAreas[gstate_c.numStereoProbeAreas++] = { source, uv };
	}
	return StereoDrawClass::PROBE_COPY;
}

// A small 2D draw near the center of the screen, like a crosshair (Peace Walker draws its dots in ortho, each a
// few pixels), unless the center is busy with other 2D draws, like a menu (Peace Walker's inventory has its
// icons there, among about 30 other draws). Counts those other draws.
static bool IsCrosshair2D() {
	// The largest crosshair part, and how far from the center its edges can be, in PSP pixels.
	const float MAX_SIZE = 64.0f;
	const float MAX_OFFSET_X = 64.0f;
	const float MAX_OFFSET_Y = 48.0f;
	// How far from the center other 2D draws make it busy, and how many of them do. Peace Walker's HUD has none
	// there besides two full screen overlays, which don't count.
	const float BUSY_OFFSET_X = 120.0f;
	const float BUSY_OFFSET_Y = 80.0f;
	const int MAX_NEARBY = 4;
	// The center of the PSP's screen (the buffer is often 512 wide).
	const float CENTER_X = 240.0f;
	const float CENTER_Y = 136.0f;

	// Over a 3D scene drawn in this host frame or the last (a game at 30 FPS can draw a frame across two).
	if (g_Config.iStereoscopyCrosshairDepth == 0 || !gstate_c.stereoHasBounds2D || gstate_c.stereoFramesSinceWorld > 1) {
		return false;
	}
	// Small render targets are effect buffers (Peace Walker blurs in 64x64 ones), and anything is small there.
	if (gstate_c.curRTWidth < 480 || gstate_c.curRTHeight < 272) {
		return false;
	}
	const StereoBounds2D &b = gstate_c.stereoBounds2D;
	const bool small = b.x2 - b.x1 <= MAX_SIZE && b.y2 - b.y1 <= MAX_SIZE;
	if (small && b.x1 >= CENTER_X - MAX_OFFSET_X && b.x2 <= CENTER_X + MAX_OFFSET_X && b.y1 >= CENTER_Y - MAX_OFFSET_Y && b.y2 <= CENTER_Y + MAX_OFFSET_Y) {
		// This frame so far, or the last one, in case the menu is drawn after.
		return gstate_c.stereoNearCenter2D < MAX_NEARBY && gstate_c.stereoNearCenter2DLast < MAX_NEARBY;
	}
	const bool fullscreen = b.x2 - b.x1 >= 0.9f * (float)gstate_c.curRTWidth && b.y2 - b.y1 >= 0.9f * (float)gstate_c.curRTHeight;
	if (!fullscreen && b.x2 > CENTER_X - BUSY_OFFSET_X && b.x1 < CENTER_X + BUSY_OFFSET_X && b.y2 > CENTER_Y - BUSY_OFFSET_Y && b.y1 < CENTER_Y + BUSY_OFFSET_Y) {
		gstate_c.stereoNearCenter2D++;
	}
	return false;
}

static StereoDrawClass ClassifyStereoDraw() {
	const bool through = gstate.isModeThrough();
	// A projection with a constant w (orthographic) has no depth to shift by, like through mode.
	const bool ortho = !through && gstate.projMatrix[3] == 0.0f && gstate.projMatrix[7] == 0.0f && gstate.projMatrix[11] == 0.0f;
	const StereoscopyUIDetection detection = (StereoscopyUIDetection)g_Config.iStereoscopyUIDetectionMode;
	// Without detection, 2D draws aren't shifted, which keeps a probe together too.
	if ((through || ortho) && detection != StereoscopyUIDetection::OFF && gstate_c.stereoHasBounds2D) {
		// Before the clear check, as a probe could clear its area.
		const StereoDrawClass probe = ClassifyStereoProbe(through);
		if (probe != StereoDrawClass::WORLD) {
			return probe;
		}
	}
	if (gstate.isModeClear()) {
		// Shifting a clear would leave a strip at the edge uncleared.
		return StereoDrawClass::FLAT;
	}
	if (through || ortho) {
		// A framebuffer drawn in 2D (a post effect, or compositing) is already in stereo.
		if (detection == StereoscopyUIDetection::OFF || (gstate.isTextureMapEnabled() && gstate_c.textureIsFramebuffer)) {
			return StereoDrawClass::FLAT;
		}
		if (IsCrosshair2D()) {
			return StereoDrawClass::CROSSHAIR;
		}
		return through && gstate_c.stereoFullscreen2D ? StereoDrawClass::SECOND_LAYER : StereoDrawClass::UI;
	}
	if (detection == StereoscopyUIDetection::ADVANCED && !gstate.isDepthTestEnabled() && !gstate.isDepthWriteEnabled()) {
		return StereoDrawClass::SECOND_LAYER;
	}
	return StereoDrawClass::WORLD;
}

void UpdateStereoDrawClass() {
	if (!gstate_c.Use(GPU_USE_STEREOSCOPY)) {
		return;
	}
	const StereoDrawClass drawClass = ClassifyStereoDraw();
	if (drawClass == StereoDrawClass::WORLD) {
		gstate_c.stereoFramesSinceWorld = 0;
	}
	if (drawClass != gstate_c.stereoDrawClass) {
		gstate_c.stereoDrawClass = drawClass;
		gstate_c.Dirty(DIRTY_STEREO);
	}
}

static void UpdateStereoUniforms(UB_VS_FS_Base *ub) {
	// The eyes move apart by the separation, which in clip space is the share of the target's half width.
	// The dominant eye stays put, and the other moves all of it.
	const float separation = g_Config.fStereoscopySeparation * 0.001f;
	float left = -separation;
	float right = separation;
	switch ((StereoscopyDominantEye)g_Config.iStereoscopyDominantEye) {
	case StereoscopyDominantEye::LEFT:
		left = 0.0f;
		right = 2.0f * separation;
		break;
	case StereoscopyDominantEye::RIGHT:
		left = -2.0f * separation;
		right = 0.0f;
		break;
	default:
		break;
	}

	const StereoDrawClass drawClass = gstate_c.stereoDrawClass;
	if (drawClass == StereoDrawClass::FLAT) {
		left = 0.0f;
		right = 0.0f;
	}
	ub->stereoEyeScale[0] = left;
	ub->stereoEyeScale[1] = right;
	const float convergence = std::clamp(g_Config.fStereoscopyConvergence, 0.0f, 200.0f);
	ub->stereoConvergence = convergence / 10.0f + (3000.0f - 200.0f / 10.0f) * std::pow(convergence / 200.0f, 4.0f);
	const bool probe = drawClass == StereoDrawClass::PROBE || drawClass == StereoDrawClass::PROBE_COPY;
	ub->stereoForceUI = (drawClass == StereoDrawClass::UI || drawClass == StereoDrawClass::CROSSHAIR || drawClass == StereoDrawClass::SECOND_LAYER || probe) ? 1.0f : 0.0f;
	const StereoscopyUIDetection detection = (StereoscopyUIDetection)g_Config.iStereoscopyUIDetectionMode;
	// Threshold-detected world vertices use UI depth; other Advanced-only detections use the second layer.
	const bool advancedWorld = drawClass == StereoDrawClass::WORLD && detection == StereoscopyUIDetection::ADVANCED;
	const bool secondLayer = drawClass == StereoDrawClass::SECOND_LAYER;
	// At most the separation either way, like the far distance.
	int depth = secondLayer ? g_Config.iStereoscopySecondLayerParallax : g_Config.iStereoscopyUIDepth;
	if (drawClass == StereoDrawClass::CROSSHAIR) {
		// Relative to the UI.
		depth += g_Config.iStereoscopyCrosshairDepth;
	}
	ub->stereoUIDepth = (float)std::clamp(depth, -100, 100) * 0.01f;
	if (probe) {
		// The far distance, where lens flares' suns are (Peace Walker's is 98% of the way there).
		ub->stereoUIDepth = 1.0f;
	}
	// A copy out of a probe area moves its texture coordinates instead. Its source is a framebuffer, whose
	// width in texels is the same as the probe's render target's in pixels (bufferWidth), so half the shift in
	// clip space, which spans two of that.
	ub->stereoUVShift = drawClass == StereoDrawClass::PROBE_COPY ? 0.5f : 0.0f;
	// Advanced detection puts world vertices nearer than the configured clip-w threshold at UI depth.
	ub->stereoUIThresholdW = advancedWorld ? (float)g_Config.iStereoscopyUIDetectionThreshold : -std::numeric_limits<float>::max();
}

void DirtyDepalForDraw(bool pixelMapped) {
	// The locked byte only holds for the draw the software transform worked it out for. Through mode always goes
	// through it, so a value left over from a culled draw can't reach a hardware transformed one.
	const u8 lockedByte = gstate.isModeThrough() ? gstate_c.nextDepalLockedByte : 0;
	gstate_c.nextDepalLockedByte = 0;
	if (gstate_c.shaderDepalMode != ShaderDepalMode::OFF && (pixelMapped != gstate_c.depalPixelMapped || lockedByte != gstate_c.depalLockedByte)) {
		gstate_c.Dirty(DIRTY_DEPAL);
	}
	gstate_c.depalLockedByte = lockedByte;
}

uint32_t PackDepalBits(bool pixelMapped) {
	gstate_c.depalPixelMapped = pixelMapped;
	const int indexMask = gstate.getClutIndexMask();
	const int indexShift = gstate.getClutIndexShift();
	const int indexOffset = gstate.getClutIndexStartPos() >> 4;
	const int format = gstate_c.depalTextureFormat;
	uint32_t val = BytesToUint32(indexMask, indexShift, indexOffset, format);
	// NOTE: This must follow similar logic to TextureCacheCommon::GetSamplingParams -
	// maybe we can share it somehow.
	// TODO: Handle replaced textures.
	bool bilinear = gstate.isMagnifyFilteringEnabled() && !pixelMapped;
	switch (GetTextureFilteringSetting()) {
	case TEX_FILTER_FORCE_NEAREST:
		bilinear = false;
		break;
	case TEX_FILTER_FORCE_LINEAR:
		if (CanForceBilinear(gstate)) {
			bilinear = true;
		}
		break;
	default:
		break;
	}
	if (bilinear) {
		// Poke in a bilinear filter flag in the top bit.
		val |= 0x80000000;
	}
	if (gstate_c.shaderDepalMode == ShaderDepalMode::CLUT8) {
		if (gstate_c.depalSmoothClut8) {
			val |= 0x40000000;
		}
		if (gstate_c.depalLockedByte) {
			// Bits 24-26 are the format.
			val |= 0x08000000 | ((u32)(gstate_c.depalLockedByte - 1) << 28);
		}
	}
	return val;
}

// For "light ubershader" bits.
// TODO: We pack these bits even when not using ubershader lighting. Maybe not bother.
uint32_t PackLightControlBits() {
	// Bit organization
	// Bottom 4 bits are enable bits for each light.
	// Then, for each light, comes 2 bits for "comp" and 2 bits for "type".
	// At the end, at bit 20, we put the three material update bits.

	uint32_t lightControl = 0;
	for (int i = 0; i < 4; i++) {
		if (gstate.isLightChanEnabled(i)) {
			lightControl |= 1 << i;
		}

		u32 computation = (u32)gstate.getLightComputation(i);  // 2 bits
		u32 type = (u32)gstate.getLightType(i);  // 2 bits
		if (type == 3) { type = 0; }  // Don't want to handle this degenerate case in the shader.
		lightControl |= computation << (4 + i * 4);
		lightControl |= type << (4 + i * 4 + 2);
	}

	// Material update is 3 bits.
	lightControl |= gstate.getMaterialUpdate() << 20;
	return lightControl;
}

void LightUpdateUniforms(UB_VS_Lights *ub, uint64_t dirtyUniforms) {
	// Lighting
	if (dirtyUniforms & DIRTY_AMBIENT) {
		Uint8x3ToFloat4_AlphaUint8(ub->ambientColor, gstate.ambientcolor, gstate.getAmbientA());
	}
	if (dirtyUniforms & DIRTY_MATDIFFUSE) {
		Uint8x3ToFloat4(ub->materialDiffuse, gstate.materialdiffuse);
	}
	if (dirtyUniforms & DIRTY_MATSPECULAR) {
		Uint8x3ToFloat4_Alpha(ub->materialSpecular, gstate.materialspecular, std::max(0.0f, PSPLightExponent(getFloat24(gstate.materialspecularcoef))));
	}
	if (dirtyUniforms & DIRTY_MATEMISSIVE) {
		// We're not touching the fourth f32 here, because we store an u32 of control bits in it.
		Uint8x3ToFloat3(ub->materialEmissive, gstate.materialemissive);
	}
	if (dirtyUniforms & DIRTY_LIGHT_CONTROL) {
		ub->lightControl = PackLightControlBits();
	}
	for (int i = 0; i < 4; i++) {
		if (dirtyUniforms & (DIRTY_LIGHT0 << i)) {
			if (gstate.isDirectionalLight(i)) {
				// Prenormalize
				ExpandFloat24x3ToFloat4AndNormalize(ub->lpos[i], &gstate.lpos[i * 3]);
			} else {
				ExpandFloat24x3ToFloat4(ub->lpos[i], &gstate.lpos[i * 3]);
			}
			// ldir is only used for spotlights. Prenormalize it.
			ExpandFloat24x3ToFloat4AndNormalize(ub->ldir[i], &gstate.ldir[i * 3]);
			ExpandFloat24x3ToFloat4(ub->latt[i], &gstate.latt[i * 3]);
			float lightAngle_spotCoef[2] = { getFloat24(gstate.lcutoff[i]), PSPLightExponent(getFloat24(gstate.lconv[i])) };
			CopyFloat2To4(ub->lightAngle_SpotCoef[i], lightAngle_spotCoef);
			Uint8x3ToFloat4(ub->lightAmbient[i], gstate.lcolor[i * 3]);
			Uint8x3ToFloat4(ub->lightDiffuse[i], gstate.lcolor[i * 3 + 1]);
			Uint8x3ToFloat4(ub->lightSpecular[i], gstate.lcolor[i * 3 + 2]);
		}
	}
}

void UpdateFogCoef(const GEState &state, float fogCoef[2]) {
	fogCoef[0] = getFloat24(gstate.fog1);
	fogCoef[1] = getFloat24(gstate.fog2);
	// The PSP just ignores infnan here (ignoring IEEE), so take it down to a valid float.
	// Workaround for https://github.com/hrydgard/ppsspp/issues/5384#issuecomment-38365988
	if (my_isnanorinf(fogCoef[0])) {
		// Not really sure what a sensible value might be, but let's try 64k.
		fogCoef[0] = std::signbit(fogCoef[0]) ? -65535.0f : 65535.0f;
	}
	if (my_isnanorinf(fogCoef[1])) {
		fogCoef[1] = std::signbit(fogCoef[1]) ? -65535.0f : 65535.0f;
	}
}
