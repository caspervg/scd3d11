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

#include "SC4VersionDetection.h"

#include <cstdint>

#include <windows.h>

namespace nSCD3D11 {
	// True in SimCity 4 1.1.641, the only version whose code SCD3D11 patches.
	// SC4VersionDetection reads the game's code at 0x6E5000 when the executable
	// has no version resource, so a host that cannot be SimCity 4 - the tests -
	// is turned away before it is asked.
	inline bool IsGameVersion641() {
		auto const *const module = reinterpret_cast<uint8_t const *>(GetModuleHandleW(nullptr));
		auto const *const dos = reinterpret_cast<IMAGE_DOS_HEADER const *>(module);
		auto const *const nt = reinterpret_cast<IMAGE_NT_HEADERS const *>(module + dos->e_lfanew);
		if (reinterpret_cast<uintptr_t>(module) != 0x00400000 || nt->OptionalHeader.SizeOfImage <= 0x002E5000)
			return false;
		return SC4VersionDetection::GetGameVersion() == 641;
	}
} // namespace nSCD3D11
