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

#include "cGDriver12.h"
#include "wire/Wire.h"

#include <memory>
#include <vector>

namespace nSCD3D12 {
	class SceneLane;

	// The whole city's terrain and water as the shim sends it (TerrainField records), drawn by the owned scene
	// where SC4 never drew: past the edges of its view, and wherever the player's own camera looks.
	//
	// It is drawn the way SC4 draws its terrain (cSTETerrainView3D): a dark untextured base, then each texture
	// added over the cells that use it, weighted by the vertex alpha, then the water surface blended over the
	// ground. The render states are SC4's own, taken from its terrain draws (Learn), with the textures read
	// from the server's asset store.
	class TerrainField {
	public:
		TerrainField(cGDriver12 &driver, SceneLane &lane);
		~TerrainField();

		void Set(nSCDWire::TerrainField const &field, nSCDWire::TerrainVertex const *vertices);
		// One of SC4's terrain draws (the terrain view object, pass 0 or 2) as the owned scene captures it, with
		// the driver's state still the draw's: keeps the states the field draws with. True for a texture SC4 blends
		// over its terrain.
		bool Learn(cGDriver12::CapturedState const &state, uint32_t pass, nSCD3D11::D3D11Vertex const *vertices,
		           uint32_t const *indices, uint32_t indexCount);
		// Cells across x and z, 0 without a field. kept in Prepare is indexed x + z * cells x.
		uint32_t CellsX() const;
		uint32_t CellsZ() const;
		// Before the owned scene starts drawing: builds what changed. kept says which cells SC4 drew: 0 none, 1 only
		// its untextured base (cells it hides under a lot), which get the textures and water over it, and 2 all of
		// it, which is left out. It changes whenever version does. False when there is nothing to draw.
		bool Prepare(std::vector<uint8_t> const &kept, uint64_t version);
		// Between cGDriver12::BeginRetained and EndRetained, after SC4's terrain and before what stands on it. view is
		// the instance holding the camera's rows.
		void Draw(D3D12_VERTEX_BUFFER_VIEW const &view);
		// The driver's textures are gone.
		void VideoMode();

	private:
		struct Impl;
		std::unique_ptr<Impl> impl;
	};
}
