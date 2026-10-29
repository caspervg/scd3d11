/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#include "cGDriverWire.h"

#include "cGDCombiner.h"
#include "Diagnostics.h"
#include "DrawOwnerHooks.h"
#include "Settings.h"
#include "TextureUploadUtils.h"
#include "VertexFormatUtils.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

using nSCDWire::BlitKind;
using nSCDWire::Op;

namespace nSCD3D11 {
	namespace {
		constexpr uint32_t kPuntBlitSourceAlpha = 0x6C4236E7;

		struct Float4 {
			float v[4]{};
		};

		Float4 Copy4(float const *source, size_t count = 4) {
			Float4 result;
			if (source != nullptr) std::memcpy(result.v, source, count * sizeof(float));
			return result;
		}

		// Bytes of `count` vertices `stride` apart, the last one only as long as the format.
		uint32_t SpanBytes(uint32_t format, uint32_t stride, uint32_t count) {
			if (count == 0) return 0;
			uint64_t const bytes = static_cast<uint64_t>(stride) * (count - 1) + RZVertexFormatStride(format);
			return bytes > UINT32_MAX ? 0 : static_cast<uint32_t>(bytes);
		}
	}

	namespace {
		cGDriverWire *gCurrent = nullptr;
	}

	cGDriverWire::cGDriverWire(cGDriver *driver, nSCDWire::Writer::Sink sink)
		: driver(driver), writer(std::move(sink)) {
		driver->AddRef();
		gCurrent = this;
	}

	cGDriverWire::~cGDriverWire() {
		if (gCurrent == this) gCurrent = nullptr;
		writer.Flush();
		driver->Release();
	}

	cGDriverWire *cGDriverWire::Current() { return gCurrent; }

	void cGDriverWire::RecordOccupant(nSCDWire::OccupantEvent const &event) {
		if (event.kind == nSCDWire::OccupantEventKind::Remove) {
			Record(Op::OccupantRemove, event.id);
			return;
		}
		writer.Begin(event.kind == nSCDWire::OccupantEventKind::Insert ? Op::OccupantInsert : Op::OccupantMove);
		writer.Put(event.id);
		writer.Put(event.type);
		writer.PutBytes(event.box, sizeof(event.box));
		writer.End();
	}

	bool cGDriverWire::QueryInterface(uint32_t riid, void **ppvObj) {
		if (ppvObj == nullptr) return false;
		*ppvObj = nullptr;
		switch (riid) {
			case GZIID_cIGZUnknown:
			case GZIID_cIGZGDriver:
				*ppvObj = static_cast<cIGZGDriver *>(this);
				break;
			case GZIID_cIGZGBufferRegionExtension:
				*ppvObj = static_cast<cIGZGBufferRegionExtension *>(this);
				break;
			case GZIID_cIGZGDriverLightingExtension:
				*ppvObj = static_cast<cIGZGDriverLightingExtension *>(this);
				break;
			case GZIID_cIGZGDriverVertexBufferExtension:
				*ppvObj = static_cast<cIGZGDriverVertexBufferExtension *>(this);
				break;
			case GZIID_cIGZGSnapshotExtension:
				*ppvObj = static_cast<cIGZGSnapshotExtension *>(this);
				break;
			default:
				return false;
		}
		AddRef();
		return true;
	}

	uint32_t cGDriverWire::AddRef(void) { return cRZBaseUnknown::AddRef(); }
	uint32_t cGDriverWire::Release(void) { return cRZBaseUnknown::Release(); }
	bool cGDriverWire::FinalRelease(void) { return true; }

	// Geometry. Vertices are rebased so the first one used sits at the start of the blob.

	void cGDriverWire::DrawArrays(uint32_t primitive, int32_t first, int32_t count) {
		if (vertexPointer != nullptr && first >= 0 && count > 0) {
			writer.Begin(Op::DrawArrays);
			writer.Put(vertexFormat);
			writer.Put(vertexStride);
			writer.Put(primitive);
			writer.Put(count);
			writer.PutBlob(vertexPointer + static_cast<size_t>(first) * vertexStride,
			               SpanBytes(vertexFormat, vertexStride, static_cast<uint32_t>(count)));
			writer.End();
		}
		driver->DrawArrays(primitive, first, count);
	}

	void cGDriverWire::DrawElements(uint32_t primitive, int32_t count, uint32_t type, void const *indices) {
		if (vertexPointer != nullptr && count > 0 && indices != nullptr && (type == 3 || type == 5)) {
			uint32_t const indexSize = type == 3 ? 2 : 4;
			uint32_t minimum = UINT32_MAX;
			uint32_t maximum = 0;
			auto const scan = [&](auto const *source) {
				for (int32_t i = 0; i < count; ++i) {
					minimum = (std::min)(minimum, static_cast<uint32_t>(source[i]));
					maximum = (std::max)(maximum, static_cast<uint32_t>(source[i]));
				}
			};
			if (type == 3) scan(static_cast<uint16_t const *>(indices));
			else scan(static_cast<uint32_t const *>(indices));

			void const *rebased = indices;
			if (minimum != 0) {
				scratch.resize(static_cast<size_t>(count) * indexSize);
				if (type == 3) {
					auto const *source = static_cast<uint16_t const *>(indices);
					auto *target = reinterpret_cast<uint16_t *>(scratch.data());
					for (int32_t i = 0; i < count; ++i) target[i] = static_cast<uint16_t>(source[i] - minimum);
				} else {
					auto const *source = static_cast<uint32_t const *>(indices);
					auto *target = reinterpret_cast<uint32_t *>(scratch.data());
					for (int32_t i = 0; i < count; ++i) target[i] = source[i] - minimum;
				}
				rebased = scratch.data();
			}
			writer.Begin(Op::DrawElements);
			writer.Put(vertexFormat);
			writer.Put(vertexStride);
			writer.Put(primitive);
			writer.Put(count);
			writer.Put(type);
			writer.PutBlob(vertexPointer + static_cast<size_t>(minimum) * vertexStride,
			               SpanBytes(vertexFormat, vertexStride, maximum - minimum + 1));
			writer.PutBlob(rebased, static_cast<uint32_t>(count) * indexSize);
			writer.End();
		}
		driver->DrawElements(primitive, count, type, indices);
	}

	void cGDriverWire::InterleavedArrays(uint32_t format, int32_t stride, void const *pointer) {
		uint32_t const packed = RZVertexFormatStride(format);
		// The same checks the driver makes: a call it rejects leaves the previous arrays in place.
		if (packed != 0 && pointer != nullptr && stride >= 0 &&
		    (stride == 0 || static_cast<uint32_t>(stride) >= packed)) {
			vertexFormat = format;
			vertexStride = stride == 0 ? packed : static_cast<uint32_t>(stride);
			vertexPointer = static_cast<uint8_t const *>(pointer);
		}
		driver->InterleavedArrays(format, stride, pointer);
	}

	uint32_t cGDriverWire::MakeVertexFormat(uint32_t count, intptr_t elements) {
		return driver->MakeVertexFormat(count, elements);
	}
	uint32_t cGDriverWire::MakeVertexFormat(uint32_t format) { return driver->MakeVertexFormat(format); }
	uint32_t cGDriverWire::VertexFormatStride(uint32_t format) { return driver->VertexFormatStride(format); }
	uint32_t cGDriverWire::VertexFormatElementOffset(uint32_t format, uint32_t element, uint32_t index) {
		return driver->VertexFormatElementOffset(format, element, index);
	}
	uint32_t cGDriverWire::VertexFormatNumElements(uint32_t format, uint32_t element) {
		return driver->VertexFormatNumElements(format, element);
	}

	// Render state.

	void cGDriverWire::Clear(uint32_t mask) {
		Record(Op::Clear, mask);
		driver->Clear(mask);
	}
	void cGDriverWire::ClearColor(float r, float g, float b, float a) {
		Record(Op::ClearColor, r, g, b, a);
		driver->ClearColor(r, g, b, a);
	}
	void cGDriverWire::ClearDepth(double depth) {
		Record(Op::ClearDepth, depth);
		driver->ClearDepth(depth);
	}
	void cGDriverWire::ClearStencil(int32_t stencil) {
		Record(Op::ClearStencil, stencil);
		driver->ClearStencil(stencil);
	}
	void cGDriverWire::ColorMask(bool enabled) {
		RecordState(kColorMaskSlot, Op::ColorMask, enabled);
		driver->ColorMask(enabled);
	}
	void cGDriverWire::DepthFunc(uint32_t function) {
		RecordState(kDepthFuncSlot, Op::DepthFunc, function);
		driver->DepthFunc(function);
	}
	void cGDriverWire::DepthMask(bool enabled) {
		RecordState(kDepthMaskSlot, Op::DepthMask, enabled);
		driver->DepthMask(enabled);
	}
	void cGDriverWire::StencilFunc(uint32_t function, int32_t reference, uint32_t mask) {
		Record(Op::StencilFunc, function, reference, mask);
		driver->StencilFunc(function, reference, mask);
	}
	void cGDriverWire::StencilMask(uint32_t mask) {
		Record(Op::StencilMask, mask);
		driver->StencilMask(mask);
	}
	void cGDriverWire::StencilOp(uint32_t fail, uint32_t depthFail, uint32_t pass) {
		Record(Op::StencilOp, fail, depthFail, pass);
		driver->StencilOp(fail, depthFail, pass);
	}
	void cGDriverWire::BlendFunc(uint32_t source, uint32_t destination) {
		RecordState(kBlendFuncSlot, Op::BlendFunc, source, destination);
		driver->BlendFunc(source, destination);
	}
	void cGDriverWire::AlphaFunc(uint32_t function, float reference) {
		RecordState(kAlphaFuncSlot, Op::AlphaFunc, function, reference);
		driver->AlphaFunc(function, reference);
	}
	void cGDriverWire::ShadeModel(uint32_t mode) {
		RecordState(kShadeModelSlot, Op::ShadeModel, mode);
		driver->ShadeModel(mode);
	}

	// Textures.

	void cGDriverWire::BindTexture(uint32_t target, uint32_t texture) {
		Record(Op::BindTexture, target, texture);
		Forget(kTextureSlot, 2);
		driver->BindTexture(target, texture);
	}

	void cGDriverWire::TexImage2D(uint32_t target, int32_t level, int32_t internalFormat, int32_t width,
	                              int32_t height, int32_t border, uint32_t format, uint32_t type,
	                              void const *pixels) {
		writer.Begin(Op::TexImage2D);
		writer.Put(target);
		writer.Put(level);
		writer.Put(internalFormat);
		writer.Put(width);
		writer.Put(height);
		writer.Put(border);
		writer.Put(format);
		writer.Put(type);
		writer.Put(pixels != nullptr);
		writer.PutBlob(pixels,
		               pixels != nullptr ? SourceImageBytes(format, type, width, height, unpackRowLength) : 0);
		writer.End();
		driver->TexImage2D(target, level, internalFormat, width, height, border, format, type, pixels);
	}

	void cGDriverWire::PixelStore(uint32_t parameter, int32_t value) {
		// Parameter 0 is the unpack row length (the driver's pixelStoreRowLength).
		if (parameter == 0 && value >= 0) unpackRowLength = value;
		Record(Op::PixelStore, parameter, value);
		driver->PixelStore(parameter, value);
	}

	void cGDriverWire::TexEnv(uint32_t target, uint32_t parameter, int32_t value) {
		if (target == 0 && parameter == 0) {
			RecordState(kTexEnvSlot + activeStage, Op::TexEnvInt, target, parameter, value);
		} else {
			Record(Op::TexEnvInt, target, parameter, value);
		}
		driver->TexEnv(target, parameter, value);
	}
	void cGDriverWire::TexEnv(uint32_t target, uint32_t parameter, float const *value) {
		Record(Op::TexEnvFloat, target, parameter, value != nullptr, Copy4(value));
		driver->TexEnv(target, parameter, value);
	}
	void cGDriverWire::TexParameter(uint32_t target, uint32_t parameter, int32_t value) {
		if (target == 0 && parameter < 4) {
			RecordState(kTexParameterSlot + activeStage * 4 + parameter, Op::TexParameter, target, parameter, value);
		} else {
			Record(Op::TexParameter, target, parameter, value);
		}
		driver->TexParameter(target, parameter, value);
	}
	void cGDriverWire::Fog(uint32_t parameter, uint32_t value) {
		Record(Op::FogInt, parameter, value);
		driver->Fog(parameter, value);
	}
	void cGDriverWire::Fog(uint32_t parameter, float const *value) {
		// Parameter 1 is the fog colour. The others are a single value.
		Record(Op::FogFloat, parameter, value != nullptr, Copy4(value, parameter == 1 ? 4 : 1));
		driver->Fog(parameter, value);
	}
	void cGDriverWire::ColorMultiplier(float r, float g, float b) {
		RecordState(kColorMultiplierSlot, Op::ColorMultiplier, r, g, b);
		driver->ColorMultiplier(r, g, b);
	}
	void cGDriverWire::AlphaMultiplier(float a) {
		RecordState(kAlphaMultiplierSlot, Op::AlphaMultiplier, a);
		// Both decide whether diffuse comes from the vertices.
		Forget(kVertexColorsSlot);
		driver->AlphaMultiplier(a);
	}
	void cGDriverWire::EnableVertexColors(bool ambient, bool diffuse) {
		RecordState(kVertexColorsSlot, Op::EnableVertexColors, ambient, diffuse);
		Forget(kAlphaMultiplierSlot);
		driver->EnableVertexColors(ambient, diffuse);
	}

	void cGDriverWire::GenTextures(int32_t count, uint32_t *names) {
		driver->GenTextures(count, names);
		if (count <= 0 || names == nullptr) return;
		// A name SetTexture ignored before may be valid now.
		Forget(kTextureSlot, 2);
		writer.Begin(Op::GenTextures);
		writer.Put(count);
		writer.PutBytes(names, static_cast<size_t>(count) * sizeof(uint32_t));
		writer.End();
	}

	void cGDriverWire::DeleteTextures(int32_t count, uint32_t const *names) {
		if (count > 0 && names != nullptr) {
			Forget(kTextureSlot, 2);
			writer.Begin(Op::DeleteTextures);
			writer.Put(count);
			writer.PutBytes(names, static_cast<size_t>(count) * sizeof(uint32_t));
			writer.End();
		}
		driver->DeleteTextures(count, names);
	}

	bool cGDriverWire::IsTexture(uint32_t name) { return driver->IsTexture(name); }
	void cGDriverWire::PrioritizeTextures(int32_t count, uint32_t const *names, float const *priorities) {
		driver->PrioritizeTextures(count, names, priorities);
	}
	bool cGDriverWire::AreTexturesResident(int32_t count, uint32_t const *names, bool *resident) {
		return driver->AreTexturesResident(count, names, resident);
	}

	// Matrices and switches.

	void cGDriverWire::MatrixMode(uint32_t mode) {
		RecordState(kMatrixModeSlot, Op::MatrixMode, mode);
		if (mode < 2) matrixMode = mode;
		driver->MatrixMode(mode);
	}
	void cGDriverWire::LoadMatrix(float const *matrix) {
		if (matrix != nullptr) {
			Matrix values;
			std::memcpy(values.v, matrix, sizeof(values.v));
			RecordState(kMatrixSlot + matrixMode, Op::LoadMatrix, values);
		}
		driver->LoadMatrix(matrix);
	}
	void cGDriverWire::LoadIdentity(void) {
		RecordState(kMatrixSlot + matrixMode, Op::LoadIdentity);
		driver->LoadIdentity();
	}

	void cGDriverWire::Flush(void) {
		Record(Op::Frame);
		writer.Flush();
		driver->Flush();
		// The driver resizes its back buffer inside Flush when the window changed size, so the renderer on
		// the other end hears about it between this frame and the next.
		int32_t dimensions[2]{};
		driver->BackBufferSize(dimensions[0], dimensions[1]);
		if (dimensions[0] > 0 && dimensions[1] > 0 && (dimensions[0] != recordedWidth || dimensions[1] != recordedHeight)) {
			if (recordedWidth != 0) {
				Record(Op::Resize, static_cast<uint32_t>(dimensions[0]), static_cast<uint32_t>(dimensions[1]));
				writer.Flush();
			}
			recordedWidth = dimensions[0];
			recordedHeight = dimensions[1];
		}
	}

	void cGDriverWire::Enable(uint32_t state) {
		RecordSwitch(Op::Enable, state);
		driver->Enable(state);
	}
	void cGDriverWire::Disable(uint32_t state) {
		RecordSwitch(Op::Disable, state);
		driver->Disable(state);
	}
	bool cGDriverWire::IsEnabled(uint32_t state) { return driver->IsEnabled(state); }
	void cGDriverWire::GetBoolean(uint32_t parameter, bool *value) { driver->GetBoolean(parameter, value); }
	void cGDriverWire::GetInteger(uint32_t parameter, int32_t *value) { driver->GetInteger(parameter, value); }
	void cGDriverWire::GetFloat(uint32_t parameter, float *value) { driver->GetFloat(parameter, value); }
	uint32_t cGDriverWire::GetError(void) { return driver->GetError(); }

	// Texture stages.

	void cGDriverWire::TexStage(uint32_t stage) {
		RecordState(kTexStageSlot, Op::TexStage, stage);
		if (stage < 2) activeStage = stage;
		driver->TexStage(stage);
	}
	void cGDriverWire::TexStageCoord(uint32_t source) {
		RecordState(kTexStageCoordSlot + activeStage, Op::TexStageCoord, source);
		driver->TexStageCoord(source);
	}
	void cGDriverWire::TexStageMatrix(float const *matrix, uint32_t rows, uint32_t columns, uint32_t flags) {
		Matrix values{};
		if (matrix != nullptr) std::memcpy(values.v, matrix, sizeof(values.v));
		if (Unchanged(kTexStageMatrixSlot + activeStage, Op::TexStageMatrix, matrix != nullptr, rows, columns, flags,
		              values)) {
			driver->TexStageMatrix(matrix, rows, columns, flags);
			return;
		}
		writer.Begin(Op::TexStageMatrix);
		writer.Put(matrix != nullptr);
		writer.Put(rows);
		writer.Put(columns);
		writer.Put(flags);
		if (matrix != nullptr) writer.PutBytes(matrix, 16 * sizeof(float));
		writer.End();
		driver->TexStageMatrix(matrix, rows, columns, flags);
	}
	void cGDriverWire::TexStageCombine(eGDTextureStageCombineScaleParamType parameter,
	                                   eGDTextureStageCombineScaleParam value) {
		Record(Op::TexStageCombineScale, static_cast<uint32_t>(parameter), static_cast<uint32_t>(value));
		driver->TexStageCombine(parameter, value);
	}
	void cGDriverWire::TexStageCombine(eGDTextureStageCombineOperandType parameter, eGDBlend value) {
		Record(Op::TexStageCombineOperand, static_cast<uint32_t>(parameter), static_cast<uint32_t>(value));
		driver->TexStageCombine(parameter, value);
	}
	void cGDriverWire::TexStageCombine(eGDTextureStageCombineSourceParamType parameter,
	                                   eGDTextureStageCombineSourceParam value) {
		Record(Op::TexStageCombineSource, static_cast<uint32_t>(parameter), static_cast<uint32_t>(value));
		driver->TexStageCombine(parameter, value);
	}
	void cGDriverWire::TexStageCombine(eGDTextureStageCombineParamType parameter,
	                                   eGDTextureStageCombineModeParam value) {
		Record(Op::TexStageCombineMode, static_cast<uint32_t>(parameter), static_cast<uint32_t>(value));
		driver->TexStageCombine(parameter, value);
	}

	void cGDriverWire::SetTexture(uint32_t texture, uint32_t stage) {
		if (stage < 2) RecordState(kTextureSlot + stage, Op::SetTexture, texture, stage);
		else Record(Op::SetTexture, texture, stage);
		driver->SetTexture(texture, stage);
	}
	intptr_t cGDriverWire::GetTexture(uint32_t stage) { return driver->GetTexture(stage); }

	intptr_t cGDriverWire::CreateTexture(uint32_t internalFormat, uint32_t width, uint32_t height,
	                                     uint32_t levels, uint32_t hints) {
		intptr_t const name = driver->CreateTexture(internalFormat, width, height, levels, hints);
		Record(Op::CreateTexture, internalFormat, width, height, levels, hints, static_cast<uint32_t>(name));
		Forget(kTextureSlot, 2);
		return name;
	}

	void cGDriverWire::LoadTextureLevel(uint32_t texture, int32_t level, int32_t x, int32_t y, int32_t width,
	                                    int32_t height, uint32_t format, uint32_t type, uint32_t rowLength,
	                                    void const *pixels) {
		writer.Begin(Op::LoadTextureLevel);
		writer.Put(texture);
		writer.Put(level);
		writer.Put(x);
		writer.Put(y);
		writer.Put(width);
		writer.Put(height);
		writer.Put(format);
		writer.Put(type);
		writer.Put(rowLength);
		writer.Put(pixels != nullptr);
		writer.PutBlob(pixels, pixels != nullptr
			                       ? SourceImageBytes(format, type, width, height, static_cast<int32_t>(rowLength))
			                       : 0);
		writer.End();
		driver->LoadTextureLevel(texture, level, x, y, width, height, format, type, rowLength, pixels);
	}

	void cGDriverWire::SetCombiner(cGDCombiner const &combiner, uint32_t stage) {
		static_assert(sizeof(cGDCombiner) == 10);
		if (stage < 2) Forget(kTexEnvSlot + stage);
		writer.Begin(Op::SetCombiner);
		writer.PutBytes(&combiner, sizeof(combiner));
		writer.Put(stage);
		writer.End();
		driver->SetCombiner(combiner, stage);
	}

	// Video modes and the window.

	uint32_t cGDriverWire::CountVideoModes(void) const { return driver->CountVideoModes(); }
	void cGDriverWire::GetVideoModeInfo(uint32_t index, sGDMode &mode) { driver->GetVideoModeInfo(index, mode); }
	void cGDriverWire::GetVideoModeInfo(sGDMode &mode) { driver->GetVideoModeInfo(mode); }

	void cGDriverWire::SetVideoMode(int32_t index, void *windowProcedure, bool show, bool fourth) {
		driver->SetVideoMode(index, windowProcedure, show, fourth);
		sGDMode mode{};
		if (index >= 0 && static_cast<uint32_t>(index) < driver->CountVideoModes()) {
			driver->GetVideoModeInfo(static_cast<uint32_t>(index), mode);
		}
		Record(Op::SetVideoMode, index, show, fourth, mode.width, mode.height, mode.isFullscreen);
		recordedWidth = static_cast<int32_t>(mode.width);
		recordedHeight = static_cast<int32_t>(mode.height);
		Forget(0, kSlotCount);
	}

	void cGDriverWire::PolygonOffset(int32_t offset) {
		RecordState(kPolygonOffsetSlot, Op::PolygonOffset, offset);
		driver->PolygonOffset(offset);
	}

	void cGDriverWire::RecordBlit(BlitKind kind, int32_t x, int32_t y, int32_t width, int32_t height,
	                              int32_t sourceWidth, int32_t sourceHeight, uint32_t format, uint32_t type,
	                              void const *pixels, bool colorKeyed, void const *colorKey, uint32_t alpha) {
		uint32_t const pixelBytes = TextureSourcePixelBytes(format, type);
		uint32_t key = 0;
		if (colorKeyed && colorKey != nullptr && pixelBytes <= 4) std::memcpy(&key, colorKey, pixelBytes);
		// The driver reads rows the unpack row length apart, as it does for texture uploads.
		uint32_t const bytes = pixels != nullptr && pixelBytes <= 4
			                       ? SourceImageBytes(format, type, sourceWidth, sourceHeight, unpackRowLength)
			                       : 0;
		writer.Begin(Op::Blit);
		writer.Put(kind);
		writer.Put(x);
		writer.Put(y);
		writer.Put(width);
		writer.Put(height);
		writer.Put(sourceWidth);
		writer.Put(sourceHeight);
		writer.Put(format);
		writer.Put(type);
		writer.Put(colorKeyed && colorKey != nullptr);
		writer.Put(key);
		writer.Put(alpha);
		writer.PutBlob(pixels, bytes);
		writer.End();
	}

	void cGDriverWire::BitBlt(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t format, uint32_t type,
	                          void const *pixels, bool colorKeyed, void const *colorKey) {
		RecordBlit(BlitKind::BitBlt, x, y, width, height, width, height, format, type, pixels, colorKeyed, colorKey,
		           0);
		driver->BitBlt(x, y, width, height, format, type, pixels, colorKeyed, colorKey);
	}
	void cGDriverWire::StretchBlt(int32_t x, int32_t y, int32_t width, int32_t height, int32_t sourceWidth,
	                              int32_t sourceHeight, uint32_t format, uint32_t type, void const *pixels,
	                              bool colorKeyed, void const *colorKey) {
		RecordBlit(BlitKind::StretchBlt, x, y, width, height, sourceWidth, sourceHeight, format, type, pixels,
		           colorKeyed, colorKey, 0);
		driver->StretchBlt(x, y, width, height, sourceWidth, sourceHeight, format, type, pixels, colorKeyed,
		                   colorKey);
	}
	void cGDriverWire::BitBltAlpha(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t format,
	                               uint32_t type, void const *pixels, bool colorKeyed, void const *colorKey,
	                               uint32_t alpha) {
		RecordBlit(BlitKind::BitBltAlpha, x, y, width, height, width, height, format, type, pixels, colorKeyed,
		           colorKey, alpha);
		driver->BitBltAlpha(x, y, width, height, format, type, pixels, colorKeyed, colorKey, alpha);
	}
	void cGDriverWire::StretchBltAlpha(int32_t x, int32_t y, int32_t width, int32_t height, int32_t sourceWidth,
	                                   int32_t sourceHeight, uint32_t format, uint32_t type, void const *pixels,
	                                   bool colorKeyed, void const *colorKey, uint32_t alpha) {
		RecordBlit(BlitKind::StretchBltAlpha, x, y, width, height, sourceWidth, sourceHeight, format, type, pixels,
		           colorKeyed, colorKey, alpha);
		driver->StretchBltAlpha(x, y, width, height, sourceWidth, sourceHeight, format, type, pixels, colorKeyed,
		                        colorKey, alpha);
	}
	void cGDriverWire::BitBltAlphaModulate(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t format,
	                                       uint32_t type, void const *pixels, bool colorKeyed,
	                                       void const *colorKey, uint32_t alpha) {
		RecordBlit(BlitKind::BitBltAlphaModulate, x, y, width, height, width, height, format, type, pixels,
		           colorKeyed, colorKey, alpha);
		driver->BitBltAlphaModulate(x, y, width, height, format, type, pixels, colorKeyed, colorKey, alpha);
	}
	void cGDriverWire::StretchBltAlphaModulate(int32_t x, int32_t y, int32_t width, int32_t height,
	                                           int32_t sourceWidth, int32_t sourceHeight, uint32_t format,
	                                           uint32_t type, void const *pixels, bool colorKeyed,
	                                           void const *colorKey, uint32_t alpha) {
		RecordBlit(BlitKind::StretchBltAlphaModulate, x, y, width, height, sourceWidth, sourceHeight, format, type,
		           pixels, colorKeyed, colorKey, alpha);
		driver->StretchBltAlphaModulate(x, y, width, height, sourceWidth, sourceHeight, format, type, pixels,
		                                colorKeyed, colorKey, alpha);
	}

	void cGDriverWire::SetViewport(int32_t x, int32_t y, int32_t width, int32_t height) {
		Record(Op::SetViewport, x, y, width, height);
		driver->SetViewport(x, y, width, height);
	}
	void cGDriverWire::SetViewport(void) {
		Record(Op::SetViewportFull);
		driver->SetViewport();
	}
	void cGDriverWire::GetViewport(int32_t dimensions[4]) { driver->GetViewport(dimensions); }
	char const *cGDriverWire::GetDriverInfo(void) const { return driver->GetDriverInfo(); }
	uint32_t cGDriverWire::GetGZCLSID(void) const { return driver->GetGZCLSID(); }

	bool cGDriverWire::Init(void) {
		Record(Op::Init);
		Forget(0, kSlotCount);
		return driver->Init();
	}
	bool cGDriverWire::Shutdown(void) {
		Record(Op::Shutdown);
		Forget(0, kSlotCount);
		writer.Flush();
		return driver->Shutdown();
	}
	bool cGDriverWire::IsDeviceReady(void) { return driver->IsDeviceReady(); }

	bool cGDriverWire::Punt(uint32_t id, void *data) {
		if (id == kPuntBlitSourceAlpha && data != nullptr) Record(Op::PuntBlitSourceAlpha, *static_cast<bool *>(data));
		return driver->Punt(id, data);
	}

	// Vertex-buffer extension. The game writes into the driver's own memory between GetVertices and
	// ReleaseVertices, and the draws that follow read from it.

	char const *cGDriverWire::GetVertexBufferName(uint32_t format) { return driver->GetVertexBufferName(format); }
	uint32_t cGDriverWire::VertexBufferType(uint32_t name) { return driver->VertexBufferType(name); }
	uint32_t cGDriverWire::MaxVertices(uint32_t name) { return driver->MaxVertices(name); }

	uint32_t cGDriverWire::GetVertices(int32_t name, uint32_t count) {
		uint32_t const pointer = driver->GetVertices(name, count);
		if (pointer != 0) {
			reservation = reinterpret_cast<uint8_t const *>(static_cast<uintptr_t>(pointer));
			reservationVertices = count;
			Record(Op::GetVertices, name, count);
		}
		return pointer;
	}

	uint32_t cGDriverWire::ContinueVertices(uint32_t name, uint32_t count) {
		uint32_t const pointer = driver->ContinueVertices(name, count);
		if (pointer != 0) {
			reservationVertices += count;
			Record(Op::ContinueVertices, name, count);
		}
		return pointer;
	}

	void cGDriverWire::ReleaseVertices(uint32_t name) {
		Record(Op::ReleaseVertices, name);
		driver->ReleaseVertices(name);
	}

	// The game reserves far more than it fills, so each draw carries only the vertices it reads.
	void cGDriverWire::RecordReservation(uint32_t name, uint32_t bytes) {
		uint32_t const stride = RZVertexFormatStride(driver->VertexBufferType(name));
		uint32_t const reserved = reservation != nullptr ? reservationVertices * stride : 0;
		writer.PutBlob(reservation, (std::min)(bytes, reserved));
	}

	void cGDriverWire::DrawPrims(uint32_t name, uint32_t primitive, void *data, uint32_t byteSize) {
		writer.Begin(Op::DrawPrims);
		writer.Put(name);
		writer.Put(primitive);
		writer.Put(byteSize);
		RecordReservation(name, byteSize);
		writer.End();
		driver->DrawPrims(name, primitive, data, byteSize);
	}

	void cGDriverWire::DrawPrimsIndexed(uint32_t name, uint32_t primitive, uint32_t count, uint16_t *indices) {
		uint32_t highest = 0;
		for (uint32_t i = 0; indices != nullptr && i < count; ++i) highest = (std::max)(highest, uint32_t{indices[i]});
		uint32_t const stride = RZVertexFormatStride(driver->VertexBufferType(name));
		writer.Begin(Op::DrawPrimsIndexed);
		writer.Put(name);
		writer.Put(primitive);
		writer.Put(count);
		writer.PutBlob(indices, indices != nullptr ? count * static_cast<uint32_t>(sizeof(uint16_t)) : 0);
		RecordReservation(name, (highest + 1) * stride);
		writer.End();
		driver->DrawPrimsIndexed(name, primitive, count, indices);
	}

	void cGDriverWire::Reset(void) {
		Record(Op::ResetVertexBuffers);
		driver->Reset();
	}

	// Buffer regions. Region numbers are recorded as the driver returned them.

	bool cGDriverWire::BufferRegionEnabled(void) { return driver->BufferRegionEnabled(); }
	uint32_t cGDriverWire::NewBufferRegion(int32_t type) {
		uint32_t const region = driver->NewBufferRegion(type);
		Record(Op::NewBufferRegion, type, region);
		return region;
	}
	bool cGDriverWire::DeleteBufferRegion(int32_t region) {
		Record(Op::DeleteBufferRegion, region);
		return driver->DeleteBufferRegion(region);
	}
	bool cGDriverWire::ReadBufferRegion(uint32_t region, int32_t a, int32_t b, int32_t c, int32_t d, int32_t e,
	                                    int32_t f) {
		Record(Op::ReadBufferRegion, region, a, b, c, d, e, f);
		return driver->ReadBufferRegion(region, a, b, c, d, e, f);
	}
	bool cGDriverWire::DrawBufferRegion(uint32_t region, int32_t a, int32_t b, int32_t c, int32_t d, int32_t e,
	                                    int32_t f) {
		Record(Op::DrawBufferRegion, region, a, b, c, d, e, f);
		return driver->DrawBufferRegion(region, a, b, c, d, e, f);
	}
	bool cGDriverWire::IsBufferRegion(uint32_t region) { return driver->IsBufferRegion(region); }
	bool cGDriverWire::CanDoPartialRegionWrites(void) { return driver->CanDoPartialRegionWrites(); }
	bool cGDriverWire::CanDoOffsetReads(void) { return driver->CanDoOffsetReads(); }
	bool cGDriverWire::DeleteAllBufferRegions(void) {
		Record(Op::DeleteAllBufferRegions);
		return driver->DeleteAllBufferRegions();
	}

	cIGZBuffer *cGDriverWire::CopyColorBuffer(int32_t x, int32_t y, int32_t width, int32_t height,
	                                          cIGZBuffer *buffer) {
		Record(Op::CopyColorBuffer, x, y, width, height);
		DrawOwnerHooks::DrawAllModels();
		return driver->CopyColorBuffer(x, y, width, height, buffer);
	}

	// Lighting extension.

	void cGDriverWire::EnableLighting(bool enabled) {
		Record(Op::EnableLighting, enabled);
		ForgetMultipliers();
		driver->EnableLighting(enabled);
	}
	void cGDriverWire::EnableLight(uint32_t light, bool enabled) {
		Record(Op::EnableLight, light, enabled);
		ForgetMultipliers();
		driver->EnableLight(light, enabled);
	}
	void cGDriverWire::LightModelAmbient(float r, float g, float b, float a) {
		Record(Op::LightModelAmbient, r, g, b, a);
		ForgetMultipliers();
		driver->LightModelAmbient(r, g, b, a);
	}
	void cGDriverWire::LightColor(uint32_t light, uint32_t parameter, float const *color) {
		Record(Op::LightColor, light, parameter, color != nullptr, Copy4(color));
		ForgetMultipliers();
		driver->LightColor(light, parameter, color);
	}
	void cGDriverWire::LightColor(uint32_t light, float const *ambient, float const *diffuse,
	                              float const *specular) {
		Record(Op::LightColors, light, ambient != nullptr, Copy4(ambient), diffuse != nullptr, Copy4(diffuse),
		       specular != nullptr, Copy4(specular));
		ForgetMultipliers();
		driver->LightColor(light, ambient, diffuse, specular);
	}
	void cGDriverWire::LightPosition(uint32_t light, float const *position) {
		Record(Op::LightPosition, light, position != nullptr, Copy4(position));
		ForgetMultipliers();
		driver->LightPosition(light, position);
	}
	void cGDriverWire::LightDirection(uint32_t light, float const *direction) {
		Record(Op::LightDirection, light, direction != nullptr, Copy4(direction, 3));
		ForgetMultipliers();
		driver->LightDirection(light, direction);
	}
	void cGDriverWire::MaterialColor(uint32_t parameter, float const *color) {
		// Parameter 4 is the shininess, a single value.
		Record(Op::MaterialColor, parameter, color != nullptr, Copy4(color, parameter == 4 ? 1 : 4));
		ForgetMultipliers();
		driver->MaterialColor(parameter, color);
	}
	void cGDriverWire::MaterialColor(float const *ambient, float const *diffuse, float const *specular,
	                                 float const *emission, float shininess) {
		Record(Op::MaterialColors, ambient != nullptr, Copy4(ambient), diffuse != nullptr, Copy4(diffuse),
		       specular != nullptr, Copy4(specular), emission != nullptr, Copy4(emission), shininess);
		ForgetMultipliers();
		driver->MaterialColor(ambient, diffuse, specular, emission, shininess);
	}
}

namespace nSCD3D11 {
	bool CreateDriver(uint32_t riid, void **ppvObj) {
		Settings::Values const &settings = Settings::Get();
		std::unique_ptr<RemoteRenderer> remote;
		if (settings.server) {
			remote = std::make_unique<RemoteRenderer>();
			if (!remote->Start()) remote.reset();
		}
		std::shared_ptr<std::FILE> output;
		if (!settings.record.empty()) {
			std::FILE *file = nullptr;
			if (fopen_s(&file, settings.record.c_str(), "wb") == 0 && file != nullptr) {
				output.reset(file, std::fclose);
				Log(LogCategory::Initialization, "record: writing every SimGL call to %s", settings.record.c_str());
			} else {
				Log(LogCategory::Initialization, "record: cannot open %s, not recording", settings.record.c_str());
			}
		}
		if (remote == nullptr && output == nullptr) return cGDriver::FactoryFunctionPtr2(riid, ppvObj);

		auto *const driver = new cGDriver();
		nSCDWire::Writer::Sink const toServer = remote != nullptr ? remote->Sink() : nSCDWire::Writer::Sink{};
		if (remote != nullptr) driver->SetRemote(remote.get());
		auto *const wire = new cGDriverWire(driver, [toServer, output](void const *data, size_t size) {
			bool ok = true;
			if (output != nullptr) ok = std::fwrite(data, 1, size, output.get()) == size && std::fflush(output.get()) == 0;
			if (toServer) ok = toServer(data, size) && ok;
			return ok;
		});
		wire->remote = std::move(remote);
		if (!wire->QueryInterface(riid, ppvObj)) {
			delete wire;
			return false;
		}
		return true;
	}

	void cGDriverReplayHooks::VideoMode(bool show, uint32_t width, uint32_t height, bool fullscreen) {
		int32_t match = -1;
		sGDMode mode{};
		for (uint32_t index = 0; index < driver.CountVideoModes(); ++index) {
			driver.GetVideoModeInfo(index, mode);
			if (mode.width == width && mode.height == height && !mode.isFullscreen) {
				match = static_cast<int32_t>(index);
				break;
			}
		}
		if (match < 0) {
			Log(LogCategory::Initialization, "replay: no windowed %ux%u mode%s", width, height,
			    fullscreen ? " (recorded fullscreen)" : "");
			return;
		}
		driver.SetVideoMode(match, nullptr, show, false);
	}

	void cGDriverReplayHooks::Vertices(uint32_t name, void const *data, uint32_t bytes) {
		uint32_t const stride = RZVertexFormatStride(vertexBuffers.VertexBufferType(name));
		if (stride == 0 || bytes == 0 || bytes % stride != 0) return;
		uint32_t const pointer = vertexBuffers.GetVertices(static_cast<int32_t>(name), bytes / stride);
		if (pointer != 0) std::memcpy(reinterpret_cast<void *>(static_cast<uintptr_t>(pointer)), data, bytes);
		vertexBuffers.ReleaseVertices(name);
	}
}
