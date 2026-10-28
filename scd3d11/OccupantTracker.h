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

// Tells the renderer on the other end of the stream which occupants exist: SC4's occupant manager posts a
// message when one is inserted, removed or moved, and each becomes an Occupant record (wire/Wire.h).
// Only does anything while a driver writes a stream (Record or Renderer=server).
namespace nSCD3D11::OccupantTracker {
	// Subscribes to the occupant messages. Needs SC4's message server, so call it from PostAppInit or later.
	void Install();
	void Uninstall();
}
