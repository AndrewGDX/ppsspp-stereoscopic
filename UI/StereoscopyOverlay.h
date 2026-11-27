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

#include <map>
#include <vector>

#include "Common/Input/InputState.h"
#include "Common/Math/geom2d.h"

class UIContext;
struct ConfigSettingRef;

namespace Draw {
class DrawContext;
}

// When the stereoscopy settings can be changed, shared with the settings page.
bool StereoscopyAvailable(Draw::DrawContext *draw);
bool StereoscopyOn(Draw::DrawContext *draw);
bool StereoscopyUIDetectionOn(Draw::DrawContext *draw);
// Only unsafe and advanced detection look at the depth.
bool StereoscopyUIThresholdOn(Draw::DrawContext *draw);

// The stereoscopy settings, changed in game while it keeps running. Drawn by EmuScreen, once for each eye,
// and steered with the keys mapped to the PSP's buttons. Changes apply right away, and are saved on close.
class StereoscopyOverlay {
public:
	explicit StereoscopyOverlay(Draw::DrawContext *draw);
	~StereoscopyOverlay();
	// Android replaces the draw context when it loses the surface, which can happen while the overlay is open.
	void SetDrawContext(Draw::DrawContext *draw) { draw_ = draw; }

	// Keys and axes, all of which the overlay takes while it's open.
	void Key(const KeyInput &key);
	void Axis(const AxisInput &axis);
	// In the coordinates Draw lays it out in.
	void Touch(const TouchInput &touch);
	void Update();
	void Draw(UIContext &dc, const Bounds &bounds);

	bool WantsClose() const { return wantsClose_; }
	bool Changed() const { return changed_; }

private:
	enum class RowType {
		CHOICE,
		BOOL,
		FLOAT,
		INT,
	};
	enum class Action {
		NONE,
		UP,
		DOWN,
		LEFT,
		RIGHT,
		CONFIRM,
		RESET,
		CLOSE,
	};
	struct Row {
		void *ptr;
		const ConfigSettingRef *ref;
		RowType type;
		float step;  // FLOAT and INT.
		// Steps by step times the value, and its slider has a log scale.
		bool proportional;
		bool (*enabled)(Draw::DrawContext *draw);  // Always enabled if null.
		Bounds bounds;  // From the last Layout.
	};

	void Layout(const Bounds &bounds);
	void DrawRow(UIContext &dc, const Row &row, bool selected);
	void DrawHints(UIContext &dc, const Bounds &bounds);

	Action ButtonToAction(int pspButton) const;
	Action MappingToAction(const InputMapping &mapping) const;
	Action KeyToAction(const KeyInput &key) const;
	void Press(Action action, uint64_t source);
	void Release(uint64_t source);
	void Perform(Action action, bool repeat);
	void MoveSelection(int dir);
	void Step(Row &row, int dir, bool fast);
	void Cycle(Row &row, bool wrap, int dir);
	void Reset(Row &row);
	void SetFromTrack(Row &row, float x);
	void Changed(const Row &row);

	bool RowEnabled(const Row &row) const;
	float TrackPos(const Row &row) const;
	Bounds ValueBounds(const Row &row) const;
	Bounds TrackBounds(const Row &row) const;

	Draw::DrawContext *draw_;
	std::vector<Row> rows_;
	int selected_ = 0;
	bool changed_ = false;
	bool wantsClose_ = false;

	// Key repeat of the held direction. Accelerates after a while.
	Action heldAction_ = Action::NONE;
	uint64_t heldSource_ = 0;
	double heldSince_ = 0.0;
	double nextRepeat_ = 0.0;
	// Which axis directions are pushed past the threshold, by source.
	std::map<uint64_t, bool> axisPushed_;

	// The slider row being dragged with the mouse or a finger, or -1.
	int dragging_ = -1;

	// From the last Layout, in the coordinates Draw was given.
	Bounds panel_;
	Bounds dividerBounds_;
	// Everything shrinks alike when the panel doesn't fit.
	float scale_ = 1.0f;
	// The strip at the top or bottom, with two columns of settings.
	bool wide_ = false;
};
