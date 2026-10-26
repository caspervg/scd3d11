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
	// On for on, true, yes, 1 or a bare -Name; off for off, false, no or 0;
	// fallback when the setting is absent or holds anything else.
	bool Flag(char const *name, bool fallback);

	// The setting's value in lower case; empty when it is absent or bare.
	std::string Value(char const *name);

	// Exposed for tests: finds -name in a command line, the last one winning,
	// and sets value to what follows its colon (empty for a bare switch).
	bool FindSwitch(std::string_view commandLine, std::string_view name, std::string &value);

	// Exposed for tests: a value as a flag, or fallback when it is neither.
	bool ParseFlag(std::string_view value, bool fallback);
} // namespace nSCD3D11::Settings
