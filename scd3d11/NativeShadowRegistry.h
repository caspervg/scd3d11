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

#include <cstdint>
#include <memory>
#include <vector>

// Replaces SC4's projected BAT shadows (buildings, flora, BAT props, poles) with
// casters in cGDriver's live shadow map. Opt-in with -NativeShadowMasks:replace,
// which also turns on every live path NativeShadowMasks:all enables.
//
// cSTEOverlayManager::AddShadow already turns each caster mesh into a world-to-UV
// projector for SC4's prerendered texture. This module keeps that projector, the
// texture and the mesh for every shadow record the game creates, takes those
// records out of the list DrawShadows draws, and hands the driver the ones each
// static pass would have drawn. See docs/true3d-shadows-replace-all.md.

namespace nSCD3D11::NativeShadowRegistry {
	struct Caster {
		uint32_t id = 0;
		void const *manager = nullptr;
		void const *binding = nullptr;
		// World-space positions, three floats per vertex.
		std::vector<float> positions;
		// Triangle list into positions. Empty when the mesh's topology could not
		// be recovered; the caster then casts from its ground quad alone.
		std::vector<uint32_t> indices;
		// cSTEOverlayManager record +0x1C: world position to texture UV,
		// column-major, constant along the sun direction.
		float projector[16]{};
		// u min, v min, u max, v max over the mesh's UVs; DrawShadows clips there.
		float uvBounds[4]{};
		float low[3]{};
		float high[3]{};
		// The occupant's placement height, which is where its ground quad sits.
		float baseHeight = 0.0f;
	};

	struct PassCaster {
		std::shared_ptr<Caster const> caster;
		// cGDriver texture name, read through the record's texture binding when
		// the pass was gathered.
		uint32_t texture = 0;
		// Record flag bit 1: DrawShadows samples with wrap instead of clamp.
		bool wrap = false;
	};

	// Everything the DrawShadows call sites handed over since the last TakePass.
	struct Pass {
		// DrawShadows' first argument: eye space to world, the texgen transform
		// SC4 composes with each record's projector.
		float eyeToWorld[16]{};
		// World-space direction the light travels, from GetShadowDirection.
		float sunDirection[3]{};
		bool sunValid = false;
		// GetShadowParams: the colour DrawShadows blends, its strength, and the
		// alpha-test scale (the reference is strength * alphaScale).
		float colour[3]{};
		float strength = 0.0f;
		float alphaScale = 0.0f;
		std::vector<PassCaster> casters;
	};

	bool Install();
	void Uninstall();
	bool Enabled();

	// Moves out the pass gathered since the last call. False when no
	// DrawShadows call ran in between, i.e. SC4 drew no shadows.
	bool TakePass(Pass &pass);

	// Asks SC4 to redraw the terrain cells under a world-space rectangle (x and
	// z), the way RedisplayStaticOverlay does when a shadow record is added or
	// removed. The redraw happens as a partial static pass on a later frame.
	bool RedisplayWorldRect(float minX, float minZ, float maxX, float maxZ);

	// Terrain altitude at a world position; false when there is no city.
	bool TerrainAltitude(float x, float z, float &altitude);

	// Exposed for tests: SC4's SimGL primitive enumeration to a triangle list.
	// Points and lines append nothing and return false.
	bool AppendTriangles(uint32_t primitive, uint32_t const *indices, uint32_t count,
	                     std::vector<uint32_t> &triangles);
} // namespace nSCD3D11::NativeShadowRegistry
