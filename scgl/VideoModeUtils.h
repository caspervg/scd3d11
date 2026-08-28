/*
 *  SCGL - a free graphics driver for SimCity 4's SimGL interface
 *  Copyright (C) 2025  Nelson Gomez (nsgomez) <nelson@ngomez.me>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include "sGDMode.h"

#include <vector>

namespace nSCGL
{
	bool AppendVideoMode(
		std::vector<sGDMode>& modes, uint32_t width, uint32_t height, uint32_t depth, bool fullscreen,
		bool supportsStencil, bool supportsDxt);
}
