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
#include <string>
#include <string_view>
#include <vector>

class cIGZCmdLine;

// SCD3D11's settings. Each is looked up on SC4's command line first (-Name or
// -Name:value) and then in SCD3D11.ini next to the DLL (Name=value under
// [SCD3D11]), so the INI holds a player's usual settings and a switch
// overrides them for one session.
namespace nSCD3D11::Settings {
	// Every setting, read once by Load. Until then, and in the tests, the
	// defaults below apply.
	// GPU=high-performance, the default. GPU=default: the GPU Windows picks. GPU=<n>: the nth GPU in the
	// doctor's list, from 1.
	struct GpuChoice {
		bool systemDefault = false;
		uint32_t number = 0;
	};

	struct Values {
		bool liveShadows = false;
		bool networkShadowMasks = false;
		bool vsync = true;
		// A monitor-sized window. It needs no -f, SCD3D11 turns full screen on, and wins over -w.
		bool borderless = false;
		bool flipModel = true;
		GpuChoice gpu;
		bool viewportCull = true;
		// ViewportClamp=on: take SetViewport rectangles that reach past the window or have no area the way
		// scvk does (clamped scissor, full window for an empty one) instead of refusing them.
		bool viewportClamp = false;
		bool reshade = true;
		bool drawStats = false;
		// LogLevel=trace.
		bool traceLog = false;
		// Doctor=on: also log every DLL and plugin setting the doctor looked at.
		bool doctor = false;
		// DoctorDialog=off: keep the doctor's problems out of SC4's own dialog, in the log only.
		bool doctorDialog = true;
		// UpdateCheck=off: do not ask GitHub, at most once a day, whether a newer SCD3D11 is out.
		bool updateCheck = true;
		// VertexCacheSegments=1-16: how many segments the vertex cache keeps.
		uint32_t vertexCacheSegments = 8;
		// VertexCacheSize=4-64: each vertex cache segment's size in MB.
		uint32_t vertexCacheSizeMB = 16;
	};

	// Reads every setting from SC4's command line (may be null) and the INI.
	// Called once from the COM director's OnStart, before SC4 starts the
	// threads that read them, so they need no locking.
	void Load(cIGZCmdLine *commandLine);

	Values const &Get();

	// A value Load could not read and ignored, for the doctor. The source is "command line" or "SCD3D11.ini".
	struct Rejection {
		std::string name;
		std::string value;
		std::string source;
	};

	std::vector<Rejection> const &Rejected();

	// Exposed for tests: reads value as a flag the way sc4-dll-utilities does
	// (on/off, true/false, enabled/disabled, 1/0, any case), with an empty
	// value - a bare -Name - meaning on. False when it is none of those.
	bool ParseFlag(std::string_view value, bool &flag);

	// Exposed for tests: reads value as a whole number from minimum to
	// maximum, digits only. False when it is anything else.
	bool ParseNumber(std::string_view value, uint32_t minimum, uint32_t maximum, uint32_t &number);
} // namespace nSCD3D11::Settings
