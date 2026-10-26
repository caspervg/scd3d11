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
// casters in cGDriver's live shadow map, as part of -LiveShadows:on.
//
// cSTEOverlayManager::AddShadow already turns each caster mesh into a world-to-UV
// projector for SC4's prerendered texture. This module keeps that projector, the
// texture and the mesh for every shadow record the game creates, takes those
// records out of the list DrawShadows draws, and hands the driver the ones each
// static pass would have drawn. True3D props SC4 would not shadow (no Is Ground
// Model) come through the same call once LiveShadowHooks lets it run, and cast
// with their own UVs. Because SC4 tracks every record's lifetime and dirties its
// rectangle when it changes, none of these casters needs a full redraw to stay
// correct.

namespace nSCD3D11::ShadowRegistry {
	// Floats per Caster vertex: world position, then the mesh UV (zero unless meshUVs).
	constexpr uint32_t kVertexFloats = 5;

	struct Caster {
		uint32_t id = 0;
		void const *manager = nullptr;
		void const *binding = nullptr;
		// kVertexFloats per vertex, laid out as the driver's shadow pass reads them.
		std::vector<float> vertices;
		// Triangle list into vertices. Empty when the mesh's topology could not
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
		// A True3D prop SC4 itself would not shadow: it casts with its own UVs
		// (in vertices) rather than through the projector, which only fits a
		// prerendered view.
		bool meshUVs = false;
		// Taken out of DrawShadows but not cast: a relaxed prop the registry
		// could not keep, which casts through the per-draw path instead.
		bool suppressOnly = false;
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
		// The projection the casters were culled against (see NoteProjection),
		// when culled is set.
		float projection[16]{};
		bool culled = false;
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

	// Marks the next AddShadow on this thread as a relaxed True3D prop
	// (LiveShadowHooks site D), to be kept with its own UVs.
	void ExpectMeshCaster();

	// The ground quad a caster adds for silhouettes its mesh does not cover
	// from the sun (tree cards, thin LOD shells): the caster's box pushed down
	// the sun (the world direction light travels, normalized) onto the plane
	// at its placement height. Every point of it shares its UV with the
	// terrain point below along the sun, so on flat ground it reproduces SC4's
	// projected shadow exactly. False when the sun is at or above the horizon.
	bool GroundQuad(Caster const &caster, float const sun[3], float quad[4][3]);

	// True when the world box holding the caster and its quad (if hasQuad)
	// lies outside the NDC square of worldToClip, an affine column-major
	// matrix: then nothing the caster shades is in that view.
	bool ReachOutside(Caster const &caster, bool hasQuad, float const quad[4][3], float const *worldToClip);

	// The driver's projection matrix whenever it changes. A static pass's
	// DrawShadows runs under that pass's projection, so the gather culls the
	// pass's casters against it rather than handing over thousands the pass
	// cannot see. Render thread only.
	void NoteProjection(float const projection[16]);

	// Moves out the pass gathered since the last call. False when no
	// DrawShadows call ran in between, i.e. SC4 drew no shadows.
	bool TakePass(Pass &pass);

	// Asks SC4 to redraw the terrain cells under a world-space rectangle (x and
	// z), the way RedisplayStaticOverlay does when a shadow record is added or
	// removed. The redraw happens as a partial static pass on a later frame.
	bool RedisplayWorldRect(float minX, float minZ, float maxX, float maxZ);

	// The same for a terrain the caller already has.
	bool RedisplayTerrainRect(void *terrain, bool updateView, float minX, float minZ, float maxX, float maxZ);

	// -DrawStats: time spent in the AddShadow and DrawShadows hooks since the last call.
	struct Stats {
		uint32_t registrations = 0;
		// AddShadow calls made on another thread than DrawShadows.
		uint32_t offThreadRegistrations = 0;
		double registerMs = 0.0;
		double gatherMs = 0.0;
		// SC4's own DrawShadows for the records the registry left it.
		double nativeDrawMs = 0.0;
	};
	Stats TakeStats();

	// Exposed for tests: SC4's SimGL primitive enumeration to a triangle list.
	// Points and lines append nothing and return false.
	bool AppendTriangles(uint32_t primitive, uint32_t const *indices, uint32_t count,
	                     std::vector<uint32_t> &triangles);
} // namespace nSCD3D11::ShadowRegistry
