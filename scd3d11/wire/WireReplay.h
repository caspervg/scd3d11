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

#include "FreeCamera.h"
#include "WireStream.h"

#include <cstdint>
#include <string>

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
		// Before each draw: the blobs it reads its vertices and indices from, size 0 for none. A blob's
		// bytes never change under its key, so a target may keep what it made from them.
		virtual void DrawBlobs(BlobKey const &, BlobKey const &) {}
		// After each Flush. Return false to stop the replay.
		virtual bool Frame() { return true; }
		virtual void CopyColorBuffer(int32_t, int32_t, int32_t, int32_t) {}
		virtual void Occupant(OccupantEvent const &) {}
		virtual void OwnerBegin(DrawOwner const &) {}
		virtual void OwnerEnd() {}
		virtual void ModelInstance(nSCDWire::ModelInstance const &) {}
		// The window changed size: the back buffer and buffer regions follow, textures stay.
		virtual void Resize(uint32_t, uint32_t) {}
		virtual void GameFolders(std::wstring const (&)[static_cast<size_t>(GameFolder::Count)]) {}
		// parts holds piece.partCount parts.
		virtual void NetworkPiece(nSCDWire::NetworkPiece const &, NetworkPart const *) {}
		virtual void CellTexture(nSCDWire::CellTexture const &) {}
		// values holds occupant.count values.
		virtual void OccupantExemplar(nSCDWire::OccupantExemplar const &, uint32_t const *) {}
		virtual void FreeCamera(nSCDWire::FreeCamera const &) {}
		// vertices holds field.verticesX x field.verticesZ values.
		virtual void TerrainField(nSCDWire::TerrainField const &, TerrainVertex const *) {}
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
