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

namespace nSCD3D11::SimTickBudget {

	// cSC4Simulator::OnTick busy-loops the simulation for up to a speed-dependent
	// budget (33/50/66 ms) before returning to the frame loop, so a heavy sim
	// starves rendering and input. This clamps that per-tick budget to a fixed
	// ceiling: the sim still advances every frame, just less per frame, and the
	// frame loop presents in between. The sim clock may lag wall-clock a little
	// more under load, in exchange for a smooth frame rate.
	//
	// Controlled by -SimTickCap:<ms> on the command line:
	//   absent      -> install with the default ceiling
	//   -SimTickCap:0 / off  -> do not install
	//   -SimTickCap:<ms>     -> install with that ceiling (clamped to [15, 500])
	//
	// Requires SimCity 4 Deluxe 1.1.641. Safe to call twice; only the first
	// installs. Returns true if the patch is active afterwards.
	bool Install(void);
	void Uninstall(void);

}
