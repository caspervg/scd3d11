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

// The SimGL command stream SCD3D11 sends to SCD3D12Server, and writes to disk with Record=<file>. The live
// stream and a capture file are the same bytes, so any capture replays offline.
//
// A stream starts with a StreamHeader, then holds records. Each record is a RecordHeader followed by
// `size` bytes of fields in the order the writer put them, little endian, with no padding and no pointers.
// Large data (vertices, indices, texels) travels as a blob: a Blob record carries the bytes once, keyed by
// their 128-bit hash, and later records refer to it by that key. ForgetBlob tells the reader it may drop one.
// The writer decides what is forgotten, so both sides always agree on what the reader still holds.
//
// Both the 32-bit shim and the 64-bit server include this file, so nothing here may depend on pointer size.
namespace nSCDWire {
	constexpr uint32_t kMagic = 0x57444353; // "SCDW"
	constexpr uint32_t kVersion = 1;

	struct StreamHeader {
		uint32_t magic = kMagic;
		uint32_t version = kVersion;
	};

	struct RecordHeader {
		uint16_t op;
		uint16_t reserved;
		uint32_t size;
	};

	struct BlobKey {
		uint64_t low = 0;
		uint64_t high = 0;
		uint32_t size = 0;
		// Fills the padding a 64-bit aligned struct gets anyway, so no stray bytes reach the stream.
		uint32_t reserved = 0;

		bool operator==(BlobKey const &other) const {
			return low == other.low && high == other.high && size == other.size;
		}
	};

	// One op per driver call the server has to repeat. Queries the shim answers itself are absent.
	// Appending is fine, renumbering breaks old captures.
	enum class Op : uint16_t {
		Blob = 1,
		ForgetBlob,
		Frame, // Flush: the end of a frame

		// cIGZGDriver
		DrawArrays, // format, stride, primitive, count, vertex blob (vertices rebased to 0)
		DrawElements, // format, stride, primitive, count, index type, vertex blob, index blob (rebased)
		Clear,
		ClearColor,
		ClearDepth,
		ClearStencil,
		ColorMask,
		DepthFunc,
		DepthMask,
		StencilFunc,
		StencilMask,
		StencilOp,
		BlendFunc,
		AlphaFunc,
		ShadeModel,
		BindTexture,
		TexImage2D,
		PixelStore,
		TexEnvInt,
		TexEnvFloat,
		TexParameter,
		FogInt,
		FogFloat,
		ColorMultiplier,
		AlphaMultiplier,
		EnableVertexColors,
		GenTextures,
		DeleteTextures,
		MatrixMode,
		LoadMatrix,
		LoadIdentity,
		Enable,
		Disable,
		TexStage,
		TexStageCoord,
		TexStageMatrix,
		TexStageCombineMode,
		TexStageCombineSource,
		TexStageCombineOperand,
		TexStageCombineScale,
		SetTexture,
		CreateTexture,
		LoadTextureLevel,
		SetCombiner,
		SetVideoMode, // index, show, the mode's width, height and fullscreen flag
		PolygonOffset,
		Blit,
		SetViewportFull,
		SetViewport,
		Init,
		Shutdown,
		PuntBlitSourceAlpha,

		// cIGZGBufferRegionExtension
		NewBufferRegion,
		DeleteBufferRegion,
		ReadBufferRegion,
		DrawBufferRegion,
		DeleteAllBufferRegions,

		// cIGZGDriverLightingExtension
		EnableLighting,
		EnableLight,
		LightModelAmbient,
		LightColor,
		LightColors,
		LightPosition,
		LightDirection,
		MaterialColor,
		MaterialColors,

		// cIGZGDriverVertexBufferExtension
		GetVertices, // name, count
		ContinueVertices, // name, count
		ReleaseVertices, // name
		DrawPrims, // name, primitive, byte size, the vertices it reads as a blob
		DrawPrimsIndexed, // name, primitive, count, index blob, the vertices up to the highest index as a blob
		ResetVertexBuffers,

		// cIGZGSnapshotExtension
		CopyColorBuffer,

		// The game's occupants, from its occupant manager's messages: id (the occupant's address in the
		// game, unique while it lives), type, and the world bounding box as min xyz then max xyz.
		OccupantInsert,
		OccupantRemove, // id
		OccupantMove, // id, bounding box

		// Which game object the draws between the two belong to (see DrawOwnerHooks). They nest, because
		// a drawable can ask to be drawn again in the same pass.
		OwnerBegin, // DrawOwner
		OwnerEnd,
		// After an OwnerBegin of a cS3DModelInstance (buildings, props, flora, automata): ModelInstance.
		ModelInstance,

		Count
	};

	enum class BlitKind : uint8_t {
		BitBlt,
		StretchBlt,
		BitBltAlpha,
		StretchBltAlpha,
		BitBltAlphaModulate,
		StretchBltAlphaModulate,
	};

	enum class OccupantEventKind : uint8_t { Insert, Remove, Move };

	struct OccupantEvent {
		OccupantEventKind kind;
		uint64_t id;
		int32_t type;
		float box[6];
	};

	enum class DrawOwnerKind : uint8_t {
		Grid, // a model the renderer keeps in its grid: buildings, props, flora, networks, automata, lot textures
		View, // a view object: terrain, water, overlays such as shadows and decals, effects
	};

	struct DrawOwner {
		// The occupant, with the same id as the Occupant records, or 0 when the drawable has none.
		uint64_t occupant;
		// The drawable or view object itself, unique while it is in the renderer.
		uint64_t drawable;
		// The drawable's vtable address in the game, which tells the kind of view object apart.
		uint32_t vtable;
		DrawOwnerKind kind;
		// cSC43DRender's pass, from the draw context: 0 pre-static views, 1 static grid, 2 post-static views,
		// 3 pre-dynamic views, 4 dynamic grid, 5 post-dynamic views.
		uint8_t pass;
		uint16_t reserved;
	};
	static_assert(sizeof(DrawOwner) == 24);

	// What a cS3DModelInstance draws, read from the game's own fields as it is drawn.
	struct ModelInstance {
		// The S3D model resource (type 0x5AD0E817 for meshes), or all zero when the instance has none.
		uint32_t type;
		uint32_t group;
		uint32_t instance;
		// The world transform: world = rotation (row major) x (scale x vertex) + translation.
		uint8_t changeCount;
		uint8_t parts; // 4 rotation, 1 translation, 2 scale
		uint16_t reserved;
		float rotation[9];
		float translation[3];
		float scale;
		// Draw attributes: byte 0 bit 0 highlighted, bit 1 translucent, bit 2 self-lit shade in the RGB at 4
		// to 12, bit 3 skipped in the highlight pass. The float at 16 is the opacity.
		uint8_t attributes[20];
		// The animation frame, and properties bit 0 for a building under construction.
		uint16_t frame;
		uint16_t properties;
	};
	static_assert(sizeof(ModelInstance) == 92);

	char const *OpName(Op op);
}
