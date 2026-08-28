/*
 *  SCD3D11 - a free graphics driver for SimCity 4's SimGL interface
 *  Copyright (C) 2025  Nelson Gomez (nsgomez) <nelson@ngomez.me>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#include "VideoModeUtils.h"

namespace nSCD3D11
{
	bool AppendVideoMode(
		std::vector<sGDMode>& modes, uint32_t width, uint32_t height, uint32_t depth, bool fullscreen,
		bool supportsStencil, bool supportsDxt) {
		for (sGDMode const& mode : modes) {
			if (mode.width == width && mode.height == height && mode.depth == depth && mode.isFullscreen == fullscreen) return false;
		}

		sGDMode mode{};
		mode.index = static_cast<uint32_t>(modes.size());
		mode.width = width;
		mode.height = height;
		mode.depth = depth;
		mode.isFullscreen = fullscreen;
		mode.supportsStencilBuffer = supportsStencil;
		mode.__unknown2 = true;
		mode.textureStageCount = 2;
		mode.supportsMultitexture = true;
		mode.supportsTextureEnvCombine = true;
		mode.supportsFogCoord = false;
		mode.supportsDxtTextures = supportsDxt;
		mode.isInitialized = true;
		// Every D3D11 swap chain uses DXGI_FORMAT_R8G8B8A8_UNORM.
		mode.alphaColorMask = 0xff000000;
		mode.redColorMask = 0x00ff0000;
		mode.greenColorMask = 0x0000ff00;
		mode.blueColorMask = 0x000000ff;
		modes.push_back(mode);
		return true;
	}
}
