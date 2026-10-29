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

namespace nSCD3D12 {
	class KeptModels;
	class SceneLane;

	// Light from the server's own copy of the city, for the driver's relighting (cGDriver12::LightHooks).
	//
	//   camera        read from the matrices SC4 loads before its dynamic passes, as the kept scenes do, so
	//                 every pixel's depth gives its place in the world
	//   terrain       SC4's terrain draws (view object 0x00AB4480 in pass 0) carry the height field in world
	//                 space on its 16 metre grid. The heights are kept for the whole city as they arrive, with
	//                 the diagonal each cell is split along, and give terrain pixels smooth vertex normals
	//   sun shadows   with a kept scene (KeptModels: the retained or the owned scene) and a GPU with DXR 1.1: a
	//                 ray from every city pixel towards the sun through its models (a bottom level structure per
	//                 mesh, built once) and the terrain. Alpha-tested meshes look up the texel's alpha for each
	//                 candidate hit
	//   night lights  the night textures of the kept models SC4 lit (stage 1, the texture id + 0x8000) are read
	//                 once per mesh on the GPU for where the mesh glows and how much. Each glowing side of a lit
	//                 instance becomes a point light, binned into screen tiles for the lighting pass
	//
	// Without a kept scene there are no models to cast or glow, and only the terrain is lit from the world.
	class WorldLight final : public cGDriver12::LightHooks {
	public:
		struct Options {
			bool rays = true;
			bool terrain = true;
			bool lights = true;
		};
		WorldLight(cGDriver12 &driver, Options const &options);
		~WorldLight() override;

		// The models that cast shadows and glow. Either may be null.
		void SetScene(KeptModels const *models, SceneLane const *lane);
		void OwnerBegin(nSCDWire::DrawOwner const &owner);
		void OwnerEnd();
		// A new video mode: the driver's textures are gone.
		void VideoMode();
		// The whole city's terrain from the shim (TerrainField records). Its heights and diagonals stand above the
		// ones read from SC4's terrain draws.
		void Terrain(nSCDWire::TerrainField const &field, nSCDWire::TerrainVertex const *vertices);
		void PrintTotals() const;

		bool BeginLighting(cGDriver12::WorldFrame &frame) override;
		bool TraceSunlight(D3D12_GPU_VIRTUAL_ADDRESS constants, D3D12_GPU_DESCRIPTOR_HANDLE views) override;
		void IndexedVertices(uint8_t const *vertices, uint32_t bytes, uint32_t stride, uint16_t const *indices,
		                     uint32_t count) override;

	private:
		struct Impl;
		std::unique_ptr<Impl> impl;
	};
}
