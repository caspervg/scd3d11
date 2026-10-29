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

// Marks which game object each of SC4's draws belongs to, in the stream SCD3D11 writes (Record or
// Renderer=server). See DrawOwnerHooks.cpp for the patch sites.
namespace nSCD3D11::DrawOwnerHooks {
	bool Install();
	void Uninstall();
	// The game is about to read its own frame (a snapshot, the region thumbnail): draw every model for a while.
	void DrawAllModels();
}
