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

namespace nSCD3D11 {
	class cGDriverWire;
}

// Sends the whole city's terrain and water to the server (a TerrainField record), so it can draw the parts of the
// city SC4 never drew. Read from SC4's own terrain view arrays once a city loads and whenever they change.
namespace nSCD3D11::TerrainStream {
	// At the end of each frame, on the render thread. Reads the terrain at most once a second and records it when it
	// differs from what was sent.
	void Poll(cGDriverWire &wire);
}
