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
		Count
	};

	void Log(LogCategory category, char const* format, ...);
	void LogHRESULT(LogCategory category, char const* operation, HRESULT result);

	enum class ObservedCategory
	{
		RenderState,
		VertexFormat,
		TextureFormat,
		Count
	};

	void RecordEncountered(ObservedCategory category, uint64_t value);
}
