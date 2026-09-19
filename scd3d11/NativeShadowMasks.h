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

// Native shadow support, opt-in with -NativeShadowMasks:network, :props or
// :all. Prebuilt network pieces and True3D props are captured as indexed live
// casters and rendered by cGDriver. SC4's existing network mask decals remain
// available as a fallback for geometry outside the prebuilt-model draw path.
//
// Independent of NativeShadowExperiment, which stays a research toggle.

namespace nSCD3D11::NativeShadowMasks {
	bool Install();
	void Uninstall();

	// Native shadow pixels extend beyond SC4's ordinary object dirty rectangles,
	// so translated backing-store updates must rebuild the static view.
	bool RequiresCleanTranslatedRedraw();
	bool LiveNetworkEnabled();
	bool LiveNetworkDrawActive();
	bool LivePropsEnabled();
	bool HasLivePropMeshes();
	bool MatchLivePropSignature(uint64_t signature);

} // namespace nSCD3D11::NativeShadowMasks
