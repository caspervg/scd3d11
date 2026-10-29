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

#include <cstdio>
#include <memory>

namespace nSCAssets {
	class AssetStore;
	struct Tgi;
}

namespace nSCD3D12 {
	// Draws the game's S3D models (buildings, props, flora, automata) from the server's own asset store, in place
	// of the draws the game sends for them.
	//
	// Every draw inside a ModelInstance bracket is one mesh of that model, in the model's order, so the lane
	// swaps the game's draw for its own copy of that mesh: vertices and indices kept on the GPU per model, the
	// textures read from the store, and the render state worked out from the S3D material and the instance's
	// draw attributes the way SC4DrawContext::SetRenderState does. The transform, the shade from the game's
	// lighting, fog and the stage 0 texture mode are the ones the game has set for the draw. Each draw the lane
	// cannot stand in for exactly is left to the game, and counted with the reason.
	//
	// Plain network pieces and lot cell textures are drawn from their records (NetworkPiece, CellTexture): the
	// lane matches each of the game's draws to a triangle of the record, draws the record's vertices with the
	// texture it reads from the store, and keeps the rest of the game's state. A cell on a slope is drawn by
	// the terrain, so there the lane keeps the game's vertices and only swaps the texture.
	//
	// With check set, every draw the lane stands in for is also compared with the game's: the render state the
	// lane worked out against the one the game set, and the lane's vertices against the game's. For networks
	// and cells it checks that each of the game's textures always matches the same store texture and back.
	class SceneLane final : public cGDriver12::DrawFilter {
	public:
		SceneLane(cGDriver12 &driver, nSCAssets::AssetStore const &store, bool check);
		~SceneLane() override;

		void OwnerBegin(nSCDWire::DrawOwner const &owner);
		void OwnerEnd();
		void ModelInstance(nSCDWire::ModelInstance const &model);
		void NetworkPiece(nSCDWire::NetworkPiece const &piece, nSCDWire::NetworkPart const *parts);
		void CellTexture(nSCDWire::CellTexture const &cell);
		bool AllowDraw(uint32_t primitive, int32_t first, uint32_t count, uint32_t indexType,
		               void const *indices) override;

		// Writes the counts of the frame that just ended as one line of `csv` (when not null) and starts the
		// next frame.
		void EndFrame(uint64_t frame, std::FILE *csv);
		static void WriteCsvHeader(std::FILE *csv);
		void PrintTotals() const;

		// Offered each mesh the lane stands in for, with the lane's state set on the driver, in place of the lane
		// drawing it. bounds is the model's box in model space, min then max. Returning true keeps the mesh out
		// of the frame.
		class Sink {
		public:
			virtual ~Sink() = default;
			virtual bool TakeMesh(nSCDWire::DrawOwner const &owner, nSCDWire::ModelInstance const &instance,
			                      float const *bounds, uint32_t mesh, D3D12_VERTEX_BUFFER_VIEW const &vertices,
			                      D3D12_INDEX_BUFFER_VIEW const &indices, uint32_t count) = 0;
		};
		void SetSink(Sink *sink);
		// Off leaves network pieces and lot cells to the game's own draws.
		void SubstituteGrid(bool on);

		// The GPU buffer that holds the lane's geometry at address, and the address's offset in it. Null when the
		// lane has none there.
		ID3D12Resource *GeometryBuffer(D3D12_GPU_VIRTUAL_ADDRESS address, uint64_t &offset) const;
		// On drops every draw of the dynamic passes (3 to 5): automata, effects, animated sprites.
		void DropDynamic(bool on);
		nSCAssets::AssetStore const &Store() const;
		// The FSH (group, id) as a driver texture, its image element or else its first, 0 when the store has none.
		// Loaded once. Changes the active stage's binding, so it runs inside a saved draw state.
		uint32_t StoreTexture(uint32_t group, uint32_t id, uint32_t element);
		// Puts any S3D model (type 0x5AD0E817) on the GPU, once per key, and gives its box in model space (min then
		// max). Returns how many meshes it has, 0 when the store has none.
		uint32_t LoadModel(nSCAssets::Tgi const &model, float *bounds);
		struct MeshDraw {
			D3D12_VERTEX_BUFFER_VIEW vertices{};
			D3D12_INDEX_BUFFER_VIEW indices{};
			uint32_t count = 0;
		};
		// Sets the driver's state for one mesh of a model as the game draws it with these draw attributes (the 20
		// bytes of ModelInstance::attributes), on top of the state the driver holds, and gives its buffers. False
		// when the lane would leave such a draw to the game.
		bool PrepareMesh(nSCAssets::Tgi const &model, uint32_t mesh, uint8_t const *attributes, MeshDraw &out);

	private:
		struct Impl;
		std::unique_ptr<Impl> impl;
	};

	// The models a scene keeps (RetainedScene.h, OwnedScene.h), for lighting from the server's own copy of the city
	// (WorldLight.h). Their meshes are the lane's, so GeometryBuffer finds them.
	class KeptModels {
	public:
		virtual ~KeptModels() = default;
		class MeshVisitor {
		public:
			virtual ~MeshVisitor() = default;
			virtual void Mesh(float const *world, D3D12_VERTEX_BUFFER_VIEW const &vertices,
			                  D3D12_INDEX_BUFFER_VIEW const &indices, uint32_t count,
			                  cGDriver12::DrawState const &state) = 0;
		};
		// Every mesh of every model kept: the model's world transform (column major), the mesh's geometry and the
		// state it is drawn with.
		virtual void VisitMeshes(MeshVisitor &visitor) const = 0;
		// Grows whenever what VisitMeshes visits changes.
		virtual uint64_t Changes() const = 0;
		// A texture of those states as a slot of the driver's view table, readable by every shader stage. 0 for none.
		virtual uint32_t TextureView(uint32_t texture) const = 0;
	};
}
