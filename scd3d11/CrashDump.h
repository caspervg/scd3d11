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

#include <string_view>

// CrashDump=on: a full-memory dump when SC4 crashes while SCD3D11 renders. SC4's own exception report only
// holds thread stacks, and display driver crashes happen on the driver's worker thread, after whatever fed it
// bad data has returned. A dump with the heap shows the driver's command data and the objects it pointed at.
namespace nSCD3D11::CrashDump {
	// Called once SCD3D11's device exists, so only when SC4 picked SCD3D11. Does nothing unless the setting
	// is on. The first crash nothing handles, or the first access violation or illegal instruction inside a
	// display driver (a DLL under Windows' DriverStore), writes one dump to %LOCALAPPDATA%\SCD3D11\CrashDumps.
	// SC4's own exception report then runs as before.
	void Install();
	void Uninstall();

	// Exposed for tests: Install without looking at the setting.
	void Arm();

	// Exposed for tests: whether a mapped image path is a display driver's.
	bool IsDriverStorePath(std::wstring_view path);
} // namespace nSCD3D11::CrashDump
