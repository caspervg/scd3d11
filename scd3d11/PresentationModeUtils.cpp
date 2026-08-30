/*
 *  SCD3D11 - a free graphics driver for SimCity 4's SimGL interface
 *  Copyright (C) 2025  Nelson Gomez (nsgomez) <nelson@ngomez.me>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#include "PresentationModeUtils.h"

#include <cstdlib>

namespace nSCD3D11
{
	std::string LowercaseCopy(std::string text) {
		for (char &character: text) {
			if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
		}
		return text;
	}

	bool BorderlessFullscreenRequested(std::string const &lowercaseCommandLine) {
		return lowercaseCommandLine.find("-borderless") != std::string::npos ||
		       lowercaseCommandLine.find("-fullscreenmode:borderless") != std::string::npos;
	}

	uint32_t RequestedMonitorIndex(std::string const &lowercaseCommandLine) {
		char const *const switchName = "-monitor:";
		size_t const position = lowercaseCommandLine.find(switchName);
		if (position == std::string::npos) return 0;

		char const *const digits = lowercaseCommandLine.c_str() + position + std::char_traits<char>::length(switchName);
		// Require a digit immediately after the colon. strtoul would otherwise skip leading
		// whitespace and accept a sign, reading "-Monitor: 2" and "-Monitor:-2" as indices the
		// switch was never meant to express.
		if (*digits < '0' || *digits > '9') return 0;
		return static_cast<uint32_t>(std::strtoul(digits, nullptr, 10));
	}

	PresentationMode SelectPresentationMode(
		bool modeIsFullscreen, bool fallbackToWindowed, std::string const &lowercaseCommandLine) {
		if (!modeIsFullscreen || fallbackToWindowed) return PresentationMode::Windowed;
		return BorderlessFullscreenRequested(lowercaseCommandLine)
			       ? PresentationMode::BorderlessFullscreen
			       : PresentationMode::ExclusiveFullscreen;
	}

	ClientRectangle CentredClientRectangle(ClientRectangle const &monitor, uint32_t width, uint32_t height) {
		int32_t const left = monitor.left + ((monitor.right - monitor.left) - static_cast<int32_t>(width)) / 2;
		int32_t const top = monitor.top + ((monitor.bottom - monitor.top) - static_cast<int32_t>(height)) / 2;
		return ClientRectangle{left, top, left + static_cast<int32_t>(width), top + static_cast<int32_t>(height)};
	}
}
