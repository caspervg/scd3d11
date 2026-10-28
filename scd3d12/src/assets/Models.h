/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// Models ready to upload, and which model an occupant shows at each zoom and rotation. DBPFKit parses the S3D
// and exemplar records. This header has no DBPFKit types, so code that includes it can stay C++20.

#pragma once

#include "AssetStore.h"

#include <span>
#include <string>
#include <vector>

namespace nSCAssets {
	constexpr uint32_t kS3dType = 0x5AD0E817;
	constexpr uint32_t kExemplarType = 0x6534284A;
	constexpr uint32_t kCohortType = 0x05342861;

	// S3D material flags as the game applies them (OpenSC4 graphics-formats §3.8). The game keeps the low 16 bits
	// of the file's flags and always adds depth and colour writes.
	enum : uint32_t {
		kMaterialAlphaTest = 0x01,
		kMaterialDepthTest = 0x02,
		kMaterialBackfaceCulling = 0x08,
		kMaterialBlend = 0x10,
		kMaterialTexturing = 0x20,
		kMaterialDepthWrite = 0x80,
		kMaterialColorWrite = 0x100,
	};

	struct MeshVertex {
		float position[3];
		float uv[2];
		// RGBA8, red in the low byte. White when the model has no vertex colours.
		uint32_t color;
	};

	struct MeshMaterial {
		uint32_t flags = 0;
		// Compare functions: 0 never, 1 less, 2 equal, 3 less or equal, 4 greater, 5 not equal, 6 greater or
		// equal, 7 always.
		uint8_t alphaFunc = 0, depthFunc = 0;
		// Blend factors: 0 zero, 1 one, 2 source colour, 3 one minus source colour, 4 source alpha, 5 one minus
		// source alpha, 6 dest alpha, 7 one minus dest alpha, 8 dest colour, 9 one minus dest colour, 10 source
		// alpha saturate.
		uint8_t srcBlend = 0, dstBlend = 0;
		// The alpha test reference as the game quantises it: the threshold's high byte to 4 bits.
		float alphaRef = 0;
		// The FSH instance of the first texture, 0 for none. See LoadModelTexture for the lookup.
		uint32_t texture = 0;
		// Texture parameters: 0 nearest, 1 linear, 2 clamp, 3 repeat, 4 to 7 the mipmapped filters.
		uint8_t wrapU = 3, wrapV = 3, magFilter = 0, minFilter = 0;
	};

	struct Mesh {
		std::string name;
		std::vector<MeshVertex> vertices;
		// A triangle list into this mesh's vertices.
		std::vector<uint16_t> indices;
		MeshMaterial material;
	};

	struct Model {
		std::vector<Mesh> meshes;
		// Over the vertices the meshes draw. Zero when there are none.
		float boundsMin[3] = {}, boundsMax[3] = {};
		uint16_t frameCount = 0, frameRate = 0;
	};

	// Builds one animation frame of an S3D record. Fails for a frame past the last, like the game.
	bool DecodeModel(std::span<uint8_t const> bytes, Model &model, uint32_t frame = 0, std::string *error = nullptr);
	bool LoadModel(AssetStore const &store, Tgi const &tgi, Model &model, uint32_t frame = 0, std::string *error = nullptr);

	// One model an occupant shows, with its offset from the occupant (RKT4 records carry one).
	struct ModelRef {
		Tgi tgi;
		float offset[3] = {};
	};

	// The model rotation the game uses for an occupant: (3 x camera rotation + orientation) mod 4, or
	// 3 x camera rotation mod 4 when the exemplar has property 0xE83A081D.
	uint32_t ModelRotation(uint32_t cameraRotation, uint32_t orientation, bool ignoreOrientation);

	// The models of one resource key property (RKT0 to RKT5, 0x27812820 to 0x27812825, or their alternates
	// 0x27812920 to 0x27812925) at zoom 0 (far) to 4 (near) and model rotation 0 to 3.
	// The rule, from cSC4ModelMakerUtility::GetModelInstanceID and GetAllModelInstanceIDs (Mac build 0x002059D4 and
	// 0x00205D44, Windows 0x00496830 and 0x00496C90 per OpenSC4 graphics-formats §4.2), with z the model level of
	// the zoom (z = zoom at the default detail, see lodOffset) and v the property's values:
	//   RKT0: (v0, v1, v2) at every zoom and rotation
	//   RKT1: (v0, v1, v2 + z x 0x100 + rotation x 0x10), nothing when v2 is 0
	//   RKT2: (v0, v1, v[2 + 4z + rotation]), 20 explicit instances
	//   RKT3: (v0, v1, v[2 + z]), one per zoom
	//   RKT4: records of (state, x, y, z, kind, then the values of an RKT0 to RKT3 key of that kind: 3 values for
	//         RKT0 and RKT1, 30 for RKT2, 5 for RKT3). Every record whose state matches gives a model at the offset
	//         x, y, z in signed 16.16 fixed point. The walk stops at an unknown kind or a record that runs past the end
	//   RKT5: the RKT1 rule plus state x 0x10000
	// lodOffset is the game's model detail setting (0 high, 1 medium, 2 low), which remaps zooms to coarser models.
	// Zero instances are left out (the game appends them and then finds no model).
	void ModelsFromKey(uint32_t property, std::span<uint32_t const> values, uint32_t zoom, uint32_t rotation, uint32_t state,
	                   std::vector<ModelRef> &out, uint32_t lodOffset = 0);

	// What GetAllModelInstanceIDs does with an exemplar and its parent cohorts: nothing when property 0x0ABFC024 is
	// set and its bit for this zoom is clear, else the first of RKT0, RKT1, RKT2, RKT3, RKT4, RKT5 the exemplar has
	// (or of their alternates 0x27812920 and up, which the game uses for props with occupant flag 0x80).
	bool OccupantModels(AssetStore const &store, Tgi const &exemplar, uint32_t zoom, uint32_t rotation, std::vector<ModelRef> &out,
	                    uint32_t state = 0, bool alternate = false, uint32_t lodOffset = 0);

	// A property of an exemplar or cohort, looked up through its parent cohorts as the game's effective list does
	// (the child's own value wins). Values are the raw 32-bit words, as the game reads keys. False when absent.
	bool ExemplarProperty(AssetStore const &store, Tgi const &exemplar, uint32_t property, std::vector<uint32_t> &values);
}
