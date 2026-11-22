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

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "Common/Data/Color/RGBAUtil.h"
#include "Common/Data/Text/I18n.h"
#include "Common/Render/DrawBuffer.h"
#include "Common/System/System.h"
#include "Common/TimeUtil.h"
#include "Common/UI/Context.h"
#include "Common/UI/View.h"
#include "Core/Config.h"
#include "Core/ConfigSettings.h"
#include "Core/ConfigValues.h"
#include "Core/ELF/ParamSFO.h"
#include "Core/HLE/sceCtrl.h"
#include "Core/HLE/sceUtility.h"
#include "Core/KeyMap.h"
#include "Core/System.h"
#include "GPU/Common/PresentationCommon.h"
#include "UI/StereoscopyOverlay.h"

bool StereoscopyAvailable(Draw::DrawContext *draw) {
	return StereoscopySupported(draw) && !g_Config.bSkipBufferEffects;
}

bool StereoscopyOn(Draw::DrawContext *draw) {
	return StereoscopyAvailable(draw) && g_Config.iStereoscopyMode != (int)StereoscopyMode::OFF;
}

bool StereoscopyUIDetectionOn(Draw::DrawContext *draw) {
	return StereoscopyOn(draw) && g_Config.iStereoscopyUIDetectionMode != (int)StereoscopyUIDetection::OFF;
}

bool StereoscopyUIThresholdOn(Draw::DrawContext *draw) {
	const StereoscopyUIDetection detection = (StereoscopyUIDetection)g_Config.iStereoscopyUIDetectionMode;
	return StereoscopyUIDetectionOn(draw) && detection == StereoscopyUIDetection::ADVANCED;
}

// Sizes in UI units, at scale 1.
static const float MARGIN = 24.0f;
static const float PADDING = 14.0f;
static const float TITLE_HEIGHT = 30.0f;
static const float SUBTITLE_HEIGHT = 20.0f;
static const float HEADER_GAP = 10.0f;
static const float ROW_HEIGHT = 34.0f;
static const float ROW_GAP = 2.0f;
static const float DIVIDER_HEIGHT = 13.0f;
static const float HINT_GAP = 8.0f;
static const float HINT_HEIGHT = 24.0f;
static const float COLUMN_GAP = 20.0f;
static const float COLUMN_WIDTH = 440.0f;
static const float STRIP_WIDTH = 940.0f;
static const float ROW_INSET = 10.0f;
static const float CORNER_RADIUS = 12.0f;

// The strip puts the settings in two columns of this many rows.
static const int STRIP_ROWS = 5;

static const double REPEAT_DELAY = 0.35;
static const double REPEAT_INTERVAL = 0.06;
// Held this long, a slider moves ten steps at a time.
static const double FAST_REPEAT_AFTER = 1.5;

static const float AXIS_PRESS = 0.6f;
static const float AXIS_RELEASE = 0.4f;

StereoscopyOverlay::StereoscopyOverlay(Draw::DrawContext *draw) : draw_(draw) {
	auto add = [&](void *ptr, RowType type, float step, bool proportional, bool (*enabled)(Draw::DrawContext *)) {
		const ConfigSettingRef *ref = Config::FindSetting(ptr);
		_dbg_assert_(ref);
		if (ref) {
			rows_.push_back(Row{ ptr, ref, type, step, proportional, enabled });
		}
	};
	add(&g_Config.iStereoscopyMode, RowType::CHOICE, 0.0f, false, &StereoscopyAvailable);
	add(&g_Config.iStereoscopyDominantEye, RowType::CHOICE, 0.0f, false, &StereoscopyOn);
	add(&g_Config.bStereoscopySwapEyes, RowType::BOOL, 0.0f, false, &StereoscopyOn);
	add(&g_Config.fStereoscopySeparation, RowType::FLOAT, 0.5f, false, &StereoscopyOn);
	// Useful values differ a lot between games, so it steps by a share of the value.
	add(&g_Config.fStereoscopyConvergence, RowType::FLOAT, 0.01f, true, &StereoscopyOn);
	add(&g_Config.iStereoscopyUIDetectionMode, RowType::CHOICE, 0.0f, false, &StereoscopyOn);
	add(&g_Config.iStereoscopyUIDepth, RowType::INT, 1.0f, false, &StereoscopyUIDetectionOn);
	add(&g_Config.iStereoscopySecondLayerParallax, RowType::INT, 1.0f, false, &StereoscopyUIDetectionOn);
	add(&g_Config.iStereoscopyUIDetectionThreshold, RowType::INT, 1.0f, false, &StereoscopyUIThresholdOn);
	// Last, as it's the overlay's own (and not per game).
	add(&g_Config.iStereoscopyOverlayPosition, RowType::CHOICE, 0.0f, false, nullptr);

	selected_ = 0;
	if (!RowEnabled(rows_[selected_])) {
		MoveSelection(1);
	}
}

bool StereoscopyOverlay::RowEnabled(const Row &row) const {
	return !row.enabled || row.enabled(draw_);
}

StereoscopyOverlay::Action StereoscopyOverlay::ButtonToAction(int pspButton) const {
	const bool confirmWithCross = g_Config.iButtonPreference == PSP_SYSTEMPARAM_BUTTON_CROSS;
	switch (pspButton) {
	case CTRL_UP:
	case VIRTKEY_AXIS_Y_MAX:
		return Action::UP;
	case CTRL_DOWN:
	case VIRTKEY_AXIS_Y_MIN:
		return Action::DOWN;
	case CTRL_LEFT:
	case VIRTKEY_AXIS_X_MIN:
		return Action::LEFT;
	case CTRL_RIGHT:
	case VIRTKEY_AXIS_X_MAX:
		return Action::RIGHT;
	case CTRL_TRIANGLE:
		return Action::RESET;
	case CTRL_CROSS:
		return confirmWithCross ? Action::CONFIRM : Action::CLOSE;
	case CTRL_CIRCLE:
		return confirmWithCross ? Action::CLOSE : Action::CONFIRM;
	default:
		// Not Pause (Escape, L2): the overlay's own hotkey can include it, and would close the overlay
		// twice, reopening it.
		return Action::NONE;
	}
}

StereoscopyOverlay::Action StereoscopyOverlay::MappingToAction(const InputMapping &mapping) const {
	std::vector<int> pspButtons;
	if (KeyMap::InputMappingToPspButton(mapping, &pspButtons)) {
		for (int button : pspButtons) {
			Action action = ButtonToAction(button);
			if (action != Action::NONE) {
				return action;
			}
		}
	}
	return Action::NONE;
}

StereoscopyOverlay::Action StereoscopyOverlay::KeyToAction(const KeyInput &key) const {
	// The keys mapped to the PSP's buttons, letters included (unlike the menus, there's no search to type in).
	Action action = MappingToAction(InputMapping(key.deviceId, key.keyCode));
	if (action != Action::NONE) {
		return action;
	}
	switch (key.keyCode) {
	case NKCODE_DPAD_UP: return Action::UP;
	case NKCODE_DPAD_DOWN: return Action::DOWN;
	case NKCODE_DPAD_LEFT: return Action::LEFT;
	case NKCODE_DPAD_RIGHT: return Action::RIGHT;
	case NKCODE_ENTER:
	case NKCODE_NUMPAD_ENTER:
	case NKCODE_SPACE:
		return Action::CONFIRM;
	case NKCODE_ESCAPE:
	case NKCODE_BACK:
		return Action::CLOSE;
	case NKCODE_DEL:
	case NKCODE_FORWARD_DEL:
		return Action::RESET;
	default:
		return Action::NONE;
	}
}

// Sources of held inputs, to match a release to its press.
static uint64_t KeySource(InputDeviceID deviceId, int keyCode) {
	return ((uint64_t)deviceId << 32) | (uint32_t)keyCode;
}

static uint64_t AxisSource(InputDeviceID deviceId, int axisId, int direction) {
	return (1ULL << 63) | ((uint64_t)deviceId << 32) | ((uint32_t)axisId << 1) | (direction > 0 ? 1 : 0);
}

void StereoscopyOverlay::Key(const KeyInput &key) {
	if (key.flags & KeyInputFlags::CHAR) {
		return;
	}
	const uint64_t source = KeySource(key.deviceId, key.keyCode);
	if (key.flags & KeyInputFlags::DOWN) {
		// We repeat on our own, the same way for keys and pads.
		if (key.flags & KeyInputFlags::IS_REPEAT) {
			return;
		}
		Press(KeyToAction(key), source);
	}
	if (key.flags & KeyInputFlags::UP) {
		Release(source);
	}
}

void StereoscopyOverlay::Axis(const AxisInput &axis) {
	for (int direction : { -1, 1 }) {
		const uint64_t source = AxisSource(axis.deviceId, axis.axisId, direction);
		bool &pushed = axisPushed_[source];
		const float value = axis.value * direction;
		if (!pushed && value >= AXIS_PRESS) {
			pushed = true;
			Press(MappingToAction(InputMapping(axis.deviceId, axis.axisId, direction)), source);
		} else if (pushed && value <= AXIS_RELEASE) {
			pushed = false;
			Release(source);
		}
	}
}

void StereoscopyOverlay::Press(Action action, uint64_t source) {
	if (action == Action::NONE) {
		return;
	}
	Perform(action, false);
	const bool repeats = action == Action::UP || action == Action::DOWN || action == Action::LEFT || action == Action::RIGHT;
	if (repeats) {
		heldAction_ = action;
		heldSource_ = source;
		heldSince_ = time_now_d();
		nextRepeat_ = heldSince_ + REPEAT_DELAY;
	}
}

void StereoscopyOverlay::Release(uint64_t source) {
	if (heldAction_ != Action::NONE && source == heldSource_) {
		heldAction_ = Action::NONE;
	}
}

void StereoscopyOverlay::Update() {
	// The selected setting can get disabled by another, like the mode.
	if (!RowEnabled(rows_[selected_])) {
		MoveSelection(1);
	}
	if (heldAction_ == Action::NONE) {
		return;
	}
	// Once per interval, without catching up after a stall.
	const double now = time_now_d();
	if (now >= nextRepeat_) {
		Perform(heldAction_, true);
		nextRepeat_ = now + REPEAT_INTERVAL;
	}
}

void StereoscopyOverlay::Perform(Action action, bool repeat) {
	Row &row = rows_[selected_];
	switch (action) {
	case Action::UP:
		MoveSelection(-1);
		break;
	case Action::DOWN:
		MoveSelection(1);
		break;
	case Action::LEFT:
	case Action::RIGHT:
	{
		const int dir = action == Action::LEFT ? -1 : 1;
		if (!RowEnabled(row)) {
			break;
		}
		if (row.type == RowType::CHOICE || row.type == RowType::BOOL) {
			// Choices don't repeat, a held direction would race through them.
			if (!repeat) {
				Cycle(row, false, dir);
			}
		} else {
			Step(row, dir, repeat && time_now_d() - heldSince_ >= FAST_REPEAT_AFTER);
		}
		break;
	}
	case Action::CONFIRM:
		if (RowEnabled(row) && (row.type == RowType::CHOICE || row.type == RowType::BOOL)) {
			Cycle(row, true, 1);
		}
		break;
	case Action::RESET:
		if (RowEnabled(row)) {
			Reset(row);
		}
		break;
	case Action::CLOSE:
		wantsClose_ = true;
		break;
	default:
		break;
	}
}

void StereoscopyOverlay::MoveSelection(int dir) {
	const int count = (int)rows_.size();
	int index = selected_;
	for (int i = 0; i < count; i++) {
		index = (index + dir + count) % count;
		if (RowEnabled(rows_[index])) {
			selected_ = index;
			return;
		}
	}
}

void StereoscopyOverlay::Cycle(Row &row, bool wrap, int dir) {
	if (row.type == RowType::BOOL) {
		bool *value = (bool *)row.ptr;
		const bool newValue = wrap ? !*value : dir > 0;
		if (newValue != *value) {
			*value = newValue;
			Changed(row);
		}
		return;
	}
	const ConfigSettingInfo &info = row.ref->setting->Info();
	int *value = (int *)row.ptr;
	const int minValue = info.choiceMin;
	const int maxValue = info.choiceMin + info.choiceCount - 1;
	int newValue = *value + dir;
	if (newValue > maxValue) {
		newValue = wrap ? minValue : maxValue;
	} else if (newValue < minValue) {
		newValue = wrap ? maxValue : minValue;
	}
	if (newValue != *value) {
		*value = newValue;
		Changed(row);
	}
}

void StereoscopyOverlay::Step(Row &row, int dir, bool fast) {
	const ConfigSettingInfo &info = row.ref->setting->Info();
	const float multiplier = fast ? 10.0f : 1.0f;
	if (row.type == RowType::FLOAT) {
		float *value = (float *)row.ptr;
		float delta = row.step * multiplier;
		if (row.proportional) {
			delta = std::max(0.1f, *value * row.step * multiplier);
		}
		// The settings have a precision of 0.1.
		float newValue = std::clamp(roundf((*value + dir * delta) * 10.0f) / 10.0f, info.minValue, info.maxValue);
		if (newValue == *value && row.proportional) {
			// The rounding ate a small step.
			newValue = std::clamp(*value + dir * 0.1f, info.minValue, info.maxValue);
		}
		if (newValue != *value) {
			*value = newValue;
			Changed(row);
		}
	} else if (row.type == RowType::INT) {
		int *value = (int *)row.ptr;
		const int newValue = std::clamp(*value + dir * (int)(row.step * multiplier), (int)info.minValue, (int)info.maxValue);
		if (newValue != *value) {
			*value = newValue;
			Changed(row);
		}
	}
}

void StereoscopyOverlay::Reset(Row &row) {
	if (row.ref->setting->RestoreToDefault(row.ref->block, false)) {
		Changed(row);
	}
}

void StereoscopyOverlay::Changed(const Row &row) {
	changed_ = true;
	if (row.ptr == &g_Config.iStereoscopyMode) {
		// Recreates the framebuffers if stereo turns on or off, and resizes them for the layout.
		System_PostUIMessage(UIMessage::GPU_CONFIG_CHANGED);
	}
	const ConfigSettingInfo &info = row.ref->setting->Info();
	if (info.onChanged) {
		info.onChanged();
	}
}

// Where the value is on the slider track, 0 to 1. Convergence goes on a log scale, as most games want small values.
float StereoscopyOverlay::TrackPos(const Row &row) const {
	const ConfigSettingInfo &info = row.ref->setting->Info();
	const float value = row.type == RowType::FLOAT ? *(const float *)row.ptr : (float)*(const int *)row.ptr;
	if (row.proportional) {
		return log1pf(std::max(0.0f, value - info.minValue)) / log1pf(info.maxValue - info.minValue);
	}
	return (value - info.minValue) / (info.maxValue - info.minValue);
}

void StereoscopyOverlay::SetFromTrack(Row &row, float x) {
	const Bounds track = TrackBounds(row);
	const float pos = std::clamp((x - track.x) / track.w, 0.0f, 1.0f);
	const ConfigSettingInfo &info = row.ref->setting->Info();
	float value;
	if (row.proportional) {
		value = info.minValue + expm1f(pos * log1pf(info.maxValue - info.minValue));
	} else {
		value = info.minValue + pos * (info.maxValue - info.minValue);
	}
	if (row.type == RowType::FLOAT) {
		float *ptr = (float *)row.ptr;
		const float newValue = std::clamp(roundf(value * 10.0f) / 10.0f, info.minValue, info.maxValue);
		if (newValue != *ptr) {
			*ptr = newValue;
			Changed(row);
		}
	} else {
		int *ptr = (int *)row.ptr;
		const int step = std::max(1, (int)row.step);
		const int newValue = std::clamp((int)roundf(value / step) * step, (int)info.minValue, (int)info.maxValue);
		if (newValue != *ptr) {
			*ptr = newValue;
			Changed(row);
		}
	}
}

Bounds StereoscopyOverlay::ValueBounds(const Row &row) const {
	const float inset = ROW_INSET * scale_;
	const Bounds &r = row.bounds;
	return Bounds(r.x + r.w * 0.5f, r.y, r.w * 0.5f - inset, r.h);
}

Bounds StereoscopyOverlay::TrackBounds(const Row &row) const {
	const Bounds value = ValueBounds(row);
	const float numberWidth = 56.0f * scale_;
	const float knobRoom = 8.0f * scale_;
	return Bounds(value.x + knobRoom, value.y, value.w - numberWidth - knobRoom * 2.0f, value.h);
}

void StereoscopyOverlay::Touch(const TouchInput &touch) {
	if (touch.flags & TouchInputFlags::DOWN) {
		for (int i = 0; i < (int)rows_.size(); i++) {
			Row &row = rows_[i];
			if (!row.bounds.Contains(touch.x, touch.y) || !RowEnabled(row)) {
				continue;
			}
			selected_ = i;
			const Bounds value = ValueBounds(row);
			if (!value.Contains(touch.x, touch.y)) {
				break;
			}
			switch (row.type) {
			case RowType::BOOL:
				Cycle(row, true, 1);
				break;
			case RowType::CHOICE:
				Cycle(row, false, touch.x < value.centerX() ? -1 : 1);
				break;
			default:
				dragging_ = i;
				SetFromTrack(row, touch.x);
				break;
			}
			break;
		}
	}
	if ((touch.flags & TouchInputFlags::MOVE) && dragging_ >= 0) {
		SetFromTrack(rows_[dragging_], touch.x);
	}
	if (touch.flags & (TouchInputFlags::UP | TouchInputFlags::CANCEL | TouchInputFlags::RELEASE_ALL)) {
		dragging_ = -1;
	}
}

void StereoscopyOverlay::Layout(const Bounds &bounds) {
	const StereoscopyOverlayPosition position = (StereoscopyOverlayPosition)g_Config.iStereoscopyOverlayPosition;
	wide_ = position == StereoscopyOverlayPosition::TOP || position == StereoscopyOverlayPosition::BOTTOM;

	const int count = (int)rows_.size();
	const int columnRows = wide_ ? STRIP_ROWS : count;
	// The column has the title and the config on lines of their own, and a divider above the overlay's own setting.
	const float headerHeight = wide_ ? TITLE_HEIGHT : TITLE_HEIGHT + SUBTITLE_HEIGHT;
	const float rowsHeight = columnRows * ROW_HEIGHT + (columnRows - 1) * ROW_GAP + (wide_ ? 0.0f : DIVIDER_HEIGHT);
	const float width = wide_ ? STRIP_WIDTH : COLUMN_WIDTH;
	const float height = PADDING + headerHeight + HEADER_GAP + rowsHeight + HINT_GAP + HINT_HEIGHT + PADDING;

	// Shrink everything alike if it doesn't fit.
	const float availW = bounds.w - MARGIN * 2.0f;
	const float availH = bounds.h - MARGIN * 2.0f;
	scale_ = std::max(0.1f, std::min({ 1.0f, availW / width, availH / height }));
	const float s = scale_;
	const float w = width * s;
	const float h = height * s;
	const float margin = MARGIN * s;

	float x, y;
	switch (position) {
	case StereoscopyOverlayPosition::RIGHT:
		x = bounds.x2() - margin - w;
		y = bounds.centerY() - h * 0.5f;
		break;
	case StereoscopyOverlayPosition::TOP:
		x = bounds.centerX() - w * 0.5f;
		y = bounds.y + margin;
		break;
	case StereoscopyOverlayPosition::BOTTOM:
		x = bounds.centerX() - w * 0.5f;
		y = bounds.y2() - margin - h;
		break;
	case StereoscopyOverlayPosition::LEFT:
	default:
		x = bounds.x + margin;
		y = bounds.centerY() - h * 0.5f;
		break;
	}
	// Whole units, so the rows' edges and text are crisp.
	panel_ = Bounds(floorf(x), floorf(y), floorf(w), floorf(h));

	const float rowsTop = panel_.y + (PADDING + headerHeight + HEADER_GAP) * s;
	const float columns = wide_ ? 2.0f : 1.0f;
	const float columnWidth = (panel_.w - PADDING * 2.0f * s - (columns - 1.0f) * COLUMN_GAP * s) / columns;
	for (int i = 0; i < count; i++) {
		const int column = wide_ ? i / STRIP_ROWS : 0;
		const int index = wide_ ? i % STRIP_ROWS : i;
		float rowY = rowsTop + index * (ROW_HEIGHT + ROW_GAP) * s;
		if (!wide_ && i == count - 1) {
			dividerBounds_ = Bounds(panel_.x + PADDING * s, rowY, panel_.w - PADDING * 2.0f * s, DIVIDER_HEIGHT * s);
			rowY += DIVIDER_HEIGHT * s;
		}
		const float rowX = panel_.x + PADDING * s + column * (columnWidth + COLUMN_GAP * s);
		rows_[i].bounds = Bounds(floorf(rowX), floorf(rowY), floorf(columnWidth), floorf(ROW_HEIGHT * s));
	}
}

void StereoscopyOverlay::Draw(UIContext &dc, const Bounds &bounds) {
	Layout(bounds);

	const UI::Theme &theme = dc.GetTheme();
	const float s = scale_;
	auto gr = GetI18NCategory(I18NCat::GRAPHICS);

	dc.SetFontScale(s, s);
	dc.DrawRectDropShadow(panel_, CORNER_RADIUS * s, 0.375f);
	// Keep the game visible behind the panel.
	dc.FillRoundRect(UI::Drawable(colorAlpha(theme.popupStyle.background.color, 0.75f)), panel_, CORNER_RADIUS * s);

	// The title, and whose settings these are.
	std::string config;
	if (g_Config.IsGameSpecific()) {
		config = std::string(gr->T("Game-specific settings")) + " (" + g_paramSFO.GetDiscID() + ")";
	} else {
		config = gr->T("Global settings");
	}
	const float inset = ROW_INSET * s;
	const Bounds title(panel_.x + PADDING * s + inset, panel_.y + PADDING * s, panel_.w - (PADDING * s + inset) * 2.0f, TITLE_HEIGHT * s);
	const uint32_t titleColor = theme.popupTitleStyle.fgColor;
	const uint32_t dimColor = colorAlpha(theme.popupStyle.fgColor, 0.65f);
	if (wide_) {
		float titleW, titleH;
		dc.MeasureText(theme.uiFont, s, s, gr->T("Stereoscopy"), &titleW, &titleH);
		dc.DrawTextRect(theme.uiFont, gr->T("Stereoscopy"), title, titleColor, ALIGN_LEFT | ALIGN_VCENTER);
		const float configX = title.x + titleW + COLUMN_GAP * s;
		dc.DrawTextRectSqueeze(theme.uiFontSmall, config, Bounds(configX, title.y, title.x2() - configX, title.h), dimColor, ALIGN_RIGHT | ALIGN_VCENTER);
	} else {
		dc.DrawTextRectSqueeze(theme.uiFont, gr->T("Stereoscopy"), title, titleColor, ALIGN_LEFT | ALIGN_VCENTER);
		dc.DrawTextRectSqueeze(theme.uiFontSmall, config, Bounds(title.x, title.y2(), title.w, SUBTITLE_HEIGHT * s), dimColor, ALIGN_LEFT | ALIGN_VCENTER);
	}

	if (!wide_) {
		const float lineY = floorf(dividerBounds_.centerY());
		dc.FillRect(UI::Drawable(colorAlpha(theme.popupStyle.fgColor, 0.2f)), Bounds(dividerBounds_.x + inset, lineY, dividerBounds_.w - inset * 2.0f, std::max(1.0f, s)));
	}

	for (int i = 0; i < (int)rows_.size(); i++) {
		DrawRow(dc, rows_[i], i == selected_);
	}

	const float hintsY = panel_.y2() - (PADDING + HINT_HEIGHT) * s;
	DrawHints(dc, Bounds(panel_.x + PADDING * s, hintsY, panel_.w - PADDING * 2.0f * s, HINT_HEIGHT * s));

	dc.SetFontScale(1.0f, 1.0f);
}

void StereoscopyOverlay::DrawRow(UIContext &dc, const Row &row, bool selected) {
	const UI::Theme &theme = dc.GetTheme();
	const float s = scale_;
	const bool enabled = RowEnabled(row);
	const ConfigSettingInfo &info = row.ref->setting->Info();
	auto cat = GetI18NCategory(info.cat);

	uint32_t textColor = theme.popupStyle.fgColor;
	if (selected) {
		dc.FillRoundRect(UI::Drawable(colorAlpha(theme.itemFocusedStyle.background.color, 0.75f)), row.bounds, 6.0f * s);
		textColor = theme.itemFocusedStyle.fgColor;
	}
	if (!enabled) {
		textColor = colorAlpha(textColor, 0.4f);
	}

	const float inset = ROW_INSET * s;
	const Bounds label(row.bounds.x + inset, row.bounds.y, row.bounds.w * 0.5f - inset * 2.0f, row.bounds.h);
	dc.DrawTextRectSqueeze(theme.uiFontSmall, cat->T(info.label, info.labelDefault ? info.labelDefault : ""), label, textColor, ALIGN_LEFT | ALIGN_VCENTER);

	const Bounds value = ValueBounds(row);
	switch (row.type) {
	case RowType::CHOICE:
	{
		const int index = *(const int *)row.ptr - info.choiceMin;
		const bool atStart = index <= 0;
		const bool atEnd = index >= info.choiceCount - 1;
		const float chevron = 18.0f * s;
		float imageW = 1.0f, imageH = 1.0f;
		dc.Draw()->MeasureImage(ImageID("I_CHEVRON_LEFT"), &imageW, &imageH);
		const float imageScale = chevron / std::max(1.0f, imageH);
		// The chevrons dim at the ends, as left and right stop there.
		const uint32_t chevronColor = colorAlpha(textColor, enabled && selected ? 1.0f : 0.5f);
		dc.Draw()->DrawImage(ImageID("I_CHEVRON_LEFT"), value.x + chevron * 0.5f, value.centerY(), imageScale, atStart ? colorAlpha(chevronColor, 0.3f) : chevronColor, ALIGN_CENTER);
		dc.Draw()->DrawImage(ImageID("I_CHEVRON_RIGHT"), value.x2() - chevron * 0.5f, value.centerY(), imageScale, atEnd ? colorAlpha(chevronColor, 0.3f) : chevronColor, ALIGN_CENTER);
		const std::string_view text = index >= 0 && index < info.choiceCount ? cat->T(info.choices[index]) : std::string_view();
		const Bounds textBounds(value.x + chevron + 4.0f * s, value.y, value.w - (chevron + 4.0f * s) * 2.0f, value.h);
		dc.DrawTextRectSqueeze(theme.uiFontSmall, text, textBounds, textColor, ALIGN_CENTER);
		break;
	}
	case RowType::BOOL:
	{
		const ImageID image = *(const bool *)row.ptr ? theme.checkOn : theme.checkOff;
		float imageW = 1.0f, imageH = 1.0f;
		dc.Draw()->MeasureImage(image, &imageW, &imageH);
		const float size = 24.0f * s;
		dc.Draw()->DrawImage(image, value.x2() - size * 0.5f, value.centerY(), size / std::max(1.0f, imageH), textColor, ALIGN_CENTER);
		break;
	}
	case RowType::FLOAT:
	case RowType::INT:
	{
		const Bounds track = TrackBounds(row);
		const float pos = std::clamp(TrackPos(row), 0.0f, 1.0f);
		const float knobX = track.x + pos * track.w;
		// Ranges around zero fill from the middle.
		float fillFrom = track.x;
		if (info.minValue < 0.0f && info.maxValue > 0.0f && !row.proportional) {
			fillFrom = track.x + (-info.minValue / (info.maxValue - info.minValue)) * track.w;
		}
		// On the selected row's background, the slider color wouldn't show.
		uint32_t fillColor = selected ? textColor : theme.popupSliderColor;
		if (!enabled) {
			fillColor = colorAlpha(fillColor, 0.4f);
		}
		const float thickness = std::max(2.0f, floorf(4.0f * s));
		const float trackY = floorf(track.centerY() - thickness * 0.5f);
		dc.FillRect(UI::Drawable(colorAlpha(selected ? textColor : 0xFF808080, enabled ? 0.4f : 0.2f)), Bounds(track.x, trackY, track.w, thickness));
		dc.FillRect(UI::Drawable(fillColor), Bounds(std::min(fillFrom, knobX), trackY, fabsf(knobX - fillFrom), thickness));
		float knobW = 1.0f, knobH = 1.0f;
		dc.Draw()->MeasureImage(theme.sliderKnob, &knobW, &knobH);
		dc.Draw()->DrawImage(theme.sliderKnob, knobX, track.centerY(), 16.0f * s / std::max(1.0f, knobH), fillColor, ALIGN_CENTER);

		char text[32];
		if (row.type == RowType::FLOAT) {
			snprintf(text, sizeof(text), "%.1f", *(const float *)row.ptr);
		} else {
			snprintf(text, sizeof(text), "%d", *(const int *)row.ptr);
		}
		dc.DrawTextRect(theme.uiFontSmall, text, Bounds(track.x2(), value.y, value.x2() - track.x2(), value.h), textColor, ALIGN_RIGHT | ALIGN_VCENTER);
		break;
	}
	}
}

// Which buttons do what. Without stereoscopy available, why instead.
void StereoscopyOverlay::DrawHints(UIContext &dc, const Bounds &bounds) {
	const UI::Theme &theme = dc.GetTheme();
	const float s = scale_;
	auto gr = GetI18NCategory(I18NCat::GRAPHICS);
	const uint32_t color = colorAlpha(theme.popupStyle.fgColor, 0.7f);

	if (!StereoscopyAvailable(draw_)) {
		std::string_view reason;
		if (g_Config.bSoftwareRendering) {
			reason = gr->T("StereoscopySoftware Tip", "Not available with software rendering");
		} else if (StereoscopySupported(draw_)) {
			reason = gr->T("StereoscopySkipBuffer Tip", "Not available with Skip buffer effects");
		} else {
			reason = gr->T("StereoscopyUnsupported Tip", "Needs the Vulkan, Metal or Direct3D 11 backend");
		}
		dc.DrawTextRectSqueeze(theme.uiFontSmall, reason, bounds, color, ALIGN_CENTER);
		return;
	}

	const bool confirmWithCross = g_Config.iButtonPreference == PSP_SYSTEMPARAM_BUTTON_CROSS;
	struct Hint {
		ImageID images[2];
		std::string_view text;
	};
	const Hint hints[] = {
		{ { ImageID("I_ARROW_LEFT"), ImageID("I_ARROW_RIGHT") }, gr->T("Change") },
		{ { ImageID("I_TRIANGLE"), ImageID::invalid() }, gr->T("Default") },
		{ { ImageID(confirmWithCross ? "I_CIRCLE" : "I_CROSS"), ImageID::invalid() }, gr->T("Close") },
	};

	const float iconSize = 18.0f * s;
	const float iconGap = 4.0f * s;
	const float textGap = 6.0f * s;
	const float hintGap = 22.0f * s;

	// Measure first, to center the whole line.
	float textWidths[ARRAY_SIZE(hints)];
	float total = 0.0f;
	for (size_t i = 0; i < ARRAY_SIZE(hints); i++) {
		float textH;
		dc.MeasureText(theme.uiFontTiny, s, s, hints[i].text, &textWidths[i], &textH);
		const int icons = hints[i].images[1].isValid() ? 2 : 1;
		total += icons * iconSize + (icons - 1) * iconGap + textGap + textWidths[i] + (i > 0 ? hintGap : 0.0f);
	}
	// Squeezed if it's too long.
	const float squeeze = std::min(1.0f, bounds.w / total);
	float x = bounds.centerX() - total * squeeze * 0.5f;
	for (size_t i = 0; i < ARRAY_SIZE(hints); i++) {
		if (i > 0) {
			x += hintGap * squeeze;
		}
		for (int j = 0; j < 2; j++) {
			const ImageID image = hints[i].images[j];
			if (!image.isValid()) {
				continue;
			}
			if (j > 0) {
				x += iconGap * squeeze;
			}
			float imageW = 1.0f, imageH = 1.0f;
			dc.Draw()->MeasureImage(image, &imageW, &imageH);
			const float imageScale = iconSize * squeeze / std::max(1.0f, std::max(imageW, imageH));
			dc.Draw()->DrawImage(image, x + iconSize * squeeze * 0.5f, bounds.centerY(), imageScale, color, ALIGN_CENTER);
			x += iconSize * squeeze;
		}
		x += textGap * squeeze;
		dc.SetFontScale(s * squeeze, s);
		dc.DrawText(theme.uiFontTiny, hints[i].text, x, bounds.centerY(), color, ALIGN_LEFT | ALIGN_VCENTER);
		dc.SetFontScale(s, s);
		x += textWidths[i] * squeeze;
	}
}
