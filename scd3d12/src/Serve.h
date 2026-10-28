/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include <string>

#include <windows.h>

namespace nSCD3D12 {
	// Draws the game whose SCD3D11 created the shared ring ringName, until the game exits. With logStats, logs
	// the frame rate and the CPU and GPU time per frame every second.
	int RunServe(std::wstring const &ringName, DWORD parentId, bool logStats);
}
