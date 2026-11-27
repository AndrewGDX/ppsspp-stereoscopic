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

#include <vector>
#include "Common/Data/Format/IniFile.h"

namespace DisplayXRPolicy {
inline std::vector<int> ModeChoices(bool available, int selected) {
	return available || selected == 5 ? std::vector<int>{5, 0, 1, 2, 3, 4, 6, 7} : std::vector<int>{0, 1, 2, 3, 4, 6, 7};
}

// Only the global default is migrated. An explicit game choice and later user edits win.
inline bool ApplyDefault(IniFile &global, const IniFile *game, int &mode, bool available) {
	Section *graphics = global.GetOrCreateSection("Graphics");
	bool applied = false;
	graphics->Get("DisplayXRDefaultApplied", &applied);
	if (!available || applied) {
		return false;
	}
	graphics->Set("DisplayXRDefaultApplied", true);
	graphics->Set("StereoscopyMode", 5);
	int overrideMode;
	const Section *overrideSection = game ? game->GetSection("Graphics") : nullptr;
	if (!overrideSection || !overrideSection->Get("StereoscopyMode", &overrideMode)) {
		mode = 5;
	}
	return true;
}
}
