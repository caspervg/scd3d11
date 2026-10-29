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

#include "SceneLane.h"

#include <memory>
#include <string>

namespace nSCD3D12 {
	struct UpscalerInputs;

	// The city's static models (buildings, props, flora) kept by the server and drawn by it every frame with the
	// game's camera, instead of coming out of SC4's backing store.
	//
	// SC4 draws the static view (passes 0 to 2) only when it has to: the whole window after a zoom or rotation,
	// the strip a scroll uncovers, the rectangle around something that changed. It saves the result in two buffer
	// regions, colour and depth, and starts every frame by copying them back (shifted by whole pixels while
	// scrolling), then draws the dynamic passes (3 to 5) and the UI over that.
	//
	// The retained scene takes every model the scene lane stands in for in the static pass (pass 1) out of that
	// pass, so the backing store only ever holds what the scene does not cover (terrain, networks, lot textures,
	// overlays, and models the lane leaves to the game). It keeps each model as an instance keyed by occupant and
	// model, with its meshes, its world transform and the full draw state the lane worked out. Just before the
	// first dynamic owner of each frame, when the backing store has been restored and SC4's static work is done,
	// it draws every instance on screen with that frame's camera, depth tested against the restored depth. The
	// dynamic passes then test against the instances, as they did against the game's own.
	//
	// The camera is the game's: at that point SC4 has loaded the full window's projection and its view matrix
	// (world to view) for the dynamic passes.
	//
	// Instances leave when the zoom or rotation changes (the models differ per view), when SC4 redraws a
	// rectangle that holds an instance's whole bounding box without drawing it, and once their occupant is
	// removed or moved, as soon as SC4 redraws any part of them (until then SC4's own view still shows them).
	//
	// It also writes a motion vector target: for every pixel, its position in the previous frame minus its
	// position now, in pixels. Whatever stands still gets it from the depth buffer and the two cameras,
	// instances that moved from their previous transform.
	class RetainedScene final : public SceneLane::Sink, public KeptModels {
	public:
		RetainedScene(cGDriver12 &driver, SceneLane &lane);
		~RetainedScene() override;

		// Before the driver sees the owner.
		void OwnerBegin(nSCDWire::DrawOwner const &owner);
		void Occupant(nSCDWire::OccupantEvent const &event);
		// A new video mode: the driver's textures are gone.
		void VideoMode();
		// Keeps this frame's motion vectors, for EndFrame to write.
		void KeepMotion();
		// After the driver submitted the frame. Writes the kept motion vectors as a PNG when path is set.
		void EndFrame(std::wstring const *motionPath);
		void PrintTotals() const;
		// Draws the instances from the next frame on with each pixel's sample this far from its centre, in pixels,
		// x right and y down, for DLAA. The motion vectors stay without it.
		void SetJitter(float x, float y);
		// This frame's motion vectors, camera and jitter, once the instances are drawn. False when the frame
		// showed no city camera.
		bool UpscalerCamera(UpscalerInputs &inputs) const;

		bool TakeMesh(nSCDWire::DrawOwner const &owner, nSCDWire::ModelInstance const &instance,
		              float const *bounds, uint32_t mesh, D3D12_VERTEX_BUFFER_VIEW const &vertices,
		              D3D12_INDEX_BUFFER_VIEW const &indices, uint32_t count) override;

		// KeptModels: every mesh of every instance kept, with the game's texture names.
		void VisitMeshes(MeshVisitor &visitor) const override;
		uint64_t Changes() const override;
		uint32_t TextureView(uint32_t texture) const override;

	private:
		struct Impl;
		std::unique_ptr<Impl> impl;
	};
}
