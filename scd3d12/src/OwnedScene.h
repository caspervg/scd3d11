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
#include "wire/FreeCamera.h"

#include <memory>
#include <string>

namespace nSCD3D12 {
	struct UpscalerInputs;

	// The city's whole static view kept by the server and drawn by it every frame with the game's camera: the
	// background, terrain and water, lot textures, networks, buildings, props, flora and overlays. SC4's
	// backing store is not used.
	//
	// SC4 draws its static view only when it has to: everything after a zoom or rotation, the strip a scroll
	// uncovers, the rectangle around a change. Every frame starts with a static window, from the frame's start
	// to its first dynamic owner, where SC4 restores its backing store and draws what it has to. The owned scene
	// takes every draw of that window (cGDriver12::DrawCapture) and keeps it, and skips the backing store's
	// copies. At the first dynamic owner it clears the frame and draws everything it keeps with the frame's
	// camera. The dynamic passes and the UI then draw over that as the game sent them.
	//
	// What is kept, and when it goes:
	//   models      from the scene lane, as instances of its meshes, keyed by occupant (or drawable) and model,
	//               as RetainedScene keeps them
	//   terrain     the terrain view object's draws (passes 0 and 2), split into its 16 metre cells. A redraw
	//               replaces the cells it draws, with every layer, and a cell that stays the same keeps its
	//               geometry on the GPU
	//   grid        every other drawable of the static grid (lot textures, networks, models the lane leaves to
	//               the game), keyed by drawable
	//   view        other view objects (overlays such as shadows and decals) and the background SC4 draws first
	//               on a full redraw, keyed by what they draw
	// Drawables go once a redraw holds their whole box without drawing them, or holds any part of them once their
	// occupant left. What view objects draw goes once a redraw overlaps it without drawing it the same. A new zoom
	// or rotation keeps only what SC4 drew with it.
	//
	// All of it is world space geometry with the state the game drew it with (cGDriver12::CapturedState), so a
	// camera SC4 never drew with can draw it too. Draws with the same state share one pipeline, one set of
	// constants and textures: models as instances of one mesh, everything else merged per 256 metre tile into
	// buffers that grow in place.
	class OwnedScene final : public SceneLane::Sink, public cGDriver12::DrawCapture {
	public:
		OwnedScene(cGDriver12 &driver, SceneLane &lane);
		~OwnedScene() override;

		// Before the driver and the lane see the owner.
		void OwnerBegin(nSCDWire::DrawOwner const &owner);
		// After them.
		void OwnerEnd();
		void Occupant(nSCDWire::OccupantEvent const &event);
		void OccupantExemplar(nSCDWire::OccupantExemplar const &occupant, uint32_t const *values);
		// Counts, at each composite, how many of the models SC4 drew this frame its occupant's record picks too
		// (PrintTotals). Needed by KeepView.
		void CheckExemplars();
		// Keeps this frame's camera, and the zoom and rotation SC4 drew it with. From then on every model captured
		// also gets its occupant's models for that view, picked from its record. False when the frame has no
		// camera or no zoom could be found.
		bool KeepView();
		// Draws the following frames' kept scene from the kept camera, with the models picked for it. Models
		// without a record stay as SC4 picked them, turned with the rest.
		void UseKeptView(bool on);
		// Draws the following frames from the player's own camera (nSCDWire::FreeCamera), told as a change to SC4's
		// camera about the ground at the middle of the screen, until a camera that is off. Every occupant then
		// shows its models for the camera's nearest SC4 zoom and rotation, picked from its record, and nothing is
		// culled. Inputs() holds the camera it drew with.
		void SetFreeCamera(nSCDWire::FreeCamera const &camera);
		// The pixel the player points at, from the window's top left. While the free camera is on, the world point
		// drawn there is read back from the depth buffer, a few frames late.
		void SetCursor(int32_t x, int32_t y);
		// What the last frame drew with the free camera, for picking in the game.
		struct FreeState {
			bool drawn = false;
			uint32_t width = 0, height = 0;
			// Column major, without jitter.
			float view[16]{}, projection[16]{};
			// The ground point the camera turns about.
			float pivot[3]{};
			// The latest world point read back under cursor, and its depth (1 where nothing was drawn).
			bool cursorValid = false;
			int32_t cursor[2]{};
			float cursorWorld[4]{};
		};
		FreeState const &Free() const;
		// A new video mode: the driver's textures are gone.
		void VideoMode();
		// After the driver submitted the frame.
		void EndFrame();
		// Whether the next frame's static view is the scene's own.
		bool Owning() const;
		void PrintTotals() const;

		// What a temporal upscaler or frame generator needs of the owned view, for each frame the scene drew.
		// Everything is at the window's size for now: the render size equals the output size until the city gets
		// a target of its own.
		struct FrameInputs {
			bool valid = false;
			// The city before the UI, in RENDER_TARGET.
			ID3D12Resource *color = nullptr;
			// The driver's depth buffer, in DEPTH_WRITE. It holds the city before the UI draws.
			ID3D12Resource *depth = nullptr;
			// Where each pixel was in the previous frame minus where it is now, in pixels (RG16F, in
			// RENDER_TARGET). The camera's motion only, without jitter.
			ID3D12Resource *motion = nullptr;
			// The sub-pixel offset the owned view was drawn with, in pixels.
			float jitter[2]{};
			// Column major, without jitter.
			float projection[16]{}, view[16]{}, previousProjection[16]{}, previousView[16]{};
			uint32_t renderWidth = 0, renderHeight = 0, outputWidth = 0, outputHeight = 0;
		};
		// Moves every owned draw of the following frames by a fraction of a pixel, (0.5, 0.5) being half a pixel
		// right and down. A Halton sequence per frame, for example.
		void SetJitter(float x, float y);
		FrameInputs const &Inputs() const;
		// The same for DLSS (Dlss.h): motion vectors, the city before the UI as the HUD-less colour, the camera and
		// the jitter. False when the frame showed no owned view.
		bool UpscalerCamera(UpscalerInputs &inputs) const;
		// Draws what is kept from a camera moved by x, y pixels along the screen, to show what SC4 is not drawing
		// now. The dynamic passes stay where SC4 put them.
		void SetLook(float x, float y);
		// Writes this frame's inputs as <prefix>city.png and <prefix>motion.png (the motion's direction as the
		// hue, its length as the brightness), and prints the average motion. After the driver submitted the frame.
		void SaveInputs(std::wstring const &prefix);

		bool TakeMesh(nSCDWire::DrawOwner const &owner, nSCDWire::ModelInstance const &instance,
		              float const *bounds, uint32_t mesh, D3D12_VERTEX_BUFFER_VIEW const &vertices,
		              D3D12_INDEX_BUFFER_VIEW const &indices, uint32_t count) override;
		bool Capture(cGDriver12::CapturedState const &state, nSCD3D11::D3D11Vertex const *vertices,
		             uint32_t vertexCount, uint32_t const *indices, uint32_t indexCount, uint32_t format) override;

	private:
		struct Impl;
		std::unique_ptr<Impl> impl;
	};
}
