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

	char const *OpName(Op op);
}
