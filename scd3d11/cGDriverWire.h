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

#include "cGDriver.h"
#include "wire/WireReplay.h"
#include "wire/WireStream.h"

#include <cRZBaseUnknown.h>

namespace nSCD3D11 {
	// Stands in front of cGDriver: every call goes to the driver, and the calls a renderer elsewhere would
	// have to repeat are also written to an SCDW stream (wire/Wire.h). SC4 sees this object as its driver.
	//
	// Data the game owns (vertices, indices, texels) is copied into the stream when it is used, because the
	// game may change or free it right after the call returns.
	class cGDriverWire final :
			public cIGZGDriver,
			public cIGZGBufferRegionExtension,
			public cIGZGDriverLightingExtension,
			public cIGZGDriverVertexBufferExtension,
			public cIGZGSnapshotExtension,
			public cRZBaseUnknown {
	public:
		// Takes a reference on driver.
		cGDriverWire(cGDriver *driver, nSCDWire::Writer::Sink sink);
		~cGDriverWire();

		cGDriver &Inner() { return *driver; }
		// The driver that writes a stream this session, if any. Render thread only, which in SC4 is also the
		// thread that pumps messages.
		static cGDriverWire *Current();
		void RecordOccupant(nSCDWire::OccupantEvent const &event);
		void RecordOwnerBegin(nSCDWire::DrawOwner const &owner) { Record(nSCDWire::Op::OwnerBegin, owner); }
		void RecordOwnerEnd() { Record(nSCDWire::Op::OwnerEnd); }
		void RecordModelInstance(nSCDWire::ModelInstance const &model) { Record(nSCDWire::Op::ModelInstance, model); }
		nSCDWire::Writer &Stream() { return writer; }

		// cIGZUnknown
		bool QueryInterface(uint32_t riid, void **ppvObj) override;
		uint32_t AddRef(void) override;
		uint32_t Release(void) override;
		bool FinalRelease(void) override;

		// cIGZGDriver
		void DrawArrays(uint32_t primitive, int32_t first, int32_t count) override;
		void DrawElements(uint32_t primitive, int32_t count, uint32_t type, void const *indices) override;
		void InterleavedArrays(uint32_t format, int32_t stride, void const *pointer) override;
		uint32_t MakeVertexFormat(uint32_t count, intptr_t elements) override;
		uint32_t MakeVertexFormat(uint32_t format) override;
		uint32_t VertexFormatStride(uint32_t format) override;
		uint32_t VertexFormatElementOffset(uint32_t format, uint32_t element, uint32_t index) override;
		uint32_t VertexFormatNumElements(uint32_t format, uint32_t element) override;
		void Clear(uint32_t mask) override;
		void ClearColor(float r, float g, float b, float a) override;
		void ClearDepth(double depth) override;
		void ClearStencil(int32_t stencil) override;
		void ColorMask(bool enabled) override;
		void DepthFunc(uint32_t function) override;
		void DepthMask(bool enabled) override;
		void StencilFunc(uint32_t function, int32_t reference, uint32_t mask) override;
		void StencilMask(uint32_t mask) override;
		void StencilOp(uint32_t fail, uint32_t depthFail, uint32_t pass) override;
		void BlendFunc(uint32_t source, uint32_t destination) override;
		void AlphaFunc(uint32_t function, float reference) override;
		void ShadeModel(uint32_t mode) override;
		void BindTexture(uint32_t target, uint32_t texture) override;
		void TexImage2D(uint32_t target, int32_t level, int32_t internalFormat, int32_t width, int32_t height,
		                int32_t border, uint32_t format, uint32_t type, void const *pixels) override;
		void PixelStore(uint32_t parameter, int32_t value) override;
		void TexEnv(uint32_t target, uint32_t parameter, int32_t value) override;
		void TexEnv(uint32_t target, uint32_t parameter, float const *value) override;
		void TexParameter(uint32_t target, uint32_t parameter, int32_t value) override;
		void Fog(uint32_t parameter, uint32_t value) override;
		void Fog(uint32_t parameter, float const *value) override;
		void ColorMultiplier(float r, float g, float b) override;
		void AlphaMultiplier(float a) override;
		void EnableVertexColors(bool ambient, bool diffuse) override;
		void GenTextures(int32_t count, uint32_t *names) override;
		void DeleteTextures(int32_t count, uint32_t const *names) override;
		bool IsTexture(uint32_t name) override;
		void PrioritizeTextures(int32_t count, uint32_t const *names, float const *priorities) override;
		bool AreTexturesResident(int32_t count, uint32_t const *names, bool *resident) override;
		void MatrixMode(uint32_t mode) override;
		void LoadMatrix(float const *matrix) override;
		void LoadIdentity(void) override;
		void Flush(void) override;
		void Enable(uint32_t state) override;
		void Disable(uint32_t state) override;
		bool IsEnabled(uint32_t state) override;
		void GetBoolean(uint32_t parameter, bool *value) override;
		void GetInteger(uint32_t parameter, int32_t *value) override;
		void GetFloat(uint32_t parameter, float *value) override;
		uint32_t GetError(void) override;
		void TexStage(uint32_t stage) override;
		void TexStageCoord(uint32_t source) override;
		void TexStageMatrix(float const *matrix, uint32_t rows, uint32_t columns, uint32_t flags) override;
		void TexStageCombine(eGDTextureStageCombineScaleParamType parameter,
		                     eGDTextureStageCombineScaleParam value) override;
		void TexStageCombine(eGDTextureStageCombineOperandType parameter, eGDBlend value) override;
		void TexStageCombine(eGDTextureStageCombineSourceParamType parameter,
		                     eGDTextureStageCombineSourceParam value) override;
		void TexStageCombine(eGDTextureStageCombineParamType parameter,
		                     eGDTextureStageCombineModeParam value) override;
		void SetTexture(uint32_t texture, uint32_t stage) override;
		intptr_t GetTexture(uint32_t stage) override;
		intptr_t CreateTexture(uint32_t internalFormat, uint32_t width, uint32_t height, uint32_t levels,
		                       uint32_t hints) override;
		void LoadTextureLevel(uint32_t texture, int32_t level, int32_t x, int32_t y, int32_t width, int32_t height,
		                      uint32_t format, uint32_t type, uint32_t rowLength, void const *pixels) override;
		void SetCombiner(cGDCombiner const &combiner, uint32_t stage) override;
		uint32_t CountVideoModes(void) const override;
		void GetVideoModeInfo(uint32_t index, sGDMode &mode) override;
		void GetVideoModeInfo(sGDMode &mode) override;
		void SetVideoMode(int32_t index, void *windowProcedure, bool show, bool fourth) override;
		void PolygonOffset(int32_t offset) override;
		void BitBlt(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t format, uint32_t type,
		            void const *pixels, bool colorKeyed, void const *colorKey) override;
		void StretchBlt(int32_t x, int32_t y, int32_t width, int32_t height, int32_t sourceWidth,
		                int32_t sourceHeight, uint32_t format, uint32_t type, void const *pixels, bool colorKeyed,
		                void const *colorKey) override;
		void BitBltAlpha(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t format, uint32_t type,
		                 void const *pixels, bool colorKeyed, void const *colorKey, uint32_t alpha) override;
		void StretchBltAlpha(int32_t x, int32_t y, int32_t width, int32_t height, int32_t sourceWidth,
		                     int32_t sourceHeight, uint32_t format, uint32_t type, void const *pixels,
		                     bool colorKeyed, void const *colorKey, uint32_t alpha) override;
		void BitBltAlphaModulate(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t format,
		                         uint32_t type, void const *pixels, bool colorKeyed, void const *colorKey,
		                         uint32_t alpha) override;
		void StretchBltAlphaModulate(int32_t x, int32_t y, int32_t width, int32_t height, int32_t sourceWidth,
		                             int32_t sourceHeight, uint32_t format, uint32_t type, void const *pixels,
		                             bool colorKeyed, void const *colorKey, uint32_t alpha) override;
		void SetViewport(int32_t x, int32_t y, int32_t width, int32_t height) override;
		void SetViewport(void) override;
		void GetViewport(int32_t dimensions[4]) override;
		char const *GetDriverInfo(void) const override;
		uint32_t GetGZCLSID(void) const override;
		bool Init(void) override;
		bool Shutdown(void) override;
		bool IsDeviceReady(void) override;
		bool Punt(uint32_t id, void *data) override;

		// cIGZGDriverVertexBufferExtension
		char const *GetVertexBufferName(uint32_t format) override;
		uint32_t VertexBufferType(uint32_t name) override;
		uint32_t MaxVertices(uint32_t name) override;
		uint32_t GetVertices(int32_t name, uint32_t count) override;
		uint32_t ContinueVertices(uint32_t name, uint32_t count) override;
		void ReleaseVertices(uint32_t name) override;
		void DrawPrims(uint32_t name, uint32_t primitive, void *data, uint32_t byteSize) override;
		void DrawPrimsIndexed(uint32_t name, uint32_t primitive, uint32_t count, uint16_t *indices) override;
		void Reset(void) override;

		// cIGZGBufferRegionExtension
		bool BufferRegionEnabled(void) override;
		uint32_t NewBufferRegion(int32_t type) override;
		bool DeleteBufferRegion(int32_t region) override;
		bool ReadBufferRegion(uint32_t region, int32_t a, int32_t b, int32_t c, int32_t d, int32_t e,
		                      int32_t f) override;
		bool DrawBufferRegion(uint32_t region, int32_t a, int32_t b, int32_t c, int32_t d, int32_t e,
		                      int32_t f) override;
		bool IsBufferRegion(uint32_t region) override;
		bool CanDoPartialRegionWrites(void) override;
		bool CanDoOffsetReads(void) override;
		bool DeleteAllBufferRegions(void) override;

		// cIGZGSnapshotExtension
		cIGZBuffer *CopyColorBuffer(int32_t x, int32_t y, int32_t width, int32_t height,
		                            cIGZBuffer *buffer) override;

		// cIGZGDriverLightingExtension
		void EnableLighting(bool enabled) override;
		void EnableLight(uint32_t light, bool enabled) override;
		void LightModelAmbient(float r, float g, float b, float a) override;
		void LightColor(uint32_t light, uint32_t parameter, float const *color) override;
		void LightColor(uint32_t light, float const *ambient, float const *diffuse,
		                float const *specular) override;
		void LightPosition(uint32_t light, float const *position) override;
		void LightDirection(uint32_t light, float const *direction) override;
		void MaterialColor(uint32_t parameter, float const *color) override;
		void MaterialColor(float const *ambient, float const *diffuse, float const *specular,
		                   float const *emission, float shininess) override;

	private:
		template<typename... Fields>
		void Record(nSCDWire::Op op, Fields const &... fields) {
			writer.Begin(op);
			(writer.Put(fields), ...);
			writer.End();
		}

		void RecordReservation(uint32_t name, uint32_t bytes);
		void RecordBlit(nSCDWire::BlitKind kind, int32_t x, int32_t y, int32_t width, int32_t height,
		                int32_t sourceWidth, int32_t sourceHeight, uint32_t format, uint32_t type,
		                void const *pixels, bool colorKeyed, void const *colorKey, uint32_t alpha);

		cGDriver *driver;
		nSCDWire::Writer writer;
		// Renderer=server. Released after the driver, which draws through it.
		std::unique_ptr<RemoteRenderer> remote;
		friend bool CreateDriver(uint32_t riid, void **ppvObj);

		uint32_t vertexFormat = 0;
		uint32_t vertexStride = 0;
		uint8_t const *vertexPointer = nullptr;
		int32_t unpackRowLength = 0;
		// The vertex-buffer extension's current reservation, which the game fills between GetVertices and
		// ReleaseVertices and draws from until the next GetVertices.
		uint8_t const *reservation = nullptr;
		uint32_t reservationVertices = 0;
		std::vector<uint8_t> scratch;
	};

	// The GZCOM factory for the driver class. With Record=<file> the driver comes wrapped in cGDriverWire
	// writing to that file.
	bool CreateDriver(uint32_t riid, void **ppvObj);

	// Lets nSCDWire::Replay drive a cGDriver: opens a windowed mode of the recorded size and refills the
	// terrain vertex buffer.
	class cGDriverReplayHooks : public nSCDWire::ReplayHooks {
	public:
		explicit cGDriverReplayHooks(cGDriver &driver) : driver(driver) {}
		void VideoMode(bool show, uint32_t width, uint32_t height, bool fullscreen) override;
		void Vertices(uint32_t name, void const *data, uint32_t bytes) override;

	private:
		cGDriver &driver;
	};
}
