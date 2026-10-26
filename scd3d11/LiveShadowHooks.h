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

// Game-side half of live shadows, opt-in with LiveShadows. Prebuilt network
// pieces and True3D props are identified here and captured by cGDriver as
// indexed casters. SC4's lighting state is read here for the composite.

namespace nSCD3D11::LiveShadowHooks {
	bool Install();
	void Uninstall();

	bool Enabled();
	// True while a prebuilt network piece or its model is being drawn.
	bool NetworkDrawActive();
	// False when no registered prop mesh has this many vertices: the draw
	// cannot match, and need not be hashed.
	bool MayMatchPropMesh(uint32_t vertexCount);
	bool MatchPropSignature(uint64_t signature);
	// Records a relaxed prop's mesh (AddShadow's position and UV streams) for
	// cGDriver to recognise when the prop is drawn. ShadowRegistry's fallback
	// for a prop it cannot keep.
	void RegisterPropMesh(uint32_t count, void const *positions, void const *uvs);

	// SC4's lighting, read from the 1.1.641 renderer: the world direction
	// shadows travel (GetShadowDirection), the shadow colour and strength
	// (GetShadowParams) and the day/night weight of that tint.
	bool SunDirection(float direction[3]);
	bool ShadowParams(float colour[3], float &strength);
	bool ShadowDaylight(float &daylight);
} // namespace nSCD3D11::LiveShadowHooks
