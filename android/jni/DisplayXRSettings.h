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

#include <algorithm>
#include "Common/UI/PopupScreens.h"
#include "Common/UI/ScreenManager.h"
#include "Common/Data/Text/I18n.h"
#include "Common/System/NativeApp.h"
#include "Core/Config.h"
#include "android/jni/DisplayXR.h"

// The persisted values stay append-only; the Android menu has its own order.
class DisplayXRModeChoice : public UI::PopupMultiChoice {
public:
	DisplayXRModeChoice(ScreenManager *manager)
		: PopupMultiChoice(&index_, GetI18NCategory(I18NCat::GRAPHICS)->T("Stereoscopic 3D Mode"), nullptr, 0, 0, I18NCat::GRAPHICS, manager), manager_(manager) {
		RefreshChoices();
	}
	void Update() override {
		const Screen *top = manager_->topScreen();
		if (!top || std::string_view(top->tag()) != "listpopup") {
			RefreshChoices();
		}
		PopupMultiChoice::Update();
	}

protected:
	void RefreshChoices() override {
		names_[0] = DisplayXR_Available() ? "DisplayXR" : "DisplayXR (unavailable)";
		values_ = DisplayXR_ModeChoices();
		const bool withDisplayXR = values_.front() == 5;
		choices_ = names_ + (withDisplayXR ? 0 : 1);
		numChoices_ = (int)values_.size();
		auto it = std::find(values_.begin(), values_.end(), g_Config.iStereoscopyMode);
		index_ = it != values_.end() ? (int)(it - values_.begin()) : (withDisplayXR ? 1 : 0);
	}
	bool PostChoiceCallback(int index) override {
		if (index < 0 || index >= (int)values_.size()) {
			return false;
		}
		if (values_[index] == 5 && !DisplayXR_Available() && !DisplayXR_Selected()) {
			RefreshChoices();
			return false;
		}
		g_Config.iStereoscopyMode = values_[index];
		if (values_[index] == 5) { DisplayXR_RequestRetry(); }
		DisplayXR_ModeChanged();
		System_PostUIMessage(UIMessage::GPU_CONFIG_CHANGED);
		return true;
	}

private:
	const char *names_[6] = { "DisplayXR", "Off", "Half SBS (Side-by-Side)", "Full SBS (Side-by-Side)", "Half TAB (Top-and-Bottom)", "Full TAB (Top-and-Bottom)" };
	ScreenManager *manager_;
	int index_ = 0;
	std::vector<int> values_;
};

class DisplayXRStatusView : public UI::TextView {
public:
	DisplayXRStatusView() : TextView(DisplayXR_GetReason(), FLAG_WRAP_TEXT, false,
		new UI::LinearLayoutParams(UI::FILL_PARENT, UI::WRAP_CONTENT)) {}
	void Update() override {
		SetText(DisplayXR_GetReason());
		TextView::Update();
	}
};
