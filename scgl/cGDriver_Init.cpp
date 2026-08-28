/*
 *  SCGL - a free graphics driver for SimCity 4's SimGL interface
 *  Copyright (C) 2025  Nelson Gomez (nsgomez) <nelson@ngomez.me>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#include "cGDriver.h"
#include "Diagnostics.h"
#include "SCGLD3D11Service.h"
#include "VideoModeUtils.h"

#ifndef NDEBUG
#include <d3d11sdklayers.h>
#endif

namespace nSCGL {
	namespace {
		char const *kWindowClassName = "GDriverClass--Direct3D11";
	}

	bool cGDriver::Init(void) {
		if (initialized) {
			return true;
		}

		WNDCLASSA windowClass{};
		windowClass.style = CS_OWNDC;
		windowClass.lpfnWndProc = DefWindowProcA;
		windowClass.hInstance = GetModuleHandleA(nullptr);
		windowClass.lpszClassName = kWindowClassName;

		if (!RegisterClassA(&windowClass) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
			Log(LogCategory::Initialization, "RegisterClassA failed (Win32 error %lu)", ::GetLastError());
			SetLastError(DriverError::CREATE_CONTEXT_FAIL);
			return false;
		}

		// These capabilities are implemented by the D3D11 target. Keep the existing
		// sGDMode mapping until its unknown fields are recovered from the Windows game.
		supportedExtensions.bgraColor = true;
		supportedExtensions.stencilBuffer = true;
		supportedExtensions.multitexture = true;
		supportedExtensions.textureEnvCombine = true;
		supportedExtensions.fogCoord = false;
		supportedExtensions.textureCompression = true;
		supportedExtensions.nvTextureEnvCombine4 = false;

		driverInfo = "Maxis 3D GDriver\nDirect3D 11\n11.0\n";
		if (InitializeVideoModeVector() == 0) {
			Log(LogCategory::Capabilities, "no compatible display modes were enumerated");
			UnregisterClassA(kWindowClassName, GetModuleHandleA(nullptr));
			SetLastError(DriverError::CREATE_CONTEXT_FAIL);
			return false;
		}

		initialized = true;
		Log(LogCategory::Initialization, "initialized; %d display modes available", videoModeCount);
		SetLastError(DriverError::OK);
		return true;
	}

	bool cGDriver::Shutdown(void) {
		if (!initialized) {
			DestroyD3D11Context();
			return true;
		}
		if (vertexBufferCacheHits + vertexBufferCacheMisses + indexBufferCacheHits + indexBufferCacheMisses != 0) {
			Log(LogCategory::Initialization,
			    "geometry cache: vertices %llu hits/%llu misses, indices %llu hits/%llu misses",
			    static_cast<unsigned long long>(vertexBufferCacheHits),
			    static_cast<unsigned long long>(vertexBufferCacheMisses),
			    static_cast<unsigned long long>(indexBufferCacheHits),
			    static_cast<unsigned long long>(indexBufferCacheMisses));
		}
		vertexBufferCacheHits = vertexBufferCacheMisses = 0;
		indexBufferCacheHits = indexBufferCacheMisses = 0;
		DestroyD3D11Context();
		UnregisterClassA(kWindowClassName, GetModuleHandleA(nullptr));
		videoModes.clear();
		videoModeCount = 0;
		currentVideoMode = -1;
		windowWidth = windowHeight = 0;
		initialized = false;
		Log(LogCategory::Initialization, "shutdown complete");
		return true;
	}

	void cGDriver::DestroyD3D11Context(void) {
		if (d3dDevice) {
			SCGLD3D11FrameContext const frame{
				sizeof(frame), 1, SCGL_D3D11_EVENT_BEFORE_DEVICE_DESTROY, deviceGeneration,
				d3dDevice.Get(), d3dContext.Get(), swapChain.Get(), renderTargetView.Get(),
				static_cast<HWND>(windowHandle)
			};
			InvokeD3D11FrameCallback(frame);
		}
		if (d3dContext) {
			d3dContext->OMSetRenderTargets(0, nullptr, nullptr);
			d3dContext->ClearState();
			d3dContext->Flush();
		}
		if (swapChain && presentationMode == PresentationMode::ExclusiveFullscreen) {
			HRESULT const result = swapChain->SetFullscreenState(FALSE, nullptr);
			if (FAILED(result)) LogHRESULT(LogCategory::SwapChain, "IDXGISwapChain::SetFullscreenState(windowed)", result);
		}

		for (BufferRegionResource &region: bufferRegions) region.texture.Reset();
		bufferRegionFlags = 0;
		depthStencilView.Reset();
		depthStencilTexture.Reset();
		renderTargetView.Reset();
		backBufferTexture.Reset();
		textures.clear();
		samplerStates.clear();
		boundTextures[0] = boundTextures[1] = 0;
		depthStencilStates.clear();
		blendStates.clear();
		rasterizerStates.clear();
		InvalidateD3D11StateCache();
		defaultSampler.Reset();
		depthRegionScratch.Reset();
		depthRegionScratchValid = false;
		dynamicIndexBuffer.Reset();
		dynamicVertexBuffer.Reset();
		for (GeometryCacheSegment &segment: indexBufferSegments) segment = {};
		for (GeometryCacheSegment &segment: vertexBufferSegments) segment = {};
		indexBufferCache.clear();
		vertexBufferCache.clear();
		for (auto &buffer: transformBuffers) buffer.Reset();
		activeTransformBuffer = 0;
		inputLayout.Reset();
		pixelShader.Reset();
		vertexShader.Reset();
		activeIndexBufferSegment = 0;
		activeVertexBufferSegment = 0;
		swapChain.Reset();
		presentationMode = PresentationMode::Windowed;
		swapChainFlags = 0;
		d3dContext.Reset();
#ifndef NDEBUG
		if (d3dDevice) {
			Microsoft::WRL::ComPtr<ID3D11Debug> debugDevice;
			if (SUCCEEDED(d3dDevice.As(&debugDevice))) {
				HRESULT const result = debugDevice->ReportLiveDeviceObjects(D3D11_RLDO_DETAIL);
				if (FAILED(result)) LogHRESULT(LogCategory::Resource, "ID3D11Debug::ReportLiveDeviceObjects", result);
			}
		}
#endif
		d3dDevice.Reset();

		if (windowHandle != nullptr) {
			DestroyWindow(static_cast<HWND>(windowHandle));
			windowHandle = nullptr;
		}
	}

	int32_t cGDriver::InitializeVideoModeVector(void) {
		videoModes.clear();
		videoModeCount = 0;

		DEVMODEA displayMode{};
		displayMode.dmSize = sizeof(displayMode);
		DWORD index = 0;
		while (EnumDisplaySettingsA(nullptr, index++, &displayMode)) {
			int const depth = static_cast<int>(displayMode.dmBitsPerPel);
			if (depth < 15) {
				continue;
			}

			AppendVideoMode(videoModes, displayMode.dmPelsWidth, displayMode.dmPelsHeight, depth, true);
			AppendVideoMode(videoModes, displayMode.dmPelsWidth, displayMode.dmPelsHeight, depth, false);
		}

		uint32_t const requiredWindowedModes[][2] = {
			{1920, 1080}, {2048, 1152}, {2560, 1600}, {3200, 1800}
		};
		for (auto const &dimensions: requiredWindowedModes) {
			AppendVideoMode(videoModes, dimensions[0], dimensions[1], 32, false);
		}
		videoModeCount = static_cast<int32_t>(videoModes.size());

		return videoModeCount;
	}
}
