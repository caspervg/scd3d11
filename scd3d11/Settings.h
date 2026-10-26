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
#include <optional>
#include <string>
#include <string_view>

class cIGZCmdLine;

// SCD3D11's settings. Each is looked up on SC4's command line first (-Name or
// -Name:value) and then in SCD3D11.ini next to the DLL (Name=value under
// [SCD3D11]), so the INI holds a player's usual settings and a switch
// overrides them for one session.
namespace nSCD3D11::Settings {
	// Every setting, read once by Load. Until then, and in the tests, the
	// defaults below apply.
	struct Values {
		bool liveShadows = false;
		bool networkShadowMasks = false;
		bool vsync = true;
		bool borderless = false;
		bool flipModel = true;
		// GPU=default: the adapter Windows picks instead of the high-performance one.
		bool defaultAdapter = false;
		// SunLatitude: live shadows follow a sun at this latitude in degrees
		// through SC4's day and year. Empty keeps SC4's fixed sun.
		std::optional<float> sunLatitude;
		bool viewportCull = true;
		// Anisotropy: the most texture samples per pixel along a slanted
		// surface, 1 when off.
		uint32_t anisotropy = 1;
		bool reshade = true;
		bool drawStats = false;
		// LogLevel=trace.
		bool traceLog = false;
	};

	// Reads every setting from SC4's command line (may be null) and the INI.
	// Called once from the COM director's OnStart, before SC4 starts the
	// threads that read them, so they need no locking.
	void Load(cIGZCmdLine *commandLine);

	Values const &Get();

	// Exposed for tests: reads value as a flag the way sc4-dll-utilities does
	// (on/off, true/false, enabled/disabled, 1/0, any case), with an empty
	// value - a bare -Name - meaning on. False when it is none of those.
	bool ParseFlag(std::string_view value, bool &flag);

	// Exposed for tests: Anisotropy, off or 2, 4, 8 or 16.
	bool ParseAnisotropy(std::string_view value, uint32_t &anisotropy);

	// Exposed for tests: SunLatitude, a number from -90 to 90 or off.
	bool ParseLatitude(std::string_view value, std::optional<float> &latitude);
} // namespace nSCD3D11::Settings
