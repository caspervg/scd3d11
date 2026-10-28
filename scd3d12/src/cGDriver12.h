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

#include "cIGZGDriver.h"
#include "cIGZGBufferRegionExtension.h"
#include "cIGZGDriverLightingExtension.h"
#include "cIGZGDriverVertexBufferExtension.h"
#include "cIGZGSnapshotExtension.h"
#include "sGDMode.h"
#include "wire/WireReplay.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace nSCD3D12 {
	// SimCity 4's SimGL driver interface on Direct3D 12, in the 64-bit server. It renders what
	// SCD3D11's cGDriver renders, from the same calls, into an offscreen back buffer the server reads
	// back or presents. cGDriver (scd3d11/) is the reference: same state rules, same shader maths, same
	// results within rounding.
	//
	// The game never talks to this object directly. Calls arrive through nSCDWire::Replay, so the
	// interfaces' pointer-returning methods that only make sense inside SC4 (GetVertices and friends)
	// are reached through the ReplayHooks instead.
	class cGDriver12 final :
			public cIGZGDriver,
			public cIGZGBufferRegionExtension,
			public cIGZGDriverLightingExtension,
			public cIGZGDriverVertexBufferExtension,
			public cIGZGSnapshotExtension,
			public nSCDWire::ReplayHooks {
	public:
		cGDriver12();
		~cGDriver12();

		// The sun the lighting pass relights the city with. SC4 turns its own sun with the camera, and so
		// does this one: directions are relative to the view.
		struct Sun {
			// Degrees about world up, from the screen's left (where SC4's own sun stands) towards the far
			// side of the scene.
			float azimuth = 0.0f;
			// Degrees above the horizon.
			float elevation = 45.0f;
			float color[3]{};
			float ambient[3]{};
		};

		// Turns relighting on, before Create. Each frame is then relit with sun into a frame of its own,
		// and the back buffer stays SC4's. Without it the back buffer is published as SC4 drew it.
		void Relight(Sun const &sun);

		// Creates the device. useWarp picks the software rasterizer (tests and machines without a GPU).
		// A non-null adapter picks that GPU, so textures can be shared with a device on the same one.
		bool Create(bool useWarp, LUID const *adapter = nullptr);

		// The frame Output() holds as tightly packed RGBA rows, after waiting for the GPU.
		bool ReadBack(std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height);
		// The GPU time of the frames finished since the last call, and how many there were.
		void TakeGpuTime(double &milliseconds, uint32_t &frames);
		ID3D12Device *Device() const;
		ID3D12CommandQueue *Queue() const;
		// The frame to publish, in the RENDER_TARGET state between calls: the relit frame, or the back
		// buffer without relighting.
		ID3D12Resource *Output() const;

		// For the scene lane (SceneLane.h), which draws some of the game's models from its own copies.

		// Asked before each DrawArrays (indices null, vertices from first) and DrawElements (first 0, indexType 3
		// for 16-bit or 5 for 32-bit indices). The vertices are the InterleavedArrays ones in the DrawState.
		// Returning false drops the game's draw.
		class DrawFilter {
		public:
			virtual ~DrawFilter() = default;
			virtual bool AllowDraw(uint32_t primitive, int32_t first, uint32_t count, uint32_t indexType,
			                       void const *indices) = 0;
		};
		void SetDrawFilter(DrawFilter *filter);

		// The state an S3D material sets, which the scene lane saves, compares and puts back around its draws.
		struct DrawState {
			struct Stage {
				bool enabled;
				uint32_t texture;
				uint8_t magFilter, minFilter, wrapU, wrapV, mode;
				uint32_t coordinates;
				float matrix[16];
			};
			Stage stages[2];
			uint8_t activeStage;
			bool alphaTest, depthTest, cullFace, blend;
			bool colorWrite, depthWrite;
			uint8_t depthFunction, sourceBlend, destinationBlend, alphaFunction;
			float alphaReference;
			bool lighting, diffuseFromVertex;
			float globalAmbient[4], materialAmbient[4], materialDiffuse[4];
			uint32_t vertexFormat, vertexStride;
			uint8_t const *vertexPointer;
		};
		void SaveDrawState(DrawState &state) const;
		void RestoreDrawState(DrawState const &state);

		// The command list being recorded, and memory in this frame's upload space for copies into the
		// caller's own resources.
		ID3D12GraphicsCommandList *CommandList() const;
		bool Upload(void const *data, uint64_t bytes, ID3D12Resource *&resource, uint64_t &offset);
		// A triangle list from the caller's buffers (D3D11Vertex layout, 16-bit indices) with the current state.
		// format is the SimGL vertex format the vertices came from.
		void DrawTriangles(D3D12_VERTEX_BUFFER_VIEW const &vertices, D3D12_INDEX_BUFFER_VIEW const &indices,
		                   uint32_t count, uint32_t format);

		// nSCDWire::ReplayHooks
		void VideoMode(bool show, uint32_t width, uint32_t height, bool fullscreen) override;
		void Resize(uint32_t width, uint32_t height) override;
		void Vertices(uint32_t name, void const *data, uint32_t bytes) override;
		void DrawBlobs(nSCDWire::BlobKey const &vertices, nSCDWire::BlobKey const &indices) override;
		bool Frame() override;
		void OwnerBegin(nSCDWire::DrawOwner const &owner) override;
		void OwnerEnd() override;

		// cIGZUnknown. Lifetime is owned by the server, so reference counting is nominal.
		bool QueryInterface(uint32_t riid, void **ppvObj) override;
		uint32_t AddRef(void) override;
		uint32_t Release(void) override;

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

		// cIGZGDriverVertexBufferExtension. Only DrawPrims, DrawPrimsIndexed and Reset are used: the
		// replay fills the buffer through Vertices().
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
		bool FinalRelease(void) override;
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
		// All D3D12 objects and SimGL state live here, so this header stays stable while the
		// implementation grows.
		struct Impl;
		std::unique_ptr<Impl> impl;
	};
}
