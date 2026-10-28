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

#include "WireStream.h"

#include <cstdint>

class cIGZGDriver;

namespace nSCDWire {
	// What a replay target does that cIGZGDriver cannot express the same way in every process.
	class ReplayHooks {
	public:
		virtual ~ReplayHooks() = default;
		// Open a window of the recorded size. The recorded mode index means nothing on another machine.
		virtual void VideoMode(bool show, uint32_t width, uint32_t height, bool fullscreen) = 0;
		// Fill vertex buffer `name` with what the game wrote between GetVertices and ReleaseVertices.
		virtual void Vertices(uint32_t name, void const *data, uint32_t bytes) = 0;
		// After each Flush. Return false to stop the replay.
		virtual bool Frame() { return true; }
		virtual void CopyColorBuffer(int32_t, int32_t, int32_t, int32_t) {}
		virtual void Occupant(OccupantEvent const &) {}
		virtual void OwnerBegin(DrawOwner const &) {}
		virtual void OwnerEnd() {}
	};

	struct ReplayStats {
		uint64_t records = 0;
		uint64_t frames = 0;
		uint64_t opCounts[static_cast<size_t>(Op::Count)]{};
	};

	// Replays a stream into driver, which must also answer QueryInterface for the buffer region, lighting
	// and vertex buffer extensions. False on a damaged stream, with the reason in error.
	bool Replay(Reader &reader, cIGZGDriver &driver, ReplayHooks &hooks, ReplayStats *stats = nullptr,
	            char const **error = nullptr);
}
