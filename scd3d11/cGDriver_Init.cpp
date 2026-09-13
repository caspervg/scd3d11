/*
 *  SCD3D11 - a free graphics driver for SimCity 4's SimGL interface
 *  Copyright (C) 2025  Nelson Gomez (nsgomez) <nelson@ngomez.me>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#include "cGDriver.h"
#include "Diagnostics.h"
#include "SCD3D11Service.h"
#include "VideoModeUtils.h"

#include <cstring>
#include <dxgi1_6.h>

#ifndef NDEBUG
#include <d3d11sdklayers.h>
#endif

namespace nSCD3D11 {
	namespace {
		char const *kWindowClassName = "GDriverClass--Direct3D11";

		bool SupportsFormat(ID3D11Device *device, DXGI_FORMAT format, UINT requiredSupport) {
			UINT support = 0;
			return device != nullptr && SUCCEEDED(device->CheckFormatSupport(format, &support)) &&
			       (support & requiredSupport) == requiredSupport;
		}

		HRESULT ProbeD3D11Device(
			IDXGIAdapter *adapter, Microsoft::WRL::ComPtr<ID3D11Device> &device, D3D_FEATURE_LEVEL &level) {
			D3D_FEATURE_LEVEL const levels[] = {
				D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
				D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
			};
			D3D_FEATURE_LEVEL const legacyLevels[] = {
				D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
			};
			D3D_DRIVER_TYPE const driverType = adapter != nullptr ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
			HRESULT result = D3D11CreateDevice(
				adapter, driverType, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
				levels, static_cast<UINT>(sizeof(levels) / sizeof(levels[0])), D3D11_SDK_VERSION,
				&device, &level, nullptr);
			if (result == E_INVALIDARG) {
				result = D3D11CreateDevice(
					adapter, driverType, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
					legacyLevels, static_cast<UINT>(sizeof(legacyLevels) / sizeof(legacyLevels[0])), D3D11_SDK_VERSION,
					&device, &level, nullptr);
			}
			return result;
		}
	}

	// Passing no adapter gets DXGI adapter 0, which on hybrid-GPU laptops is usually the integrated
	// GPU. Prefer the high-performance GPU instead; -GPU:default restores the old behaviour.
	Microsoft::WRL::ComPtr<IDXGIAdapter> cGDriver::SelectAdapter(void) {
		if (std::strstr(GetCommandLineA(), "-GPU:default") != nullptr) return nullptr;

		Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
		Microsoft::WRL::ComPtr<IDXGIFactory6> factory6;
		Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
		DXGI_ADAPTER_DESC1 description{};
		if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory.As(&factory6)) ||
		    FAILED(factory6->EnumAdapterByGpuPreference(
			    0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter))) ||
		    FAILED(adapter->GetDesc1(&description)) || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
			return nullptr;
		}
		return adapter;
	}

	LRESULT CALLBACK cGDriver::DriverWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
		cGDriver *driver = reinterpret_cast<cGDriver *>(GetWindowLongPtrA(window, GWLP_USERDATA));
		if (message == WM_NCCREATE) {
			CREATESTRUCTA const *creation = reinterpret_cast<CREATESTRUCTA const *>(lParam);
			driver = creation == nullptr ? nullptr : static_cast<cGDriver *>(creation->lpCreateParams);
			SetWindowLongPtrA(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(driver));
		}

		WNDPROC procedure = driver == nullptr ? nullptr : reinterpret_cast<WNDPROC>(driver->windowProcedure);
		if (procedure != nullptr) {
			MEMORY_BASIC_INFORMATION memory{};
			if (VirtualQuery(reinterpret_cast<void *>(procedure), &memory, sizeof(memory)) == sizeof(memory)) {
				DWORD const protection = memory.Protect & 0xff;
				bool const executable = memory.State == MEM_COMMIT &&
					(protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
					 protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY);
				if (executable) return CallWindowProcA(procedure, window, message, wParam, lParam);
			}
		}
		return DefWindowProcA(window, message, wParam, lParam);
	}

	bool cGDriver::Init(void) {
		if (initialized) {
			return true;
		}

		WNDCLASSA windowClass{};
		windowClass.style = CS_OWNDC;
		windowClass.lpfnWndProc = DriverWindowProcedure;
		windowClass.hInstance = GetModuleHandleA(nullptr);
		windowClass.lpszClassName = kWindowClassName;

		if (!RegisterClassA(&windowClass) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
			Log(LogCategory::Initialization, "RegisterClassA failed (Win32 error %lu)", ::GetLastError());
			SetLastError(DriverError::CREATE_CONTEXT_FAIL);
			return false;
		}

		Microsoft::WRL::ComPtr<ID3D11Device> probeDevice;
		D3D_FEATURE_LEVEL probeLevel{};
		HRESULT const probeResult = ProbeD3D11Device(SelectAdapter().Get(), probeDevice, probeLevel);
		if (FAILED(probeResult)) {
			LogHRESULT(LogCategory::Initialization, "D3D11CreateDevice(capability probe)", probeResult);
			UnregisterClassA(kWindowClassName, GetModuleHandleA(nullptr));
			SetLastError(DriverError::CREATE_CONTEXT_FAIL);
			return false;
		}

		supportedExtensions.bgraColor = true;
		supportedExtensions.stencilBuffer = SupportsFormat(
			probeDevice.Get(), DXGI_FORMAT_D24_UNORM_S8_UINT, D3D11_FORMAT_SUPPORT_DEPTH_STENCIL);
		supportedExtensions.multitexture = true;
		supportedExtensions.textureEnvCombine = true;
		supportedExtensions.fogCoord = false;
		UINT const bcSupport = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE;
		supportedExtensions.textureCompression =
			SupportsFormat(probeDevice.Get(), DXGI_FORMAT_BC1_UNORM, bcSupport) &&
			SupportsFormat(probeDevice.Get(), DXGI_FORMAT_BC2_UNORM, bcSupport) &&
			SupportsFormat(probeDevice.Get(), DXGI_FORMAT_BC3_UNORM, bcSupport);
		supportedExtensions.nvTextureEnvCombine4 = false;

		depthStencilFormat = supportedExtensions.stencilBuffer
			                     ? DXGI_FORMAT_D24_UNORM_S8_UINT
			                     : DXGI_FORMAT_D32_FLOAT;
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

	void cGDriver::DestroyD3D11Context(bool preserveResources) {
		if (d3dDevice) {
			SCD3D11FrameContext const frame{
				sizeof(frame), 1, SCD3D11_EVENT_BEFORE_DEVICE_DESTROY, deviceGeneration,
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
		if (!preserveResources) bufferRegionFlags = 0;
		depthStencilView.Reset();
		depthShaderView.Reset();
		depthStencilTexture.Reset();
		renderTargetViewSrgb.Reset();
		renderTargetView.Reset();
		backBufferTexture.Reset();
		swapChainBuffer.Reset();
		sceneDepth = SceneDepthPipeline{};
		blit = BlitPipeline{};
		if (preserveResources) {
			for (auto &entry: textures) {
				entry.second.texture.Reset();
				entry.second.view.Reset();
			}
			for (TextureStageState &stage: textureStages) stage.sampler.Reset();
		} else {
			textures.clear();
			boundTextures[0] = boundTextures[1] = 0;
		}
		samplerStates.clear();
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
		flatPixelShader.Reset();
		pixelShader.Reset();
		appliedPixelShader = nullptr;
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
			if (depth < 24) {
				continue;
			}

			AppendVideoMode(
				videoModes, displayMode.dmPelsWidth, displayMode.dmPelsHeight, 32, true,
				supportedExtensions.stencilBuffer, supportedExtensions.textureCompression);
			AppendVideoMode(
				videoModes, displayMode.dmPelsWidth, displayMode.dmPelsHeight, 32, false,
				supportedExtensions.stencilBuffer, supportedExtensions.textureCompression);
		}

		uint32_t const requiredWindowedModes[][2] = {
			{1920, 1080}, {2048, 1152}, {2560, 1600}, {3200, 1800}
		};
		for (auto const &dimensions: requiredWindowedModes) {
			AppendVideoMode(
				videoModes, dimensions[0], dimensions[1], 32, false,
				supportedExtensions.stencilBuffer, supportedExtensions.textureCompression);
		}
		videoModeCount = static_cast<int32_t>(videoModes.size());

		return videoModeCount;
	}
}
