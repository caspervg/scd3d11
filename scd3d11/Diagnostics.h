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
#include <filesystem>
#include <windows.h>

namespace nSCD3D11
{
	enum class LogCategory
	{
		Initialization,
		Capabilities,
		SwapChain,
		Resource,
		Grid,
		Unsupported,
		Window,
		// DrawStats frame reports.
		Stats,
		// Doctor findings about the rest of the installation.
		Doctor,
		Count
	};

	void Log(LogCategory category, char const* format, ...);
	// The Logs folder of the user directory, next to the Plugins folder holding SCD3D11.dll. Empty when the
	// module path is unavailable, which puts the log in the working directory.
	std::filesystem::path LogDirectory();
	// Only written with LogLevel=trace.
	void LogTrace(LogCategory category, char const* format, ...);
	void LogHRESULT(LogCategory category, char const* operation, HRESULT result);
	// Free address space, RAM and page file, to tell which one ran out when allocations fail.
	void LogMemoryStatus(LogCategory category, char const* when);
	// The DrawStats and LogLevel=trace settings, handed over by Settings::Load.
	void SetDiagnostics(bool drawStats, bool traceLog);
	bool DrawStatsEnabled();

	enum class ObservedCategory
	{
		RenderState,
		VertexFormat,
		TextureFormat,
		Count
	};

	void RecordEncountered(ObservedCategory category, uint64_t value);

	// Render-loop watchdog: the render thread reports what it is doing, and a background thread logs where
	// that thread is when no frame completes for a few seconds. Phases must be string literals.
	void NoteRenderPhase(char const* phase);
	void NoteRenderFrame();
	void StartRenderWatchdog(HWND window);
	void StopRenderWatchdog();
}
