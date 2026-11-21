# Copyright (c) 2012- PPSSPP Project.

# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, version 2.0 or later versions.

# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License 2.0 for more details.

# A copy of the GPL 2.0 should have been included with the program.
# If not, see http://www.gnu.org/licenses/

cmake_minimum_required(VERSION 3.16)

# MoltenVK is loaded with dlopen, so dependency scanning cannot discover it.
file(COPY "${MOLTENVK_LIBRARY}" DESTINATION "${BUNDLE_DIR}/Contents/Frameworks")

include(BundleUtilities)
set(BU_CHMOD_BUNDLE_ITEMS ON)
fixup_bundle("${BUNDLE_DIR}" "${BUNDLE_DIR}/Contents/Frameworks/libMoltenVK.dylib" "")

# Rewriting install names invalidates library signatures. Sign dependencies first.
file(GLOB_RECURSE bundled_libraries "${BUNDLE_DIR}/Contents/Frameworks/*.dylib")
foreach(item IN LISTS bundled_libraries)
	execute_process(COMMAND /usr/bin/codesign --force --sign - "${item}"
		RESULT_VARIABLE sign_result)
	if(NOT sign_result EQUAL 0)
		message(FATAL_ERROR "Could not sign ${item}")
	endif()
endforeach()
execute_process(COMMAND /usr/bin/codesign --force --sign -
	--entitlements "${CMAKE_CURRENT_LIST_DIR}/Entitlements.plist" "${BUNDLE_DIR}"
	RESULT_VARIABLE sign_result)
if(NOT sign_result EQUAL 0)
	message(FATAL_ERROR "Could not sign ${BUNDLE_DIR}")
endif()
