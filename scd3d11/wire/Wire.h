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
		// The window's client area changed size between two frames: width, height. Unlike SetVideoMode the
		// game keeps its textures and buffer regions.
		Resize,
		// The folders SimCity 4 loaded its data from, so the server can read the same packages: for each of
		// GameFolder's kinds in order, a uint32 count of UTF-16 code units, then the units.
		GameFolders,
		// After an OwnerBegin of a plain network piece: NetworkPiece, then its partCount NetworkParts.
		NetworkPiece,
		// After an OwnerBegin of a lot's cell texture (cSC4CellTextureModel): CellTexture.
		CellTexture,
		// After the OccupantInsert or OccupantMove of a building, prop or flora, and in the listing:
		// OccupantExemplar, then its count uint32 values.
		OccupantExemplar,

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

	// GameFolders, in the order SimCity 4 adds them to its resources (see AssetStore.h).
	enum class GameFolder : uint8_t {
		Install, // the install folder's *.dat files
		Language, // its language folder, English for example
		Sku, // Sku_Data, every *.dat below it
		Plugins, // the install folder's Plugins
		UserPlugins, // the user folder's Plugins
		Count
	};

	// A vertex as SC4 keeps it for networks and lot textures (format 10): position, BGRA colour, uv.
	struct GridVertex {
		float position[3];
		uint32_t color;
		float uv[2];
	};
	static_assert(sizeof(GridVertex) == 24);

	// A plain network piece (a cSC4NetworkOccupant drawn as a textured quad), read from the game's own fields
	// as it is drawn. It draws the under texture first when it has one, then the piece's texture, each as up to
	// two triangles of the quad, then its extra parts, one triangle each.
	struct NetworkPiece {
		uint32_t piece; // the texture id when variation is 0, and the fallback
		uint32_t underTexture; // 0 for none
		uint8_t rotationFlip; // rotation in the low 7 bits, bit 7 flipped
		uint8_t variation; // the texture is piece + 16 x variation
		uint8_t zoom; // the draw context's zoom, 0 to 4
		uint8_t highlighted; // drawn by the highlight pass, twice and self lit
		// Bit 0x2000000 draws the first triangle, 0x4000000 the second, 0x1000000 splits the quad along the
		// other diagonal.
		uint32_t flags;
		GridVertex corners[4]; // world space
		uint32_t partCount;
	};
	static_assert(sizeof(NetworkPiece) == 116);

	struct NetworkPart {
		uint32_t texture;
		GridVertex vertices[3]; // world space
	};
	static_assert(sizeof(NetworkPart) == 76);

	// A lot's cell texture (cSC4CellTextureModel), read as it is drawn: a 16 m square at (16 x, 16 z).
	struct CellTexture {
		uint32_t key; // the texture is (FSH, 0x0986135E, key + zoom), or key itself when that is missing
		uint8_t x, z;
		uint8_t orientation, mirror;
		uint8_t rgb[3], alpha;
		uint8_t mode; // 255 untinted, else the tint combiner's colour operation
		uint8_t element; // which of the texture binding's images is drawn
		uint8_t textureAlpha; // the bound texture has alpha
		uint8_t zoom; // the draw context's zoom, which the texture is loaded for
		uint8_t flat; // on a flat lot the model draws its own four vertices, otherwise the terrain draws it
		uint8_t highlighted; // the highlight pass, which draws nothing
		uint8_t bound; // the game found a texture
		uint8_t reserved;
		GridVertex vertices[4]; // a triangle strip, for a flat cell
	};
	static_assert(sizeof(CellTexture) == 116);

	enum class OccupantKind : uint8_t { Other, Building, Prop, Flora };

	// Which models an occupant shows, read from the occupant's own properties (which hold its exemplar's), so
	// the renderer can pick them for any zoom and rotation (see nSCAssets::ModelsFromKey).
	struct OccupantExemplar {
		uint64_t id;
		// The exemplar as far as the occupant tells: type 0x6534284A, group 0 (unknown), and the occupant's
		// building, prop or flora type as the instance.
		uint32_t type, group, instance;
		// The resource key property the values come from: RKT0 to RKT5 (0x27812820 to 0x27812825), else their
		// alternates (0x27812920 to 0x27812925), 0 for none.
		uint32_t property;
		// Property 0x0ABFC024, the zooms the occupant shows at (bit n for zoom n), all set when absent.
		uint32_t zoomMask;
		OccupantKind kind;
		uint8_t orientation; // 0 to 3
		uint8_t state; // a prop's or flora's state, for RKT4 and RKT5
		uint8_t flags; // bit 0: property 0xE83A081D is set, the model ignores the orientation
		uint32_t count;
		uint32_t reserved;
	};
	static_assert(sizeof(OccupantExemplar) == 40);

	char const *OpName(Op op);
}
