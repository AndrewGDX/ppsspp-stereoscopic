// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#if defined(ANDROID) && !defined(__LIBRETRO__)
#include "android/jni/DisplayXR.h"
#endif

#include <algorithm>
#include <cmath>
#include <set>
#include <cstdint>
#include <cstring>
#include <memory>
#include "Common/GPU/thin3d.h"
#include "Common/GPU/ShaderWriter.h"

#include "Common/System/Display.h"
#include "Common/System/System.h"
#include "Common/System/OSD.h"
#include "Common/File/VFS/VFS.h"
#include "Common/VR/PPSSPPVR.h"
#ifdef __LIBRETRO__
#include "libretro/LibretroVR.h"
#endif
#include "Common/Math/geom2d.h"
#include "Common/Log.h"
#include "Common/TimeUtil.h"
#include "Core/Config.h"
#include "Core/ConfigValues.h"
#include "Core/System.h"
#include "Core/HW/Display.h"
#include "GPU/Common/Overlay.h"
#include "GPU/Common/PostShader.h"
#include "GPU/Common/PresentationCommon.h"
#include "GPU/GPUState.h"
#include "Common/GPU/ShaderTranslation.h"

struct Vertex {
	float x, y, z;
	float u, v;
	uint32_t rgba;
};

static bool g_overrideScreenBounds;
static Bounds g_screenBounds;

void SetOverrideScreenFrame(const Bounds *bounds) {
	g_overrideScreenBounds = bounds != nullptr;
	if (bounds) {
		g_screenBounds = *bounds;
	}
}

OutputParams PrimaryOutputParams(int pixelWidth, int pixelHeight) {
	OutputParams output;
	output.pixelWidth = pixelWidth;
	output.pixelHeight = pixelHeight;
	// Remove the DPI scale to get back to pixels.
	output.insetLeft = System_GetPropertyFloat(SYSPROP_DISPLAY_SAFE_INSET_LEFT) / g_display.dpi_scale_x;
	output.insetRight = System_GetPropertyFloat(SYSPROP_DISPLAY_SAFE_INSET_RIGHT) / g_display.dpi_scale_x;
	output.insetTop = System_GetPropertyFloat(SYSPROP_DISPLAY_SAFE_INSET_TOP) / g_display.dpi_scale_y;
	output.insetBottom = System_GetPropertyFloat(SYSPROP_DISPLAY_SAFE_INSET_BOTTOM) / g_display.dpi_scale_y;
	output.orientation = g_display.GetDeviceOrientation();
	output.rotation = g_display.rotation;
	output.rotMatrix = g_display.rot_matrix;
	output.useOverrideFrame = true;
	return output;
}

FRect GetScreenFrame(bool ignoreScreenInsets, const OutputParams &output) {
	FRect rc = FRect{
		0.0f,
		0.0f,
		(float)output.pixelWidth,
		(float)output.pixelHeight,
	};

	const bool applyInset = !ignoreScreenInsets;

	if (g_overrideScreenBounds && output.useOverrideFrame) {
		// Set rectangle to match central node. Here we ignore bIgnoreScreenInsets.
		rc.x = g_screenBounds.x;
		rc.y = g_screenBounds.y;
		rc.w = g_screenBounds.w;
		rc.h = g_screenBounds.h;
	} else if (applyInset) {
		float left = output.insetLeft;
		float right = output.insetRight;
		float top = output.insetTop;
		float bottom = output.insetBottom;

		// NOTE: Similarly to what we do in UI, we disregard any bottom inset when in landscape mode.
		if (output.orientation == DeviceOrientation::Landscape) {
			bottom = 0.0f;
		}

		// Adjust left edge to compensate for cutouts (notches) if any.
		rc.x += left;
		rc.w -= (left + right);
		rc.y += top;
		rc.h -= (top + bottom);
	}

	return rc;
}

FRect GetScreenFrame(bool ignoreScreenInsets, float pixelWidth, float pixelHeight) {
	return GetScreenFrame(ignoreScreenInsets, PrimaryOutputParams((int)pixelWidth, (int)pixelHeight));
}

void CalculateDisplayOutputRect(const DisplayLayoutConfig &config, FRect *rc, float origW, float origH, const FRect &screenFrame, int rotation) {
	CalculateDisplayOutputRect(config, rc, origW, origH, screenFrame, rotation, g_display.GetDeviceOrientation());
}

void CalculateDisplayOutputRect(const DisplayLayoutConfig &config, FRect *rc, float origW, float origH, const FRect &screenFrame, int rotation, DeviceOrientation outputOrientation) {
	float outW;
	float outH;

	bool rotated = rotation == ROTATION_LOCKED_VERTICAL || rotation == ROTATION_LOCKED_VERTICAL180;

	bool stretch = config.bDisplayStretch && !config.bDisplayIntegerScale;

	float offsetX = config.fDisplayOffsetX;
	float offsetY = config.fDisplayOffsetY;

	float scale = config.fDisplayScale;

	// With an overlay that has a hole, the game is fit into the hole instead. The offset and scale
	// options were already applied to the overlay itself, see GetOverlayLayout.
	FRect frame = screenFrame;
	OverlayLayout overlay;
	const bool inOverlayHole = GetOverlayLayout(config, screenFrame.x, screenFrame.y, screenFrame.w, screenFrame.h, &overlay) && overlay.hasHole;
	if (inOverlayHole) {
		frame = FRect{ overlay.holeX, overlay.holeY, overlay.holeW, overlay.holeH };
		offsetX = 0.5f;
		offsetY = 0.5f;
		scale = 1.0f;
	}
	float aspectRatioAdjust = config.fDisplayAspectRatio;

	float origRatio = !rotated ? origW / origH : origH / origW;
	float frameRatio = frame.w / frame.h;

	if (stretch) {
		// Automatically set aspect ratio to match the display, IF the rotation matches the output display ratio! Otherwise, just
		// ignore it because actually stretching will just look silly.
		if (rotated == (outputOrientation == DeviceOrientation::Portrait)) {
			origRatio = frameRatio;
		} else {
			origRatio *= aspectRatioAdjust;
		}
	} else {
		origRatio *= aspectRatioAdjust;
	}

	float scaledWidth = frame.w * scale;
	float scaledHeight = frame.h * scale;

	if (origRatio > frameRatio) {
		// Image is wider than frame. Center vertically.
		outW = scaledWidth;
		outH = scaledWidth / origRatio;
	} else {
		// Image is taller than frame. Center horizontally.
		outW = scaledHeight * origRatio;
		outH = scaledHeight;
	}

	// Ye olde 1080p hack: If everything is setup to exactly cover the screen (defaults), and the screen display aspect ratio is 16:9,
	// cut off one line from the top and bottom.
	if (!inOverlayHole && scale == 1.0f && aspectRatioAdjust == 1.0f && offsetX == 0.5f && offsetY == 0.5f && g_Config.bDisplayCropTo16x9) {
		if (fabsf(frame.w / frame.h - 16.0f / 9.0f) < 0.0001f) {
			outW *= 272.0f / 270.0f;
			outH *= 272.0f / 270.0f;
		}
	}

	if (config.bDisplayIntegerScale) {
		float wDim = 480.0f;
		if (rotated) {
			wDim = 272.0f;
		}

		int zoom = g_Config.iInternalResolution;
		if (zoom == 0) {
			// Auto (1:1) mode, not super meaningful with integer scaling, but let's do something that makes
			// some sense. use the longest dimension, just to have something. round down.
			if (!config.InternalRotationIsPortrait()) {
				zoom = (PSP_CoreParameter().pixelWidth) / 480;
			} else {
				zoom = (PSP_CoreParameter().pixelHeight) / 480;
			}
		}
		// If integer scaling, limit ourselves to even multiples of the rendered resolution,
		// to make sure all the pixels are square.
		wDim *= zoom;
		outW = std::max(1.0f, floorf(outW / wDim)) * wDim;
		outH = outW / origRatio;
	}

	if (IsVREnabled()) {
		rc->x = 0;
		rc->y = 0;
		rc->w = floorf(frame.w);
		rc->h = floorf(frame.h);
		outW = frame.w;
		outH = frame.h;
	} else if (inOverlayHole && overlay.layer == OverlayLayer::Foreground) {
		// Round outwards, so no black seam shows between the game and the overlay's border.
		const float x0 = frame.x + frame.w * offsetX - outW * 0.5f;
		const float y0 = frame.y + frame.h * offsetY - outH * 0.5f;
		rc->x = floorf(x0);
		rc->y = floorf(y0);
		rc->w = ceilf(x0 + outW) - rc->x;
		rc->h = ceilf(y0 + outH) - rc->y;
	} else {
		rc->x = floorf(frame.x + frame.w * offsetX - outW * 0.5f);
		rc->y = floorf(frame.y + frame.h * offsetY - outH * 0.5f);
		rc->w = floorf(outW);
		rc->h = floorf(outH);
	}
}

PresentationCommon::PresentationCommon(Draw::DrawContext *draw) : draw_(draw) {
	CreateDeviceObjects();
}

PresentationCommon::~PresentationCommon() {
	DestroyDeviceObjects();
}

// Cardboard VR is for the device's own screen, in a headset, not a TV.
bool PresentationCommon::UseCardboard(const DisplayLayoutConfig &config) const {
	return config.bEnableCardboardVR && !hasOutputOverride_;
}

void PresentationCommon::GetCardboardSettings(const DisplayLayoutConfig &config, const OutputParams &output, CardboardSettings *cardboardSettings) const {
	if (!UseCardboard(config)) {
		cardboardSettings->enabled = false;
		return;
	}

	// Calculate Cardboard Settings
	float cardboardScreenScale = config.iCardboardScreenSize / 100.0f;
	float cardboardScreenWidth = output.pixelWidth / 2.0f * cardboardScreenScale;
	float cardboardScreenHeight = output.pixelHeight * cardboardScreenScale;
	float cardboardMaxXShift = (output.pixelWidth / 2.0f - cardboardScreenWidth) / 2.0f;
	float cardboardUserXShift = config.iCardboardXShift / 100.0f * cardboardMaxXShift;
	float cardboardLeftEyeX = cardboardMaxXShift + cardboardUserXShift;
	float cardboardRightEyeX = output.pixelWidth / 2.0f + cardboardMaxXShift - cardboardUserXShift;
	float cardboardMaxYShift = output.pixelHeight / 2.0f - cardboardScreenHeight / 2.0f;
	float cardboardUserYShift = config.iCardboardYShift / 100.0f * cardboardMaxYShift;
	float cardboardScreenY = cardboardMaxYShift + cardboardUserYShift;

	cardboardSettings->enabled = true;
	cardboardSettings->leftEyeXPosition = cardboardLeftEyeX;
	cardboardSettings->rightEyeXPosition = cardboardRightEyeX;
	cardboardSettings->screenYPosition = cardboardScreenY;
	cardboardSettings->screenWidth = cardboardScreenWidth;
	cardboardSettings->screenHeight = cardboardScreenHeight;
}

static float GetShaderSettingValue(const ShaderInfo *shaderInfo, int i, const char *nameSuffix) {
	std::string key = shaderInfo->section + nameSuffix;
	auto it = g_Config.mPostShaderSetting.find(key);
	if (it != g_Config.mPostShaderSetting.end())
		return it->second;
	return shaderInfo->settings[i].value;
}

void PresentationCommon::CalculatePostShaderUniforms(int bufferWidth, int bufferHeight, int targetWidth, int targetHeight, const ShaderInfo *shaderInfo, PostShaderUniforms *uniforms) const {
	float u_delta = 1.0f / bufferWidth;
	float v_delta = 1.0f / bufferHeight;
	float u_pixel_delta = 1.0f / targetWidth;
	float v_pixel_delta = 1.0f / targetHeight;
	int flipCount = __DisplayGetFlipCount();
	int vCount = __DisplayGetVCount();
	float time[4] = { (float)time_now_d(), (vCount % 60) * 1.0f / 60.0f, (float)vCount, (float)(flipCount % 60) };

	uniforms->texelDelta[0] = u_delta;
	uniforms->texelDelta[1] = v_delta;
	uniforms->pixelDelta[0] = u_pixel_delta;
	uniforms->pixelDelta[1] = v_pixel_delta;
	memcpy(uniforms->time, time, 4 * sizeof(float));
	uniforms->timeDelta[0] = time[0] - previousUniforms_.time[0];
	uniforms->timeDelta[1] = (time[2] - previousUniforms_.time[2]) * (1.0f / 60.0f);
	uniforms->timeDelta[2] = time[2] - previousUniforms_.time[2];
	uniforms->timeDelta[3] = time[3] != previousUniforms_.time[3] ? 1.0f : 0.0f;
	uniforms->video = hasVideo_ ? 1.0f : 0.0f;
	uniforms->vr = IsVREnabled() && IsBigScreenVRMode() ? 1.0f : 0.0f;

	// The shader translator tacks this onto our shaders, if we don't set it they render garbage.
	uniforms->gl_HalfPixel[0] = u_pixel_delta * 0.5f;
	uniforms->gl_HalfPixel[1] = v_pixel_delta * 0.5f;

	uniforms->setting[0] = GetShaderSettingValue(shaderInfo, 0, "SettingCurrentValue1");
	uniforms->setting[1] = GetShaderSettingValue(shaderInfo, 1, "SettingCurrentValue2");
	uniforms->setting[2] = GetShaderSettingValue(shaderInfo, 2, "SettingCurrentValue3");
	uniforms->setting[3] = GetShaderSettingValue(shaderInfo, 3, "SettingCurrentValue4");
}

static std::string ReadShaderSrc(const Path &filename) {
	size_t sz = 0;
	char *data = (char *)g_VFS.ReadFile(filename.c_str(), &sz);
	if (!data) {
		return "";
	}

	std::string src(data, sz);
	delete[] data;
	return src;
}

// Where each eye goes in the frame. Each eye is laid out in eyeFrame (at the origin) like a mono image is in the
// whole frame, then drawn with the eye's viewport. The half modes squeeze each eye into half the frame, the full
// ones keep its aspect ratio, so the two together are twice as wide (or tall) as the frame and get letterboxed.
static void GetStereoscopyLayout(StereoscopyMode mode, const FRect frame, FRect *eyeFrame, FRect viewports[2]) {
	const float halfW = frame.w * 0.5f;
	const float halfH = frame.h * 0.5f;
	const bool full = mode == StereoscopyMode::FULL_SBS || mode == StereoscopyMode::FULL_TAB;
	*eyeFrame = { 0.0f, 0.0f, full ? halfW : frame.w, full ? halfH : frame.h };
	for (int eye = 0; eye < 2; eye++) {
		FRect &vp = viewports[eye];
		switch (mode) {
		case StereoscopyMode::FULL_SBS:
			vp = { frame.x + eye * halfW, frame.y + halfH * 0.5f, halfW, halfH };
			break;
		case StereoscopyMode::HALF_TAB:
			vp = { frame.x, frame.y + eye * halfH, frame.w, halfH };
			break;
		case StereoscopyMode::FULL_TAB:
			vp = { frame.x + halfW * 0.5f, frame.y + eye * halfH, halfW, halfH };
			break;
		case StereoscopyMode::HALF_SBS:
		default:
			vp = { frame.x + eye * halfW, frame.y, halfW, frame.h };
			break;
		}
	}
}

static bool StereoscopySideBySide(StereoscopyMode mode) {
	return mode == StereoscopyMode::HALF_SBS || mode == StereoscopyMode::FULL_SBS
#if defined(ANDROID) && !defined(__LIBRETRO__)
		|| mode == StereoscopyMode::DISPLAY_XR
#endif
		;
}

// With stereoscopy, post shaders run on both eyes composited side by side (or top and bottom), so their
// framebuffers are twice the size of an eye along the layout.
void PresentationCommon::ScaleForStereoComposite(int *w, int *h) const {
	if (stereoPostMode_ == StereoscopyMode::OFF) {
		return;
	}
	if (StereoscopySideBySide(stereoPostMode_)) {
		*w *= 2;
	} else {
		*h *= 2;
	}
}

// Note: called on resize and settings changes.
bool PresentationCommon::UpdatePostShader(const DisplayLayoutConfig &config) {
	std::vector<const ShaderInfo *> shaderInfo;
	if (!g_Config.vPostShaderNames.empty()) {
		ReloadAllPostShaderInfo(draw_);
		shaderInfo = GetFullPostShadersChain(g_Config.vPostShaderNames);
	}

	DestroyPostShader();
	if (shaderInfo.empty()) {
		usePostShader_ = false;
		return false;
	}

	if (gstate_c.Use(GPU_USE_STEREOSCOPY)) {
		stereoPostMode_ = (StereoscopyMode)g_Config.iStereoscopyMode;
		int w = renderWidth_;
		int h = renderHeight_;
		ScaleForStereoComposite(&w, &h);
		stereoComposite_ = draw_->CreateFramebuffer({ w, h, 1, 1, 0, false, "stereo_composite" });
		if (!stereoComposite_) {
			DestroyPostShader();
			return false;
		}
	}

	bool usePreviousFrame = false;
	bool usePreviousAtOutputResolution = false;
	for (size_t i = 0; i < shaderInfo.size(); ++i) {
		const ShaderInfo *next = i + 1 < shaderInfo.size() ? shaderInfo[i + 1] : nullptr;
		Draw::Pipeline *postPipeline = nullptr;
		if (!BuildPostShader(config, shaderInfo[i], next, &postPipeline)) {
			DestroyPostShader();
			return false;
		}
		_dbg_assert_(postPipeline);
		postShaderPipelines_.push_back(postPipeline);
		postShaderInfo_.push_back(*shaderInfo[i]);
		if (shaderInfo[i]->usePreviousFrame) {
			usePreviousFrame = true;
			usePreviousAtOutputResolution = shaderInfo[i]->outputResolution;
		}
	}

	if (usePreviousFrame) {
		const OutputParams output = GetOutput();
		int w = usePreviousAtOutputResolution ? output.pixelWidth : renderWidth_;
		int h = usePreviousAtOutputResolution ? output.pixelHeight : renderHeight_;
		if (stereoPostMode_ != StereoscopyMode::OFF && usePreviousAtOutputResolution) {
			// Both eyes' viewports together.
			FRect eyeFrame, viewports[2];
			GetStereoscopyLayout(stereoPostMode_, GetScreenFrame(config.bIgnoreScreenInsets, output), &eyeFrame, viewports);
			w = (int)viewports[0].w;
			h = (int)viewports[0].h;
			ScaleForStereoComposite(&w, &h);
		} else if (!usePreviousAtOutputResolution) {
			ScaleForStereoComposite(&w, &h);
		}

		_dbg_assert_(w > 0 && h > 0);

		static constexpr int FRAMES = 2;
		previousFramebuffers_.resize(FRAMES);
		previousIndex_ = 0;

		for (int i = 0; i < FRAMES; ++i) {
			previousFramebuffers_[i] = draw_->CreateFramebuffer({ w, h, 1, 1, 0, false, "inter_presentation" });
			if (!previousFramebuffers_[i]) {
				DestroyPostShader();
				return false;
			}
		}
	}

	usePostShader_ = true;
	return true;
}

bool PresentationCommon::CompilePostShader(const ShaderInfo *shaderInfo, Draw::Pipeline **outPipeline) const {
	_assert_(shaderInfo);

	std::string vsSourceGLSL = ReadShaderSrc(shaderInfo->vertexShaderFile);
	std::string fsSourceGLSL = ReadShaderSrc(shaderInfo->fragmentShaderFile);
	if (vsSourceGLSL.empty() || fsSourceGLSL.empty()) {
		return false;
	}

	std::string vsError;
	std::string fsError;

	// All post shaders are written in GLSL 1.0 so that's what we pass in here as a "from" language.
	Draw::ShaderModule *vs = CompileShaderModule(ShaderStage::Vertex, GLSL_1xx, vsSourceGLSL, &vsError);
	Draw::ShaderModule *fs = CompileShaderModule(ShaderStage::Fragment, GLSL_1xx, fsSourceGLSL, &fsError);

	// Don't worry, CompileShaderModule makes sure they get freed if one succeeded.
	if (!fs || !vs) {
		std::string errorString = vsError + "\n" + fsError;
		// DO NOT turn this into an ERROR_LOG_REPORT, as it will pollute our logs with all kinds of
		// user shader experiments.
		ERROR_LOG(Log::FrameBuf, "Failed to build post-processing program from %s and %s!\n%s", shaderInfo->vertexShaderFile.c_str(), shaderInfo->fragmentShaderFile.c_str(), errorString.c_str());
		ShowPostShaderError(errorString);
		return false;
	}

	UniformBufferDesc postShaderDesc{ sizeof(PostShaderUniforms), {
		{ "gl_HalfPixel", 0, -1, UniformType::FLOAT4, offsetof(PostShaderUniforms, gl_HalfPixel) },
		{ "u_texelDelta", 1, 1, UniformType::FLOAT2, offsetof(PostShaderUniforms, texelDelta) },
		{ "u_pixelDelta", 2, 2, UniformType::FLOAT2, offsetof(PostShaderUniforms, pixelDelta) },
		{ "u_time", 3, 3, UniformType::FLOAT4, offsetof(PostShaderUniforms, time) },
		{ "u_timeDelta", 4, 4, UniformType::FLOAT4, offsetof(PostShaderUniforms, timeDelta) },
		{ "u_setting", 5, 5, UniformType::FLOAT4, offsetof(PostShaderUniforms, setting) },
		{ "u_video", 6, 6, UniformType::FLOAT1, offsetof(PostShaderUniforms, video) },
		{ "u_vr", 7, 7, UniformType::FLOAT1, offsetof(PostShaderUniforms, vr) },
	} };

	Draw::Pipeline *pipeline = CreatePipeline({ vs, fs }, true, &postShaderDesc);

	fs->Release();
	vs->Release();

	if (!pipeline)
		return false;

	*outPipeline = pipeline;
	return true;
}

bool PresentationCommon::BuildPostShader(const DisplayLayoutConfig &config, const ShaderInfo *shaderInfo, const ShaderInfo * next, Draw::Pipeline **outPipeline) {
	if (!CompilePostShader(shaderInfo, outPipeline)) {
		return false;
	}

	if (!shaderInfo->outputResolution || next) {
		int nextWidth = renderWidth_;
		int nextHeight = renderHeight_;
		ScaleForStereoComposite(&nextWidth, &nextHeight);

		// When chaining, we use the previous resolution as a base, rather than the render resolution.
		if (!postShaderFramebuffers_.empty())
			draw_->GetFramebufferDimensions(postShaderFramebuffers_.back(), &nextWidth, &nextHeight);

		if (next && next->isUpscalingFilter) {
			// Force 1x for this shader, so the next can upscale.
			const bool isPortrait = config.InternalRotationIsPortrait();
			nextWidth = isPortrait ? 272 : 480;
			nextHeight = isPortrait ? 480 : 272;
			ScaleForStereoComposite(&nextWidth, &nextHeight);
		} else if (next && next->SSAAFilterLevel >= 2) {
			// Increase the resolution this shader outputs for the next to SSAA.
			nextWidth *= next->SSAAFilterLevel;
			nextHeight *= next->SSAAFilterLevel;
		} else if (shaderInfo->outputResolution) {
			// If the current shader uses output res (not next), we will use output res for it.
			const OutputParams output = GetOutput();
			FRect rc;
			FRect frame = GetScreenFrame(config.bIgnoreScreenInsets, output);
			FRect viewports[2];
			if (stereoPostMode_ != StereoscopyMode::OFF) {
				GetStereoscopyLayout(stereoPostMode_, frame, &frame, viewports);
			}
			CalculateDisplayOutputRect(config, &rc, 480.0f, 272.0f, frame, config.iInternalScreenRotation, output.orientation);
			nextWidth = (int)rc.w;
			nextHeight = (int)rc.h;
			if (stereoPostMode_ != StereoscopyMode::OFF) {
				// The eye as it's shown (the half modes squeeze it), twice.
				nextWidth = (int)(rc.w * viewports[0].w / frame.w);
				nextHeight = (int)(rc.h * viewports[0].h / frame.h);
				ScaleForStereoComposite(&nextWidth, &nextHeight);
			}
		}

		if (!AllocateFramebuffer(nextWidth, nextHeight)) {
			(*outPipeline)->Release();
			*outPipeline = nullptr;
			return false;
		}
	}

	return true;
}

bool PresentationCommon::AllocateFramebuffer(int w, int h) {
	using namespace Draw;

	// First, let's try to find a framebuffer of the right size that is NOT the most recent.
	Framebuffer *last = postShaderFramebuffers_.empty() ? nullptr : postShaderFramebuffers_.back();
	for (const auto &prev : postShaderFBOUsage_) {
		if (prev.w == w && prev.h == h && prev.fbo != last) {
			// Great, this one's perfect.  Ref it for when we release.
			prev.fbo->AddRef();
			postShaderFramebuffers_.push_back(prev.fbo);
			return true;
		}
	}

	// No depth/stencil for post processing
	Draw::Framebuffer *fbo = draw_->CreateFramebuffer({ w, h, 1, 1, 0, false, "presentation" });
	if (!fbo) {
		return false;
	}

	postShaderFBOUsage_.push_back({ fbo, w, h });
	postShaderFramebuffers_.push_back(fbo);
	return true;
}

void PresentationCommon::ShowPostShaderError(const std::string &errorString) {
	// let's show the first line of the error string as an OSM.
	std::set<std::string> blacklistedLines;
	// These aren't useful to show, skip to the first interesting line.
	blacklistedLines.insert("Fragment shader failed to compile with the following errors:");
	blacklistedLines.insert("Vertex shader failed to compile with the following errors:");
	blacklistedLines.insert("Compile failed.");
	blacklistedLines.insert("");

	std::string firstLine;
	size_t start = 0;
	for (size_t i = 0; i < errorString.size(); i++) {
		if (errorString[i] == '\n' && i == start) {
			start = i + 1;
		} else if (errorString[i] == '\n') {
			firstLine = errorString.substr(start, i - start);
			if (blacklistedLines.find(firstLine) == blacklistedLines.end()) {
				break;
			}
			start = i + 1;
			firstLine.clear();
		}
	}
	if (!firstLine.empty()) {
		g_OSD.Show(OSDType::MESSAGE_ERROR_DUMP, "Post-shader error: " + firstLine + "...:\n" + errorString, 10.0f);
	} else {
		g_OSD.Show(OSDType::MESSAGE_ERROR, "Post-shader error, see log for details", 10.0f);
	}
}

void PresentationCommon::DeviceLost() {
	DestroyDeviceObjects();
	draw_ = nullptr;
}

void PresentationCommon::DeviceRestore(Draw::DrawContext *draw) {
	draw_ = draw;
	CreateDeviceObjects();
}

Draw::Pipeline *PresentationCommon::CreatePipeline(std::vector<Draw::ShaderModule *> shaders, bool postShader, const UniformBufferDesc *uniformDesc, bool alphaBlend) const {
	using namespace Draw;

	Semantic pos = SEM_POSITION;
	Semantic tc = SEM_TEXCOORD0;
	// Shader translation marks these both as "TEXCOORDs" on HLSL...
	if (postShader && lang_ == HLSL_D3D11) {
		pos = SEM_TEXCOORD0;
		tc = SEM_TEXCOORD1;
	}

	// TODO: Maybe get rid of color0.
	InputLayoutDesc inputDesc = {
		sizeof(Vertex),
		{
			{ pos, DataFormat::R32G32B32_FLOAT, 0 },
			{ tc, DataFormat::R32G32_FLOAT, 12 },
			{ SEM_COLOR0, DataFormat::R8G8B8A8_UNORM, 20 },
		},
	};

	InputLayout *inputLayout = draw_->CreateInputLayout(inputDesc);
	DepthStencilState *depth = draw_->CreateDepthStencilState({ false, false, Comparison::LESS });
	BlendState *blendstateOff = alphaBlend ?
		draw_->CreateBlendState({ true, 0xF, BlendFactor::SRC_ALPHA, BlendFactor::ONE_MINUS_SRC_ALPHA, BlendOp::ADD, BlendFactor::ONE, BlendFactor::ONE_MINUS_SRC_ALPHA, BlendOp::ADD }) :
		draw_->CreateBlendState({ false, 0xF });
	RasterState *rasterNoCull = draw_->CreateRasterState({});

	PipelineDesc pipelineDesc{ Primitive::TRIANGLE_STRIP, shaders, inputLayout, depth, blendstateOff, rasterNoCull, uniformDesc };
	Pipeline *pipeline = draw_->CreateGraphicsPipeline(pipelineDesc, "presentation");

	inputLayout->Release();
	depth->Release();
	blendstateOff->Release();
	rasterNoCull->Release();

	return pipeline;
}

void PresentationCommon::CreateDeviceObjects() {
	using namespace Draw;

	// Still hitting this somehow!
	_dbg_assert_(vdata_ == nullptr);

	// TODO: Could probably just switch to DrawUP, it's supported well by all backends now.
	vdata_ = draw_->CreateBuffer(sizeof(Vertex) * 16, BufferUsageFlag::DYNAMIC | BufferUsageFlag::VERTEXDATA);

	samplerNearest_ = draw_->CreateSamplerState({ TextureFilter::NEAREST, TextureFilter::NEAREST, TextureFilter::NEAREST, 0.0f, TextureAddressMode::CLAMP_TO_EDGE, TextureAddressMode::CLAMP_TO_EDGE, TextureAddressMode::CLAMP_TO_EDGE });
	samplerLinear_ = draw_->CreateSamplerState({ TextureFilter::LINEAR, TextureFilter::LINEAR, TextureFilter::LINEAR, 0.0f, TextureAddressMode::CLAMP_TO_EDGE, TextureAddressMode::CLAMP_TO_EDGE, TextureAddressMode::CLAMP_TO_EDGE });

	texColor_ = CreatePipeline({ draw_->GetVshaderPreset(VS_TEXTURE_COLOR_2D), draw_->GetFshaderPreset(FS_TEXTURE_COLOR_2D) }, false, &vsTexColBufDesc);
	texColorRBSwizzle_ = CreatePipeline({ draw_->GetVshaderPreset(VS_TEXTURE_COLOR_2D), draw_->GetFshaderPreset(FS_TEXTURE_COLOR_2D_RB_SWIZZLE) }, false, &vsTexColBufDesc);
	texColorBlend_ = CreatePipeline({ draw_->GetVshaderPreset(VS_TEXTURE_COLOR_2D), draw_->GetFshaderPreset(FS_TEXTURE_COLOR_2D) }, false, &vsTexColBufDesc, true);
	if (IsVREnabled() && lang_ == GLSL_VULKAN && draw_->GetDeviceCaps().multiViewSupported) {
		// FS_TEXTURE_COLOR_2D, sampling the layer of the eye being drawn.
		static const VaryingDef varyings[] = {
			{ "vec4", "oColor0", SEM_COLOR0, 0, "lowp" },
			{ "vec2", "oTexCoord0", SEM_TEXCOORD0, 1, "highp" },
		};
		static const SamplerDef samplers[] = {
			{ 0, "tex", SamplerFlags::ARRAY_ON_VULKAN },
		};
		char code[2048];
		ShaderWriter writer(code, draw_->GetShaderLanguageDesc(), ShaderStage::Fragment, Slice<const char *>::empty(), ShaderWriterFlags::FS_AUTO_STEREO);
		writer.DeclareSamplers(samplers);
		writer.BeginFSMain(Slice<UniformDef>::empty(), varyings);
		writer.C("vec4 col = ");
		writer.SampleTexture2D("tex", "oTexCoord0");
		writer.C(" * oColor0;\n");
		writer.C("col.rgb *= oColor0.a;\n");
		writer.EndFSMain("col");
		ShaderModule *fs = draw_->CreateShaderModule(ShaderStage::Fragment, lang_, (const uint8_t *)code, strlen(code), "present_stereo");
		if (fs) {
			texColorStereo_ = CreatePipeline({ draw_->GetVshaderPreset(VS_TEXTURE_COLOR_2D), fs }, false, &vsTexColBufDesc);
			fs->Release();
		}
	}
	overlayVdata_ = draw_->CreateBuffer(sizeof(Vertex) * 4, BufferUsageFlag::DYNAMIC | BufferUsageFlag::VERTEXDATA);
	// Not resetting restorePostShader_: after a device loss, BeginFrame uses it to bring the post-shader back.
}

template <typename T>
static void DoRelease(T *&obj) {
	if (obj)
		obj->Release();
	obj = nullptr;
}

template <typename T>
static void DoReleaseVector(std::vector<T *> &list) {
	for (auto &obj : list) {
		// Can be null when a creation failed partway.
		if (obj) {
			obj->Release();
		}
	}
	list.clear();
}

void PresentationCommon::DestroyDeviceObjects() {
	DoRelease(texColor_);
	DoRelease(texColorRBSwizzle_);
	DoRelease(texColorStereo_);
	DoRelease(texColorBlend_);
	DoRelease(overlayVdata_);
	DoRelease(overlayTexture_);
	overlayGeneration_ = 0xFFFFFFFF;
	DoRelease(samplerNearest_);
	DoRelease(samplerLinear_);
	DoRelease(vdata_);
	DoRelease(srcTexture_);
	DoRelease(srcFramebuffer_);

	restorePostShader_ = usePostShader_;
	DestroyPostShader();
}

void PresentationCommon::DestroyPostShader() {
	usePostShader_ = false;

	DoReleaseVector(postShaderPipelines_);
	DoReleaseVector(postShaderFramebuffers_);
	DoReleaseVector(previousFramebuffers_);
	DoRelease(stereoComposite_);
	stereoPostMode_ = StereoscopyMode::OFF;
	postShaderInfo_.clear();
	postShaderFBOUsage_.clear();
}

Draw::ShaderModule *PresentationCommon::CompileShaderModule(ShaderStage stage, ShaderLanguage lang, const std::string &src, std::string *errorString) const {
	std::string translated = src;
	if (lang != lang_) {
		// Gonna have to upconvert the shader.
		if (!TranslateShader(&translated, lang_, draw_->GetShaderLanguageDesc(), nullptr, src, lang, stage, errorString)) {
			ERROR_LOG(Log::FrameBuf, "Failed to translate post-shader. Error string: '%s'\nSource code:\n%s\n", errorString->c_str(), src.c_str());
			return nullptr;
		}
	}
	return draw_->CreateShaderModule(stage, lang_, (const uint8_t *)translated.c_str(), translated.size(), "postshader");
}

bool StereoscopySupported(Draw::DrawContext *draw) {
	return draw && (draw->GetDeviceCaps().multiViewSupported || draw->GetDeviceCaps().stereoPassReplaySupported) &&
		!g_Config.bSoftwareRendering && !IsVREnabled() && !g_Config.bForceVR;
}

bool StereoscopyEnabled(Draw::DrawContext *draw) {
#if defined(ANDROID) && !defined(__LIBRETRO__)
	// Not whether it can weave right now: that's decided per frame, and changing this recreates the framebuffers.
	if (DisplayXR_Selected() && !DisplayXR_Available()) {
		return false;
	}
#endif
	return g_Config.iStereoscopyMode != (int)StereoscopyMode::OFF && !g_Config.bSkipBufferEffects && StereoscopySupported(draw);
}

bool GetStereoscopyEyeViewports(const DisplayLayoutConfig &config, float pixelWidth, float pixelHeight, FRect viewports[2]) {
#if defined(ANDROID) && !defined(__LIBRETRO__)
	if (DisplayXR_Selected()) {
		return false;  // The service composites a full-window mono UI overlay.
	}
#endif
	// The flag follows the setting on the next frame, so this matches how the game was drawn.
	const StereoscopyMode mode = (StereoscopyMode)g_Config.iStereoscopyMode;
	if (!gstate_c.Use(GPU_USE_STEREOSCOPY) || mode == StereoscopyMode::OFF) {
		return false;
	}
	FRect eyeFrame;
	GetStereoscopyLayout(mode, GetScreenFrame(config.bIgnoreScreenInsets, pixelWidth, pixelHeight), &eyeFrame, viewports);
	return true;
}

void PresentationCommon::SourceBlank() {
	DoRelease(srcTexture_);
	DoRelease(srcFramebuffer_);

	srcWidth_ = 0;
	srcHeight_ = 0;
}

// If texture == null, that means there's nothing to display, so we should show a black screen in CopyToOutput.
void PresentationCommon::SourceTexture(Draw::Texture *texture, int bufferWidth, int bufferHeight) {
	// AddRef before release and assign in case it's the same.
	texture->AddRef();

	DoRelease(srcTexture_);
	DoRelease(srcFramebuffer_);

	srcTexture_ = texture;
	srcWidth_ = bufferWidth;
	srcHeight_ = bufferHeight;
}

void PresentationCommon::SourceFramebuffer(Draw::Framebuffer *fb, int bufferWidth, int bufferHeight) {
	fb->AddRef();

	DoRelease(srcTexture_);
	DoRelease(srcFramebuffer_);

	srcFramebuffer_ = fb;
	srcWidth_ = bufferWidth;
	srcHeight_ = bufferHeight;
}

bool PresentationCommon::UseVRStereoSource() const {
#ifdef __LIBRETRO__
	if (lang_ == GLSL_VULKAN && Libretro::VRActive()) {
		return true;
	}
#endif
	return texColorStereo_ && !srcTexture_ && srcFramebuffer_ && srcFramebuffer_->Layers() > 1;
}

// Return value is if stereo binding succeeded.
bool PresentationCommon::BindSource(int binding, bool bindStereo) {
	if (stereoPost_) {
		// What the post shaders see as the source.
		draw_->BindFramebufferAsTexture(stereoComposite_, binding, Draw::Aspect::COLOR_BIT, 0);
		return false;
	} else if (srcTexture_) {
		draw_->BindTexture(binding, srcTexture_);
		return false;
	} else if (srcFramebuffer_) {
		if (bindStereo) {
			if (srcFramebuffer_->Layers() > 1) {
				draw_->BindFramebufferAsTexture(srcFramebuffer_, binding, Draw::Aspect::COLOR_BIT, Draw::ALL_LAYERS);
				return true;
			} else {
				// Single layer. This might be from a post shader and those don't yet support stereo.
				draw_->BindFramebufferAsTexture(srcFramebuffer_, binding, Draw::Aspect::COLOR_BIT, 0);
				return false;
			}
		} else {
			draw_->BindFramebufferAsTexture(srcFramebuffer_, binding, Draw::Aspect::COLOR_BIT, 0);
			return false;
		}
	} else {
		_assert_(false);
		return false;
	}
}

void PresentationCommon::UpdateUniforms(bool hasVideo) {
	hasVideo_ = hasVideo;
}

void PresentationCommon::RunPostshaderPasses(const DisplayLayoutConfig &config, OutputFlags flags, int uvRotation, float u0, float v0, float u1, float v1) {
	draw_->Invalidate(InvalidationFlags::CACHED_RENDER_STATE);

	postShaderOutput_ = nullptr;
	outputFlags_ = flags;

	// TODO: If shader objects have been created by now, we might have received errors.
	// GLES can have the shader fail later, shader->failed / shader->error.
	// This should auto-disable usePostShader_ and call ShowPostShaderError().

	bool useNearest = flags & OutputFlags::NEAREST;
	// Post shaders output one layer, which would show the left eye's view to both.
	const bool vrStereo = UseVRStereoSource();
	// A mono source (like a frame from RAM) is shown to both eyes, so the layout doesn't jump.
	stereoscopyOutput_ = gstate_c.Use(GPU_USE_STEREOSCOPY) && !(flags & OutputFlags::NATIVE_SIZE);
#if defined(ANDROID) && !defined(__LIBRETRO__)
	if (DisplayXR_Selected() && !DisplayXR_CanRender(draw_)) { stereoscopyOutput_ = false; }
#endif
	const bool postShaderAllowed = usePostShader_ && !vrStereo && !(flags & (OutputFlags::RB_SWIZZLE | OutputFlags::NO_POST_SHADER));
	// Post shaders run on both eyes composited, once they're built for this layout (see UpdatePostShader).
	stereoPost_ = stereoscopyOutput_ && postShaderAllowed && stereoComposite_ && stereoPostMode_ == (StereoscopyMode)g_Config.iStereoscopyMode;

	const bool usePostShader = postShaderAllowed && (!stereoscopyOutput_ || stereoPost_);
	const bool isFinalAtOutputResolution = usePostShader && postShaderFramebuffers_.size() < postShaderPipelines_.size();
	int lastWidth = srcWidth_;
	int lastHeight = srcHeight_;

	const OutputParams output = GetOutput();
	int pixelWidth = output.pixelWidth;
	int pixelHeight = output.pixelHeight;

	// These are the output coordinates.
	FRect frame = GetScreenFrame(config.bIgnoreScreenInsets, output);
	if (stereoscopyOutput_) {
		GetStereoscopyLayout((StereoscopyMode)g_Config.iStereoscopyMode, frame, &frame, stereoViewports_);
		pixelWidth = (int)frame.w;
		pixelHeight = (int)frame.h;
	} else if (UseCardboard(config)) {
		// Note: In cardboard mode, we halve the width here to compensate
		// for splitting the window in half, while still reusing normal centering.
		frame.w /= 2.0;
		pixelWidth /= 2;
	}
	CalculateDisplayOutputRect(config, &rc_, 480.0f, 272.0f, frame, uvRotation, output.orientation);
	if (flags & OutputFlags::NATIVE_SIZE) {
		// Top-left, or bottom-left in a flipped (OpenGL) backbuffer.
		rc_.w = (u1 - u0) * srcWidth_;
		rc_.h = (v1 - v0) * srcHeight_;
		rc_.x = 0.0f;
		rc_.y = (flags & OutputFlags::BACKBUFFER_FLIPPED) ? pixelHeight - rc_.h : 0.0f;
	}

	if (!srcTexture_ && !srcFramebuffer_) {
		// Presenting blank, no need to run post shaders. But we did compute the output rect.
		stereoPost_ = false;
		return;
	}

	// To make buffer updates easier, we use one array of verts.
	int postVertsOffset = (int)sizeof(Vertex) * 4;

	float finalU0 = u0, finalU1 = u1, finalV0 = v0, finalV1 = v1;

	if (usePostShader && (stereoPost_ || !(isFinalAtOutputResolution && postShaderPipelines_.size() == 1))) {
		// The final blit will thus use the full texture.
		finalU0 = 0.0f;
		finalV0 = 0.0f;
		finalU1 = 1.0f;
		finalV1 = 1.0f;
	}

	// Our vertex buffer is split into four parts, with four vertices each:
	// 0-3: The final blit vertices (needs to handle cropping the input ONLY if post-processing is not enabled)
	// 4-7: Post-processing, other passes
	// 8-11: Post-processing, first pass (needs to handle cropping the input image, if wrong dimensions)
	// 12-15: With stereoscopy and post-processing, the final blit of the right eye (0-3 do the left)
	Vertex verts[16] = {
		{ rc_.x, rc_.y, 0, finalU0, finalV0, 0xFFFFFFFF }, // TL
		{ rc_.x + rc_.w, rc_.y, 0, finalU1, finalV0, 0xFFFFFFFF }, // TR
		{ rc_.x, rc_.y + rc_.h, 0, finalU0, finalV1, 0xFFFFFFFF }, // BL
		{ rc_.x + rc_.w, rc_.y + rc_.h, 0, finalU1, finalV1, 0xFFFFFFFF }, // BR
	};

	// Rescale X, Y to normalized coordinate system.
	float invDestW = 2.0f / pixelWidth;
	float invDestH = 2.0f / pixelHeight;
	for (int i = 0; i < 4; i++) {
		verts[i].x = verts[i].x * invDestW - 1.0f;
		verts[i].y = verts[i].y * invDestH - 1.0f;
	}

	if (uvRotation != ROTATION_LOCKED_HORIZONTAL) {
		struct {
			float u;
			float v;
		} temp[4];
		int rotation = 0;
		// Vertical and Vertical180 needed swapping after we changed the coordinate system.
		switch (uvRotation) {
		case ROTATION_LOCKED_HORIZONTAL180: rotation = 2; break;
		case ROTATION_LOCKED_VERTICAL: rotation = 3; break;
		case ROTATION_LOCKED_VERTICAL180: rotation = 1; break;
		}

		// If we flipped, we rotate the other way.
		if ((flags & OutputFlags::BACKBUFFER_FLIPPED)) {
			if ((rotation & 1) != 0)
				rotation ^= 2;
		}

		static int rotLookup[4] = {0, 1, 3, 2};

		for (int i = 0; i < 4; i++) {
			int otherI = rotLookup[(rotLookup[i] + rotation) & 3];
			temp[i].u = verts[otherI].u;
			temp[i].v = verts[otherI].v;
		}
		for (int i = 0; i < 4; i++) {
			verts[i].u = temp[i].u;
			verts[i].v = temp[i].v;
		}
	}

#ifdef __LIBRETRO__
	for (int i = 0; i < 4; ++i) {
		vrUV_[i][0] = verts[i].u;
		vrUV_[i][1] = verts[i].v;
	}
#endif
	if (isFinalAtOutputResolution) {
		// In this mode, we ignore the output's rotation matrix. Apply manually.
		if (output.rotation != DisplayRotation::ROTATE_0) {
			for (int i = 0; i < 4; i++) {
				Lin::Vec3 v(verts[i].x, verts[i].y, verts[i].z);
				// Backwards notation, should fix that...
				v = v * output.rotMatrix;
				verts[i].x = v.x;
				verts[i].y = v.y;
			}
		}
	}

	if (flags & OutputFlags::PILLARBOX) {
		for (int i = 0; i < 4; i++) {
			// Looks about right.
			verts[i].x *= 0.75f;
		}
	}

	// Finally, we compensate the y vertex positions for the backbuffer for any flipping.
	if (flags & OutputFlags::BACKBUFFER_FLIPPED) {
		for (int i = 0; i < 4; i++) {
			verts[i].y = -verts[i].y;
		}
	}

	if (stereoPost_) {
		// Each eye shows its half of the composite. The UVs above are 0 or 1, whatever the rotation.
		const bool sideBySide = StereoscopySideBySide(stereoPostMode_);
		for (int i = 0; i < 4; i++) {
			verts[i + 12] = verts[i];
			float &left = sideBySide ? verts[i].u : verts[i].v;
			float &right = sideBySide ? verts[i + 12].u : verts[i + 12].v;
			left *= 0.5f;
			right = 0.5f + right * 0.5f;
		}
	}

	// Grab the previous framebuffer early so we can change previousIndex_ when we want.
	Draw::Framebuffer *previousFramebuffer = previousFramebuffers_.empty() ? nullptr : previousFramebuffers_[previousIndex_];

	const auto performShaderPass = [&](const ShaderInfo *shaderInfo, Draw::Framebuffer *postShaderFramebuffer, Draw::Pipeline *postShaderPipeline, int vertsOffset) {
		if (postShaderOutput_) {
			draw_->BindFramebufferAsTexture(postShaderOutput_, 0, Draw::Aspect::COLOR_BIT, 0);
		} else {
			BindSource(0, false);
		}
		BindSource(1, false);
		if (shaderInfo->usePreviousFrame)
			draw_->BindFramebufferAsTexture(previousFramebuffer, 2, Draw::Aspect::COLOR_BIT, 0);

		int nextWidth, nextHeight;
		draw_->GetFramebufferDimensions(postShaderFramebuffer, &nextWidth, &nextHeight);
		Draw::Viewport viewport{0, 0, (float)nextWidth, (float)nextHeight, 0.0f, 1.0f};
		draw_->SetViewport(viewport);
		draw_->SetScissorRect(0, 0, nextWidth, nextHeight);

		PostShaderUniforms uniforms;
		CalculatePostShaderUniforms(lastWidth, lastHeight, nextWidth, nextHeight, shaderInfo, &uniforms);

		draw_->BindPipeline(postShaderPipeline);
		draw_->UpdateDynamicUniformBuffer(&uniforms, sizeof(uniforms));

		Draw::SamplerState *sampler = useNearest || shaderInfo->isUpscalingFilter ? samplerNearest_ : samplerLinear_;
		draw_->BindSamplerStates(0, 1, &sampler);
		draw_->BindSamplerStates(1, 1, &sampler);
		if (shaderInfo->usePreviousFrame)
			draw_->BindSamplerStates(2, 1, &sampler);

		draw_->BindVertexBuffer(vdata_, vertsOffset);
		draw_->Draw(4, 0);

		postShaderOutput_ = postShaderFramebuffer;
		lastWidth = nextWidth;
		lastHeight = nextHeight;
	};

	if (usePostShader) {
		// When we render to temp framebuffers during post, we switch position, not UV.
		float y0 = -1.0f;
		float y1 = 1.0f;
		verts[4] = {-1.0f, y0, 0.0f, 0.0f, 0.0f, 0xFFFFFFFF}; // TL
		verts[5] = {1.0f, y0, 0.0f, 1.0f, 0.0f, 0xFFFFFFFF}; // TR
		verts[6] = {-1.0f, y1, 0.0f, 0.0f, 1.0f, 0xFFFFFFFF}; // BL
		verts[7] = {1.0f, y1, 0.0f, 1.0f, 1.0f, 0xFFFFFFFF}; // BR

		// Now, adjust for the desired input rectangle.
		verts[8] = {-1.0f, y0, 0.0f, u0, v0, 0xFFFFFFFF}; // TL
		verts[9] = {1.0f, y0, 0.0f, u1, v0, 0xFFFFFFFF}; // TR
		verts[10] = {-1.0f, y1, 0.0f, u0, v1, 0xFFFFFFFF}; // BL
		verts[11] = {1.0f, y1, 0.0f, u1, v1, 0xFFFFFFFF}; // BR

		draw_->UpdateBuffer(vdata_, (const uint8_t *)verts, 0, sizeof(verts), Draw::UPDATE_DISCARD);

		if (stereoPost_) {
			// Put the eyes side by side (or top and bottom), cropped, for the post shaders to run on.
			int w, h;
			draw_->GetFramebufferDimensions(stereoComposite_, &w, &h);
			draw_->BindFramebufferAsRenderTarget(stereoComposite_, { Draw::RPAction::DONT_CARE, Draw::RPAction::DONT_CARE, Draw::RPAction::DONT_CARE }, "StereoComposite");
			draw_->BindPipeline(texColor_);
			Draw::VsTexColUB ub{};
			memcpy(ub.WorldViewProj, Lin::Matrix4x4::identity().m, sizeof(ub.WorldViewProj));
			draw_->UpdateDynamicUniformBuffer(&ub, sizeof(ub));
			Draw::SamplerState *sampler = useNearest || postShaderInfo_[0].isUpscalingFilter ? samplerNearest_ : samplerLinear_;
			draw_->BindSamplerStates(0, 1, &sampler);
			draw_->BindVertexBuffer(vdata_, (int)sizeof(Vertex) * 8);
			draw_->SetScissorRect(0, 0, w, h);
			const bool sideBySide = StereoscopySideBySide(stereoPostMode_);
			for (int eye = 0; eye < 2; eye++) {
				const int layer = g_Config.bStereoscopySwapEyes ? 1 - eye : eye;
				if (srcFramebuffer_) {
					draw_->BindFramebufferAsTexture(srcFramebuffer_, 0, Draw::Aspect::COLOR_BIT, srcFramebuffer_->Layers() > 1 ? layer : 0);
				} else {
					draw_->BindTexture(0, srcTexture_);
				}
				Draw::Viewport viewport{ sideBySide ? eye * w * 0.5f : 0.0f, sideBySide ? 0.0f : eye * h * 0.5f, sideBySide ? w * 0.5f : (float)w, sideBySide ? (float)h : h * 0.5f, 0.0f, 1.0f };
				draw_->SetViewport(viewport);
				draw_->Draw(4, 0);
			}
			postShaderOutput_ = stereoComposite_;
			lastWidth = w;
			lastHeight = h;
		}

		for (size_t i = 0; i < postShaderFramebuffers_.size(); ++i) {
			Draw::Pipeline *postShaderPipeline = postShaderPipelines_[i];
			const ShaderInfo *shaderInfo = &postShaderInfo_[i];
			Draw::Framebuffer *postShaderFramebuffer = postShaderFramebuffers_[i];
			if (!isFinalAtOutputResolution && i == postShaderFramebuffers_.size() - 1 && !previousFramebuffers_.empty()) {
				// This is the last pass and we're going direct to the backbuffer after this.
				// Redirect output to a separate framebuffer to keep the previous frame.
				previousIndex_++;
				if (previousIndex_ >= (int)previousFramebuffers_.size())
					previousIndex_ = 0;
				postShaderFramebuffer = previousFramebuffers_[previousIndex_];
			}

			draw_->BindFramebufferAsRenderTarget(postShaderFramebuffer, {Draw::RPAction::DONT_CARE, Draw::RPAction::DONT_CARE, Draw::RPAction::DONT_CARE}, "PostShader");

			// Pick vertices 8-11 for the first pass, unless the composite did the cropping.
			int vertOffset = i == 0 && !stereoPost_ ? (int)sizeof(Vertex) * 8 : (int)sizeof(Vertex) * 4;
			performShaderPass(shaderInfo, postShaderFramebuffer, postShaderPipeline, vertOffset);
		}

		if (isFinalAtOutputResolution && postShaderInfo_.back().isUpscalingFilter)
			useNearest = true;
	} else {
		// Only need to update the first four verts, the rest are unused.
		draw_->UpdateBuffer(vdata_, (const uint8_t *)verts, 0, postVertsOffset, Draw::UPDATE_DISCARD);
	}

	// If we need to save the previous frame, we have to save any final pass in a framebuffer.
	if (isFinalAtOutputResolution && !previousFramebuffers_.empty()) {
		Draw::Pipeline *postShaderPipeline = postShaderPipelines_.back();
		const ShaderInfo *shaderInfo = &postShaderInfo_.back();

		// Pick the next to render to.
		previousIndex_++;
		if (previousIndex_ >= (int)previousFramebuffers_.size())
			previousIndex_ = 0;
		Draw::Framebuffer *postShaderFramebuffer = previousFramebuffers_[previousIndex_];

		draw_->BindFramebufferAsRenderTarget(postShaderFramebuffer, {Draw::RPAction::CLEAR, Draw::RPAction::DONT_CARE, Draw::RPAction::DONT_CARE}, "InterFrameBlit");
		performShaderPass(shaderInfo, postShaderFramebuffer, postShaderPipeline, postVertsOffset);
	}
}

// Uploads the active overlay image if it changed. Returns false if there's nothing to draw.
bool PresentationCommon::PrepareOverlayTexture(const OverlayLayout &layout) {
	if (overlayTexture_ && overlayGeneration_ == layout.generation)
		return true;

	DoRelease(overlayTexture_);
	overlayGeneration_ = layout.generation;

	std::shared_ptr<const OverlayImage> image = GetOverlayImage(layout.generation);
	if (!image || image->width <= 0 || image->height <= 0) {
		// Changed under us, we'll get it next frame. Retry then.
		overlayGeneration_ = 0xFFFFFFFF;
		return false;
	}

	Draw::TextureDesc desc{};
	desc.type = Draw::TextureType::LINEAR2D;
	desc.format = Draw::DataFormat::R8G8B8A8_UNORM;
	desc.width = image->width;
	desc.height = image->height;
	desc.depth = 1;
	desc.mipLevels = 1;
	desc.generateMips = false;
	desc.tag = "overlay";
	desc.initData.push_back(image->rgba.data());
	overlayTexture_ = draw_->CreateTexture(desc);
	if (!overlayTexture_) {
		ERROR_LOG(Log::G3D, "Failed to create overlay texture (%dx%d)", image->width, image->height);
		return false;
	}
	return true;
}

void PresentationCommon::DrawOverlay(const OverlayLayout &layout, const OutputParams &output) {
	const float x0 = layout.overlayX, y0 = layout.overlayY;
	const float x1 = x0 + layout.overlayW, y1 = y0 + layout.overlayH;
	Vertex verts[4] = {
		{ x0, y0, 0, 0.0f, 0.0f, 0xFFFFFFFF }, // TL
		{ x1, y0, 0, 1.0f, 0.0f, 0xFFFFFFFF }, // TR
		{ x0, y1, 0, 0.0f, 1.0f, 0xFFFFFFFF }, // BL
		{ x1, y1, 0, 1.0f, 1.0f, 0xFFFFFFFF }, // BR
	};
	// Same transform as the game quad, see RunPostshaderPasses.
	const bool flipped = draw_->GetDeviceCaps().coordConvention == CoordConvention::OpenGL;
	const float invDestW = 2.0f / output.pixelWidth;
	const float invDestH = 2.0f / output.pixelHeight;
	for (auto &v : verts) {
		v.x = v.x * invDestW - 1.0f;
		v.y = v.y * invDestH - 1.0f;
		if (flipped)
			v.y = -v.y;
	}
	draw_->UpdateBuffer(overlayVdata_, (const uint8_t *)verts, 0, sizeof(verts), Draw::UPDATE_DISCARD);

	draw_->BindPipeline(texColorBlend_);
	Draw::VsTexColUB ub{};
	memcpy(ub.WorldViewProj, output.rotMatrix.m, sizeof(float) * 16);
	draw_->UpdateDynamicUniformBuffer(&ub, sizeof(ub));
	draw_->BindTexture(0, overlayTexture_);
	draw_->BindSamplerStates(0, 1, &samplerLinear_);
	draw_->BindVertexBuffer(overlayVdata_, 0);

	Draw::Viewport viewport{ 0.0f, 0.0f, (float)output.pixelWidth, (float)output.pixelHeight, 0.0f, 1.0f };
	draw_->SetViewport(viewport);
	draw_->SetScissorRect(0, 0, output.pixelWidth, output.pixelHeight);
	draw_->Draw(4, 0);
}

#ifdef __LIBRETRO__
void PresentationCommon::CopyToLibretroVR() {
	if (!srcFramebuffer_ && !srcTexture_) {
		presentedThisFrame_ = true;
		return;
	}

	const int width = PSP_CoreParameter().pixelWidth;
	const int height = PSP_CoreParameter().pixelHeight;
	const bool cylinder = Libretro::VRCanvasActive();
	const int mode = (cylinder ? 2 : 0) + (srcFramebuffer_ && srcFramebuffer_->Layers() > 1 ? 1 : 0);

	if (vrPresentationMode_ != mode) {
		INFO_LOG(Log::G3D, "Libretro Vulkan VR: %s, %d source layer(s), %dx%d per eye",
			cylinder ? "tracked cylinder" : "stereo screen", srcFramebuffer_ ? srcFramebuffer_->Layers() : 1, width, height);
		vrPresentationMode_ = mode;
	}

	std::vector<Vertex> vertices;
	if (cylinder) {
		constexpr int segments = 64;
		constexpr float pi = 3.14159265358979323846f;
		const float canvasHeight = pi / Libretro::VRCanvasAspect();
		const float distance = g_Config.fCanvas3DDistance / 4.0f - 1.0f;

		for (int i = 0; i <= segments; ++i) {
			const float u = (float)i / segments;
			const float angle = (u - 0.5f) * pi;

			for (int row = 0; row < 2; ++row) {
				const float *left = vrUV_[row * 2];
				const float *right = vrUV_[row * 2 + 1];
				vertices.push_back({2.0f * sinf(angle), (1 - row * 2) * canvasHeight,
					-2.0f * cosf(angle) - distance,
					left[0] + u * (right[0] - left[0]), left[1] + u * (right[1] - left[1]), 0xFFFFFFFF});
			}
		}
	}

	draw_->BindPipeline((outputFlags_ & OutputFlags::RB_SWIZZLE) ? texColorRBSwizzle_ : texColor_);
	Draw::SamplerState *sampler = (outputFlags_ & OutputFlags::NEAREST) ? samplerNearest_ : samplerLinear_;
	draw_->BindSamplerStates(0, 1, &sampler);

	for (int eye = 0; eye < 2; ++eye) {
		if (srcFramebuffer_) {
			draw_->BindFramebufferAsTexture(srcFramebuffer_, 0, Draw::Aspect::COLOR_BIT,
				srcFramebuffer_->Layers() > 1 ? eye : 0);
		} else {
			draw_->BindTexture(0, srcTexture_);
		}

		Draw::VsTexColUB ub{};
		if (cylinder) {
			Libretro::GetVRCanvasMatrix(eye, ub.WorldViewProj);
			// OpenGL clip coordinates to Vulkan (Y down, depth 0..1).
			for (int col = 0; col < 4; ++col) {
				ub.WorldViewProj[col * 4 + 1] *= -1.0f;
				ub.WorldViewProj[col * 4 + 2] = (ub.WorldViewProj[col * 4 + 2] + ub.WorldViewProj[col * 4 + 3]) * 0.5f;
			}
		} else {
			const OutputParams output = GetOutput();
			memcpy(ub.WorldViewProj, output.rotMatrix.m, sizeof(ub.WorldViewProj));
		}

		draw_->UpdateDynamicUniformBuffer(&ub, sizeof(ub));
		draw_->SetViewport({(float)(eye * width), 0.0f, (float)width, (float)height, 0.0f, 1.0f});
		draw_->SetScissorRect(eye * width, 0, width, height);

		if (cylinder) {
			draw_->DrawUP(vertices.data(), (int)vertices.size());
		} else {
			draw_->BindVertexBuffer(vdata_, 0);
			draw_->Draw(4, 0);
		}
	}

	DoRelease(srcFramebuffer_);
	DoRelease(srcTexture_);
	draw_->Invalidate(InvalidationFlags::CACHED_RENDER_STATE);

	presentedThisFrame_ = true;
}
#endif

void PresentationCommon::CopyToOutput(const DisplayLayoutConfig &config) {
#ifdef __LIBRETRO__
	if (lang_ == GLSL_VULKAN && Libretro::VRActive()) {
		CopyToLibretroVR();
		return;
	}
	vrPresentationMode_ = -1;
#endif
	bool useNearest = outputFlags_ & OutputFlags::NEAREST;
	const bool vrStereo = UseVRStereoSource();

	const bool usePostShader = usePostShader_ && (!stereoscopyOutput_ || stereoPost_) && !vrStereo && !(outputFlags_ & (OutputFlags::RB_SWIZZLE | OutputFlags::NO_POST_SHADER));
	const bool isFinalAtOutputResolution = usePostShader && postShaderFramebuffers_.size() < postShaderPipelines_.size();
	int lastWidth = srcWidth_;
	int lastHeight = srcHeight_;
	// The final pass reads what the earlier passes made, not the source.
	if (postShaderOutput_) {
		draw_->GetFramebufferDimensions(postShaderOutput_, &lastWidth, &lastHeight);
	}

	const OutputParams output = GetOutput();
	draw_->SetScissorRect(0, 0, output.pixelWidth, output.pixelHeight);

	OverlayLayout overlay;
	bool drawOverlay = false;
	// An overlay is laid out for the whole frame, not per eye.
	if (!(outputFlags_ & OutputFlags::NATIVE_SIZE) && !stereoscopyOutput_) {
		const FRect frame = GetScreenFrame(config.bIgnoreScreenInsets, output);
		drawOverlay = GetOverlayLayout(config, frame.x, frame.y, frame.w, frame.h, &overlay) && PrepareOverlayTexture(overlay);
	}

	if (!srcFramebuffer_ && !srcTexture_) {
		// Bound blank. Keep the overlay up though, so it doesn't flicker during loading and such.
		if (drawOverlay) {
			DrawOverlay(overlay, output);
			draw_->Invalidate(InvalidationFlags::CACHED_RENDER_STATE);
		}
		presentedThisFrame_ = true;
		return;
	}

	bool stereoOutput = stereoscopyOutput_;
#if defined(ANDROID) && !defined(__LIBRETRO__)
	const bool displayXR = stereoOutput && !hasOutputOverride_ && lang_ == GLSL_VULKAN &&
		DisplayXR_Begin(draw_, output.pixelWidth, output.pixelHeight);
	// One eye, for this frame only: a frame presented again without a new flip may weave.
	if (DisplayXR_Selected() && !displayXR) { stereoOutput = false; }
#endif
	if (drawOverlay && overlay.layer == OverlayLayer::Background) {
		DrawOverlay(overlay, output);
	}

	Draw::Pipeline *pipeline = (outputFlags_ & OutputFlags::RB_SWIZZLE) ? texColorRBSwizzle_ : texColor_;

	if (vrStereo) {
		draw_->BindPipeline(texColorStereo_);
		BindSource(0, true);
	} else {
		if (isFinalAtOutputResolution && previousFramebuffers_.empty()) {
			pipeline = postShaderPipelines_.back();
		}

		draw_->BindPipeline(pipeline);
		if (postShaderOutput_) {
			draw_->BindFramebufferAsTexture(postShaderOutput_, 0, Draw::Aspect::COLOR_BIT, 0);
		} else {
			BindSource(0, false);
		}
	}
	BindSource(1, false);

	PostShaderUniforms uniforms;
	if (isFinalAtOutputResolution && previousFramebuffers_.empty()) {
		CalculatePostShaderUniforms(lastWidth, lastHeight, (int)rc_.w, (int)rc_.h, &postShaderInfo_.back(), &uniforms);
		draw_->UpdateDynamicUniformBuffer(&uniforms, sizeof(uniforms));
		previousUniforms_ = uniforms;
	} else {
		Draw::VsTexColUB ub{};
		memcpy(ub.WorldViewProj, output.rotMatrix.m, sizeof(float) * 16);
		draw_->UpdateDynamicUniformBuffer(&ub, sizeof(ub));
	}

	draw_->BindVertexBuffer(vdata_, 0);

	Draw::SamplerState *sampler = useNearest ? samplerNearest_ : samplerLinear_;
	draw_->BindSamplerStates(0, 1, &sampler);
	draw_->BindSamplerStates(1, 1, &sampler);

	auto setViewport = [&](float x, float y, float w, float h) {
		Draw::Viewport viewport{ x, y, w, h, 0.0f, 1.0f };
		draw_->SetViewport(viewport);
	};

	CardboardSettings cardboardSettings;
	GetCardboardSettings(config, output, &cardboardSettings);
	if (stereoOutput) {
		for (int eye = 0; eye < 2; eye++) {
			const int layer = g_Config.bStereoscopySwapEyes ? 1 - eye : eye;
			if (stereoPost_) {
				// The eye's half of what the post shaders made, swapped already.
				draw_->BindVertexBuffer(vdata_, eye == 0 ? 0 : (int)sizeof(Vertex) * 12);
			} else if (srcFramebuffer_) {
				draw_->BindFramebufferAsTexture(srcFramebuffer_, 0, Draw::Aspect::COLOR_BIT, srcFramebuffer_->Layers() > 1 ? layer : 0);
			}
			const FRect &vp = stereoViewports_[eye];
			setViewport(vp.x, vp.y, vp.w, vp.h);
			draw_->Draw(4, 0);
		}
	} else if (cardboardSettings.enabled) {
		// TODO: This could actually support stereo now, with an appropriate shader.

		// This is what the left eye sees.
		setViewport(cardboardSettings.leftEyeXPosition, cardboardSettings.screenYPosition, cardboardSettings.screenWidth, cardboardSettings.screenHeight);
		draw_->Draw(4, 0);

		// And this is the right eye, unless they're a pirate.
		setViewport(cardboardSettings.rightEyeXPosition, cardboardSettings.screenYPosition, cardboardSettings.screenWidth, cardboardSettings.screenHeight);
		draw_->Draw(4, 0);
	} else {
		setViewport(0.0f, 0.0f, (float)output.pixelWidth, (float)output.pixelHeight);
		draw_->Draw(4, 0);
	}

	if (drawOverlay && overlay.layer == OverlayLayer::Foreground) {
		DrawOverlay(overlay, output);
	}

	DoRelease(srcFramebuffer_);
	DoRelease(srcTexture_);

#if defined(ANDROID) && !defined(__LIBRETRO__)
	if (displayXR) {
		DisplayXR_End(draw_);
	}
#endif
	// Unbinds all textures and samplers too, needed since sometimes a MakePixelTexture is deleted etc.
	draw_->Invalidate(InvalidationFlags::CACHED_RENDER_STATE);

	presentedThisFrame_ = true;
}

void PresentationCommon::CalculateRenderResolution(const DisplayLayoutConfig &config, int *width, int *height, int *scaleFactor, bool *upscaling, bool *ssaa) const {
	// Check if postprocessing shader is doing upscaling as it requires native resolution
	std::vector<const ShaderInfo *> shaderInfo;
	if (!g_Config.vPostShaderNames.empty()) {
		ReloadAllPostShaderInfo(draw_);
		RemoveUnknownPostShaders(&g_Config.vPostShaderNames);
		FixPostShaderOrder(&g_Config.vPostShaderNames);
		shaderInfo = GetFullPostShadersChain(g_Config.vPostShaderNames);
	}

	bool firstIsUpscalingFilter = shaderInfo.empty() ? false : shaderInfo.front()->isUpscalingFilter;
	int firstSSAAFilterLevel = shaderInfo.empty() ? 0 : shaderInfo.front()->SSAAFilterLevel;

	// In auto mode (zoom == 0), round up to an integer zoom factor for the render size.
	int zoom = g_Config.iInternalResolution;
	if (zoom == 0 || firstSSAAFilterLevel >= 2) {
		// auto mode, use the longest dimension
		int pixelWidth = PSP_CoreParameter().pixelWidth;
		int pixelHeight = PSP_CoreParameter().pixelHeight;
		if (gstate_c.Use(GPU_USE_STEREOSCOPY)) {
			// The frame each eye is laid out in. The full modes halve it, the half ones squeeze it afterwards,
			// which keeps the full resolution along the other axis.
			FRect eyeFrame, viewports[2];
			GetStereoscopyLayout((StereoscopyMode)g_Config.iStereoscopyMode, { 0.0f, 0.0f, (float)pixelWidth, (float)pixelHeight }, &eyeFrame, viewports);
			pixelWidth = (int)eyeFrame.w;
			pixelHeight = (int)eyeFrame.h;
		}
		if (!config.InternalRotationIsPortrait()) {
			zoom = (pixelWidth + 479) / 480;
		} else {
			zoom = (pixelHeight + 479) / 480;
		}
		if (firstSSAAFilterLevel >= 2)
			zoom *= firstSSAAFilterLevel;
	}
	if (zoom <= 1 || firstIsUpscalingFilter)
		zoom = 1;

	if (upscaling) {
		*upscaling = firstIsUpscalingFilter;
		for (auto &info : shaderInfo) {
			*upscaling = *upscaling || info->isUpscalingFilter;
		}
	}
	if (ssaa) {
		*ssaa = firstSSAAFilterLevel >= 2;
		for (auto &info : shaderInfo) {
			*ssaa = *ssaa || info->SSAAFilterLevel >= 2;
		}
	}

	if (IsVREnabled()) {
		*width = 480 * zoom;
		*height = 480 * zoom;
	} else {
		// Note: We previously checked g_Config.IsPortrait (internal rotation) here but that was wrong -
		// we still render at 480x272 * zoom.
		*width = 480 * zoom;
		*height = 272 * zoom;
	}

	*scaleFactor = zoom;
}
