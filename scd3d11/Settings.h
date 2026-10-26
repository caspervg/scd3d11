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
#include <string_view>

// SCD3D11's switches. Each is looked up on the command line first (-Name or
// -Name:value) and then in SCD3D11.ini next to the DLL (Name=value under
// [SCD3D11]), so the INI holds a player's usual settings and a switch
// overrides them for one session. Names and values are case-insensitive.
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
		bool viewportCull = true;
		bool reshade = true;
		bool drawStats = false;
		// LogLevel=trace.
		bool traceLog = false;
	};

	// Reads every setting. Called once from the COM director's OnStart, before
	// SC4 starts the threads that read them, so they need no locking.
	void Load();

	Values const &Get();

	// Exposed for tests: finds -name in a command line, the last one winning,
	// and sets value to what follows its colon (empty for a bare switch).
	bool FindSwitch(std::string_view commandLine, std::string_view name, std::string &value);

	// Exposed for tests: a value as a flag, or fallback when it is neither.
	bool ParseFlag(std::string_view value, bool fallback);
} // namespace nSCD3D11::Settings
