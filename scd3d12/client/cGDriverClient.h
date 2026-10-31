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

#include "cGDriverBase.h"
#include "RemoteRenderer.h"

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace nSCD3D12 {
	// SCD3D12's driver: SCD3D12Server draws everything, so this keeps only what the game asks about (texture
	// names, switches, the viewport, the terrain vertex buffer, buffer region numbers) and shows the server's
	// frames. Its Direct3D 11 device holds the swap chain and the frame being shown, nothing else.
	//
	// A server that dies is started again. The game is then asked to upload its textures again and to redraw
	// the whole view, as after a lost device.
	class cGDriverClient final : public nSCD3D11::cGDriverBase {
	public:
		// The class ID of the stock DirectX driver, which SCD3D11 takes over too. SCD3D12 claims it with a
		// higher version, so it wins when both are installed.
		static constexpr uint32_t kGZCLSID = 0xbadb6906;

		cGDriverClient();
		~cGDriverClient() override;

		// Starts the server and returns where the stream to it goes, the file too when recording. Without a
		// server the stream goes nowhere until one starts.
		std::unique_ptr<nSCDWire::Output> Connect(std::shared_ptr<std::FILE> record);

		// cGDriverBase
		void BackBufferSize(int32_t &width, int32_t &height) const override;
		bool RemoteOwnsView() const override;
		std::unique_ptr<nSCDWire::Output> TakeRestartedOutput() override;

		// cIGZUnknown
		bool QueryInterface(uint32_t riid, void **ppvObj) override;
		uint32_t AddRef(void) override;
		uint32_t Release(void) override;
		bool FinalRelease(void) override { return true; }

		// cIGZGDriver
		void DrawArrays(uint32_t, int32_t, int32_t) override {}
		void DrawElements(uint32_t, int32_t, uint32_t, void const *) override {}
		void InterleavedArrays(uint32_t, int32_t, void const *) override {}
		uint32_t MakeVertexFormat(uint32_t count, intptr_t elements) override;
		uint32_t MakeVertexFormat(uint32_t format) override;
		uint32_t VertexFormatStride(uint32_t format) override;
		uint32_t VertexFormatElementOffset(uint32_t format, uint32_t element, uint32_t index) override;
		uint32_t VertexFormatNumElements(uint32_t format, uint32_t element) override;
		void Clear(uint32_t) override {}
		void ClearColor(float, float, float, float) override {}
		void ClearDepth(double) override {}
		void ClearStencil(int32_t) override {}
		void ColorMask(bool) override {}
		void DepthFunc(uint32_t) override {}
		void DepthMask(bool) override {}
		void StencilFunc(uint32_t, int32_t, uint32_t) override {}
		void StencilMask(uint32_t) override {}
		void StencilOp(uint32_t, uint32_t, uint32_t) override {}
		void BlendFunc(uint32_t, uint32_t) override {}
		void AlphaFunc(uint32_t, float) override {}
		void ShadeModel(uint32_t) override {}
		void BindTexture(uint32_t target, uint32_t texture) override;
		void TexImage2D(uint32_t target, int32_t level, int32_t internalFormat, int32_t width, int32_t height,
		                int32_t border, uint32_t format, uint32_t type, void const *pixels) override;
		void PixelStore(uint32_t parameter, int32_t value) override;
		void TexEnv(uint32_t, uint32_t, int32_t) override {}
		void TexEnv(uint32_t, uint32_t, float const *) override {}
		void TexParameter(uint32_t, uint32_t, int32_t) override {}
		void Fog(uint32_t, uint32_t) override {}
		void Fog(uint32_t, float const *) override {}
		void ColorMultiplier(float, float, float) override {}
		void AlphaMultiplier(float) override {}
		void EnableVertexColors(bool, bool) override {}
		void GenTextures(int32_t count, uint32_t *names) override;
		void DeleteTextures(int32_t count, uint32_t const *names) override;
		bool IsTexture(uint32_t name) override;
		void PrioritizeTextures(int32_t, uint32_t const *, float const *) override {}
		bool AreTexturesResident(int32_t count, uint32_t const *names, bool *resident) override;
		void MatrixMode(uint32_t) override {}
		void LoadMatrix(float const *) override {}
		void LoadIdentity(void) override {}
		void Flush(void) override;
		void Enable(uint32_t state) override { SetCapability(state, true); }
		void Disable(uint32_t state) override { SetCapability(state, false); }
		bool IsEnabled(uint32_t state) override;
		void GetBoolean(uint32_t parameter, bool *value) override;
		void GetInteger(uint32_t parameter, int32_t *value) override;
		void GetFloat(uint32_t parameter, float *value) override;
		uint32_t GetError(void) override;
		void TexStage(uint32_t stage) override;
		void TexStageCoord(uint32_t) override {}
		void TexStageMatrix(float const *, uint32_t, uint32_t, uint32_t) override {}
		void TexStageCombine(eGDTextureStageCombineScaleParamType, eGDTextureStageCombineScaleParam) override {}
		void TexStageCombine(eGDTextureStageCombineOperandType, eGDBlend) override {}
		void TexStageCombine(eGDTextureStageCombineSourceParamType, eGDTextureStageCombineSourceParam) override {}
		void TexStageCombine(eGDTextureStageCombineParamType, eGDTextureStageCombineModeParam) override {}
		void SetTexture(uint32_t texture, uint32_t stage) override;
		intptr_t GetTexture(uint32_t stage) override;
		intptr_t CreateTexture(uint32_t internalFormat, uint32_t width, uint32_t height, uint32_t levels,
		                       uint32_t hints) override;
		void LoadTextureLevel(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, uint32_t,
		                      void const *) override {}
		void SetCombiner(cGDCombiner const &, uint32_t) override {}
		uint32_t CountVideoModes(void) const override;
		void GetVideoModeInfo(uint32_t index, sGDMode &mode) override;
		void GetVideoModeInfo(sGDMode &mode) override;
		void SetVideoMode(int32_t index, void *windowProcedure, bool show, bool fourth) override;
		void PolygonOffset(int32_t) override {}
		void BitBlt(int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, void const *, bool, void const *) override {}
		void StretchBlt(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, void const *, bool,
		                void const *) override {}
		void BitBltAlpha(int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, void const *, bool, void const *,
		                 uint32_t) override {}
		void StretchBltAlpha(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, void const *,
		                     bool, void const *, uint32_t) override {}
		void BitBltAlphaModulate(int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, void const *, bool,
		                         void const *, uint32_t) override {}
		void StretchBltAlphaModulate(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t,
		                             void const *, bool, void const *, uint32_t) override {}
		void SetViewport(int32_t x, int32_t y, int32_t width, int32_t height) override;
		void SetViewport(void) override;
		void GetViewport(int32_t dimensions[4]) override;
		char const *GetDriverInfo(void) const override;
		uint32_t GetGZCLSID(void) const override { return kGZCLSID; }
		bool Init(void) override;
		bool Shutdown(void) override;
		bool IsDeviceReady(void) override;
		bool Punt(uint32_t, void *) override { return false; }

		// cIGZGDriverVertexBufferExtension: the terrain's vertex buffer, in this process's memory.
		char const *GetVertexBufferName(uint32_t format) override;
		uint32_t VertexBufferType(uint32_t name) override;
		uint32_t MaxVertices(uint32_t name) override;
		uint32_t GetVertices(int32_t name, uint32_t count) override;
		uint32_t ContinueVertices(uint32_t name, uint32_t count) override;
		void ReleaseVertices(uint32_t name) override;
		void DrawPrims(uint32_t, uint32_t, void *, uint32_t) override {}
		void DrawPrimsIndexed(uint32_t, uint32_t, uint32_t, uint16_t *) override {}
		void Reset(void) override;

		// cIGZGBufferRegionExtension: numbers only, the server keeps the pixels.
		bool BufferRegionEnabled(void) override { return true; }
		uint32_t NewBufferRegion(int32_t type) override;
		bool DeleteBufferRegion(int32_t region) override;
		bool ReadBufferRegion(uint32_t region, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t) override {
			return IsBufferRegion(region);
		}
		bool DrawBufferRegion(uint32_t region, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t) override {
			return IsBufferRegion(region);
		}
		bool IsBufferRegion(uint32_t region) override;
		bool CanDoPartialRegionWrites(void) override { return true; }
		bool CanDoOffsetReads(void) override { return true; }
		bool DeleteAllBufferRegions(void) override;

		// cIGZGSnapshotExtension: the server's last frame, read back.
		cIGZBuffer *CopyColorBuffer(int32_t x, int32_t y, int32_t width, int32_t height, cIGZBuffer *buffer) override;

		// cIGZGDriverLightingExtension: all of it is the server's.
		void EnableLighting(bool) override {}
		void EnableLight(uint32_t, bool) override {}
		void LightModelAmbient(float, float, float, float) override {}
		void LightColor(uint32_t, uint32_t, float const *) override {}
		void LightColor(uint32_t, float const *, float const *, float const *) override {}
		void LightPosition(uint32_t, float const *) override {}
		void LightDirection(uint32_t, float const *) override {}
		void MaterialColor(uint32_t, float const *) override {}
		void MaterialColor(float const *, float const *, float const *, float const *, float) override {}

	private:
		enum class Error : uint32_t {
			OK = 0,
			OUT_OF_RANGE = 2,
			NOT_SUPPORTED = 3,
			CREATE_CONTEXT_FAIL = 6,
			INVALID_ENUM = 7,
			INVALID_VALUE = 8,
		};
		enum class Presentation { Windowed, Borderless, Exclusive };

		static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
		void SetCapability(uint32_t state, bool enabled);
		void Fail(Error error) { lastError = error; }
		bool CreateDevice(uint32_t width, uint32_t height, HWND window);
		void DestroyDevice();
		HRESULT CreateFrameTarget(uint32_t width, uint32_t height);
		// Keeps the swap chain at the window's size. False while the window has no area.
		bool FollowWindowSize();
		// Copies the server's frame `frame` into frameTarget, waiting for it.
		bool CopyServerFrame(uint64_t frame);
		// Starts a server in place of one that stopped, at most every few seconds.
		void RestartServer();

		bool initialized = false;
		Error lastError = Error::OK;
		std::string driverInfo;

		std::vector<sGDMode> videoModes;
		int32_t currentVideoMode = -1;
		Presentation presentation = Presentation::Windowed;
		HWND window = nullptr;
		void *gameWindowProcedure = nullptr;
		bool exclusivePending = false;
		ULONGLONG nextExclusiveAttempt = 0;
		int32_t width = 0, height = 0;
		UINT swapChainFlags = 0;
		Microsoft::WRL::ComPtr<ID3D11Device> device;
		Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
		Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain;
		// The server's frame being shown, which CopyColorBuffer reads too.
		Microsoft::WRL::ComPtr<ID3D11Texture2D> frameTarget;
		LUID adapter{};
		bool adapterKnown = false;

		// The server. Frames count Flush calls as the server counts them, from its start.
		std::unique_ptr<nSCD3D11::RemoteRenderer> remote;
		// One that stopped, kept until the stream to it is replaced.
		std::unique_ptr<nSCD3D11::RemoteRenderer> retired;
		std::unique_ptr<nSCDWire::Output> restartedOutput;
		std::shared_ptr<std::FILE> record;
		uint64_t frame = 0;
		// The first frame drawn at the current size. Earlier ones are gone with the old shared textures.
		uint64_t generationFrame = 0;
		uint64_t frameShown = UINT64_MAX;
		ULONGLONG nextRestart = 0;
		uint32_t restarts = 0;

		// Texture names, and whether each has an image yet.
		std::unordered_map<uint32_t, bool> textures;
		uint32_t nextTexture = 1;
		uint32_t boundTextures[2]{};
		uint32_t activeStage = 0;
		bool capabilities[kGDNumCapabilities]{};
		bool stageTextures[2]{};
		int32_t rowLength = 0;
		int32_t viewport[4]{};

		// The terrain vertex buffer (name 0) of the vertex buffer extension.
		std::vector<uint8_t> vertices;
		uint32_t vertexCursor = 0;
		bool verticesLocked = false;

		uint8_t bufferRegions = 0;
	};
}
