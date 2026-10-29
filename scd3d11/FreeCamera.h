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

// FreeCamera=on (SCD3D12 M7): the city view's camera turns, tilts and zooms freely. Drag with the middle mouse
// button to turn and tilt, scroll for smooth zoom, hold ALT for the classic angle. It moves SC4's own camera
// (camera/Camera.cpp, from sc4-modern-camera), so SC4 draws, culls and picks with it, and the server's owned scene
// draws what SC4 sends.
namespace nSCD3D11::FreeCamera {
	// Needs SC4's message server and framework, so call it from PostAppInit or later. Does nothing unless the
	// setting is on.
	void Install();
	void Uninstall();
}
