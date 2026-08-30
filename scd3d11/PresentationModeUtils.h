/*
 *  SCD3D11 - a free graphics driver for SimCity 4's SimGL interface
 *  Copyright (C) 2025  Nelson Gomez (nsgomez) <nelson@ngomez.me>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include <cstdint>
#include <string>

namespace nSCD3D11
{
	enum class PresentationMode : uint8_t {
		Windowed,
		ExclusiveFullscreen,
		BorderlessFullscreen,
	};

	// A client area, in screen coordinates. Deliberately not Win32's RECT so the mode selection
	// logic stays testable without a window.
	struct ClientRectangle {
		int32_t left;
		int32_t top;
		int32_t right;
		int32_t bottom;
	};

	// The command line comparisons below are all lowercase, so callers fold the process command
	// line once and reuse it.
	std::string LowercaseCopy(std::string text);

	bool BorderlessFullscreenRequested(std::string const &lowercaseCommandLine);

	// `-Monitor:<n>` picks a 1-based display for the fullscreen modes. Returns 0 when the switch
	// is absent or does not parse, meaning "the primary monitor".
	uint32_t RequestedMonitorIndex(std::string const &lowercaseCommandLine);

	// The display device `-Monitor:<n>` names, as `\\.\DISPLAY<n>`. That is the numbering Windows'
	// own Display Settings shows, unlike the arbitrary order EnumDisplayMonitors calls back in.
	// Returns an empty string for index 0, meaning "the primary display".
	std::string MonitorDeviceName(uint32_t index);

	// fallbackToWindowed forces a window regardless of what the mode and command line ask for;
	// device recovery sets it once it has given up on a fullscreen mode.
	PresentationMode SelectPresentationMode(
		bool modeIsFullscreen, bool fallbackToWindowed, std::string const &lowercaseCommandLine);

	// A client area of exactly the requested size, centred on the monitor. Both fullscreen modes
	// keep the client area the size the game renders at and believes it is presenting to, so
	// window-space mouse coordinates line up with the game's own UI layout; a mode smaller than
	// the monitor is centred rather than stretched to fit.
	ClientRectangle CentredClientRectangle(ClientRectangle const &monitor, uint32_t width, uint32_t height);
}
