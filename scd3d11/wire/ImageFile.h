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

#include <cstdint>
#include <vector>

namespace nSCDWire {
	// Writes tightly packed RGBA rows, the back buffer's layout, as a PNG with alpha forced opaque.
	// COM must be initialised.
	bool SavePng(wchar_t const *path, uint32_t width, uint32_t height, uint8_t const *rgba);
	// Reads a PNG back as tightly packed RGBA rows.
	bool LoadPng(wchar_t const *path, uint32_t &width, uint32_t &height, std::vector<uint8_t> &rgba);
}
