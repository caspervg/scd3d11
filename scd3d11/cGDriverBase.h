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

#include "cIGZGDriver.h"
#include "ext/cIGZGBufferRegionExtension.h"
#include "ext/cIGZGDriverLightingExtension.h"
#include "ext/cIGZGDriverVertexBufferExtension.h"
#include "ext/cIGZGSnapshotExtension.h"
#include "sGDMode.h"
#include "wire/WireStream.h"

#include <cRZBaseUnknown.h>

#include <cstdint>
#include <memory>

namespace nSCD3D11 {
	// The driver cGDriverWire stands in front of: SCD3D11's cGDriver, which draws, or SCD3D12's cGDriverClient,
	// which keeps the state the game asks about and shows the server's frames.
	class cGDriverBase :
			public cIGZGDriver,
			public cIGZGBufferRegionExtension,
			public cIGZGDriverLightingExtension,
			public cIGZGDriverVertexBufferExtension,
			public cIGZGSnapshotExtension,
			public cRZBaseUnknown {
	public:
		// Each of these is declared by more than one of the interfaces.
		bool QueryInterface(uint32_t riid, void **ppvObj) override = 0;
		uint32_t AddRef(void) override = 0;
		uint32_t Release(void) override = 0;
		bool Init(void) override = 0;
		bool Shutdown(void) override = 0;

		// The back buffer's size now, which Flush may change when the window does.
		virtual void BackBufferSize(int32_t &width, int32_t &height) const = 0;
		// Whether the server draws the static view itself, so the game's own draws of it may be left out.
		virtual bool RemoteOwnsView() const = 0;
		// After Flush: the output of a server started since the last call, which cGDriverWire then brings up to
		// date. Null when none was.
		virtual std::unique_ptr<nSCDWire::Output> TakeRestartedOutput() { return nullptr; }
	};
}
