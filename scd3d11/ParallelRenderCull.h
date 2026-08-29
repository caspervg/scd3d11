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

namespace nSCD3D11::ParallelRenderCull {

	// Installs inline hooks on cSC43DRender::DrawStaticView / DrawDynamicView that
	// move the quad-grid visibility gather + sort-key computation off the render
	// thread onto a small worker pool. Sorting and draw-call submission stay on the
	// render thread, byte-for-byte identical to the stock path.
	//
	// Controlled by the SC4D3D11_PARALLEL_CULL environment variable:
	//   unset / "1" / "on"  -> parallel gather (default)
	//   "serial"            -> reimplemented gather, single threaded (debugging aid)
	//   "0" / "off"         -> do not install; stock code runs untouched
	//
	// Safe to call more than once; only the first call installs. Returns true if the
	// hooks are active afterwards.
	bool Install(void);

	// Removes the hooks and tears the worker pool down. Safe if Install never ran.
	void Uninstall(void);

}
