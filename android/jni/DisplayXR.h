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

#pragma once

#include <string>
#include <vector>

namespace Draw { class DrawContext; }
class VulkanContext;

enum class DisplayXRStatus { UNSUPPORTED, MISSING, CHECKING, AVAILABLE, NEEDS_SETUP, FAILED };

DisplayXRStatus DisplayXR_GetStatus();
std::string DisplayXR_GetReason();
bool DisplayXR_Available();
bool DisplayXR_Selected();
bool DisplayXR_Active();
bool DisplayXR_CanRender(Draw::DrawContext *draw);
void DisplayXR_Update(Draw::DrawContext *draw = nullptr);
void DisplayXR_RequestRetry();
void DisplayXR_Gameplay(bool running);
void DisplayXR_ModeChanged();
std::vector<int> DisplayXR_ModeChoices();
// Returns false when the game must use its normal output (including mono fallback).
bool DisplayXR_Begin(Draw::DrawContext *draw, int width, int height);
void DisplayXR_End(Draw::DrawContext *draw);
void DisplayXR_Destroy(VulkanContext *vulkan);
void DisplayXR_DeviceExtensions(VulkanContext *vulkan);
// Render thread, before the ordinary swapchain present. The semaphore is consumed and re-signalled.
void DisplayXR_Present(VulkanContext *vulkan, int frame, void *image, void *semaphore);
