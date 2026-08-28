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
#include "D3D11Conversions.h"
#include "Diagnostics.h"
#include "SCGLD3D11Service.h"

#include <cstring>

#ifndef NDEBUG
#include <d3d11sdklayers.h>
#include <vector>
#endif

namespace nSCGL {
	namespace {
		char const *kWindowClassName = "GDriverClass--Direct3D11";

#ifndef NDEBUG
		void LogDebugLayerMessages(ID3D11Device *device) {
			Microsoft::WRL::ComPtr<ID3D11InfoQueue> queue;
			if (device == nullptr || FAILED(device->QueryInterface(IID_PPV_ARGS(&queue)))) return;

			uint64_t const count = queue->GetNumStoredMessagesAllowedByRetrievalFilter();
			for (uint64_t index = 0; index < count; ++index) {
				SIZE_T size = 0;
				if (FAILED(queue->GetMessage(index, nullptr, &size)) || size == 0) continue;
				std::vector<uint8_t> storage(size);
				D3D11_MESSAGE *message = reinterpret_cast<D3D11_MESSAGE *>(storage.data());
				if (FAILED(queue->GetMessage(index, message, &size))) continue;
				if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
					Log(LogCategory::Resource, "D3D11 debug [%u/%u]: %s",
					    message->Severity, message->ID, message->pDescription);
				}
			}
			queue->ClearStoredMessages();
		}
#endif
	}

	uint32_t cGDriver::CountVideoModes(void) const {
		return videoModeCount;
	}

	void cGDriver::GetVideoModeInfo(uint32_t index, sGDMode &mode) {
		if (index >= static_cast<uint32_t>(videoModeCount)) {
			SetLastError(DriverError::OUT_OF_RANGE);
			return;
		}
		mode = videoModes[index];
	}

	void cGDriver::GetVideoModeInfo(sGDMode &mode) {
		GetVideoModeInfo(currentVideoMode, mode);
	}

	HRESULT cGDriver::CreateBackBufferTargets(uint32_t width, uint32_t height) {
		if (!d3dDevice || !d3dContext || !swapChain || width == 0 || height == 0) {
			return E_INVALIDARG;
		}

		Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
		HRESULT result = swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
		if (FAILED(result)) {
			LogHRESULT(LogCategory::Resource, "IDXGISwapChain::GetBuffer", result);
			return result;
		}

		result = d3dDevice->CreateRenderTargetView(backBuffer.Get(), nullptr, &renderTargetView);
		if (FAILED(result)) {
			LogHRESULT(LogCategory::Resource, "ID3D11Device::CreateRenderTargetView", result);
			return result;
		}

		D3D11_TEXTURE2D_DESC depthDescription{};
		depthDescription.Width = width;
		depthDescription.Height = height;
		depthDescription.MipLevels = 1;
		depthDescription.ArraySize = 1;
		depthDescription.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		depthDescription.SampleDesc.Count = 1;
		depthDescription.Usage = D3D11_USAGE_DEFAULT;
		depthDescription.BindFlags = D3D11_BIND_DEPTH_STENCIL;

		result = d3dDevice->CreateTexture2D(&depthDescription, nullptr, &depthStencilTexture);
		if (FAILED(result)) {
			LogHRESULT(LogCategory::Resource, "ID3D11Device::CreateTexture2D(depth)", result);
			renderTargetView.Reset();
			return result;
		}

		result = d3dDevice->CreateDepthStencilView(depthStencilTexture.Get(), nullptr, &depthStencilView);
		// Window size changed; force the depth region scratch to be recreated at the new size.
		depthRegionScratch.Reset();
		depthRegionScratchValid = false;
		if (FAILED(result)) {
			LogHRESULT(LogCategory::Resource, "ID3D11Device::CreateDepthStencilView", result);
			depthStencilTexture.Reset();
			renderTargetView.Reset();
			return result;
		}

		ID3D11RenderTargetView *renderTarget = renderTargetView.Get();
		d3dContext->OMSetRenderTargets(1, &renderTarget, depthStencilView.Get());
		backBufferTexture = backBuffer;

		windowWidth = static_cast<int>(width);
		windowHeight = static_cast<int>(height);
		SetViewport();
		result = RecreateBufferRegions();
		if (FAILED(result)) {
			return result;
		}
		Log(LogCategory::SwapChain, "back buffer ready at %ux%u", width, height);
		return S_OK;
	}

	HRESULT cGDriver::ResizeBackBufferIfNeeded() {
		if (!swapChain || windowHandle == nullptr) {
			return E_POINTER;
		}

		RECT client{};
		if (!GetClientRect(static_cast<HWND>(windowHandle), &client)) {
			HRESULT const result = HRESULT_FROM_WIN32(::GetLastError());
			LogHRESULT(LogCategory::SwapChain, "GetClientRect", result);
			return result;
		}

		uint32_t const width = static_cast<uint32_t>(client.right - client.left);
		uint32_t const height = static_cast<uint32_t>(client.bottom - client.top);
		if (width == 0 || height == 0) {
			return S_FALSE;
		}
		if (renderTargetView && depthStencilView &&
		    width == static_cast<uint32_t>(windowWidth) && height == static_cast<uint32_t>(windowHeight)) {
			return S_OK;
		}

		// Clear every direct context binding before releasing backbuffer views.
		// This covers state a frame callback may have installed outside SCGL's caches.
		d3dContext->ClearState();
		InvalidateD3D11StateCache();
		depthStencilView.Reset();
		depthStencilTexture.Reset();
		renderTargetView.Reset();
		backBufferTexture.Reset();

		HRESULT const result = swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
		if (FAILED(result)) {
			LogHRESULT(LogCategory::SwapChain, "IDXGISwapChain::ResizeBuffers", result);
			return result;
		}
		return CreateBackBufferTargets(width, height);
	}

	void cGDriver::SetVideoMode(int32_t newModeIndex, void *windowProcedure, bool showWindow, bool) {
		if (newModeIndex == -1) {
			DestroyD3D11Context();
			currentVideoMode = -1;
			windowWidth = windowHeight = 0;
			SetLastError(DriverError::OK);
			return;
		}
		if (newModeIndex < 0 || newModeIndex >= videoModeCount) {
			SetLastError(DriverError::OUT_OF_RANGE);
			return;
		}

		sGDMode const mode = videoModes[newModeIndex];
		if (mode.isFullscreen) {
			Log(LogCategory::Unsupported, "fullscreen mode %dx%d requested; using a windowed swap chain", mode.width,
			    mode.height);
		}

		DestroyD3D11Context();

		DWORD const style = WS_OVERLAPPEDWINDOW | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
		DWORD const extendedStyle = WS_EX_APPWINDOW | WS_EX_WINDOWEDGE;
		RECT windowRectangle{0, 0, static_cast<LONG>(mode.width), static_cast<LONG>(mode.height)};
		if (!AdjustWindowRectEx(&windowRectangle, style, FALSE, extendedStyle)) {
			Log(LogCategory::Initialization, "AdjustWindowRectEx failed (Win32 error %lu)", ::GetLastError());
			SetLastError(DriverError::CREATE_CONTEXT_FAIL);
			return;
		}

		HWND const window = CreateWindowExA(
			extendedStyle,
			kWindowClassName,
			"SimCity 4 (Direct3D 11)",
			style,
			CW_USEDEFAULT,
			CW_USEDEFAULT,
			windowRectangle.right - windowRectangle.left,
			windowRectangle.bottom - windowRectangle.top,
			nullptr,
			nullptr,
			GetModuleHandleA(nullptr),
			nullptr);
		if (window == nullptr) {
			Log(LogCategory::Initialization, "CreateWindowExA failed (Win32 error %lu)", ::GetLastError());
			SetLastError(DriverError::CREATE_CONTEXT_FAIL);
			return;
		}
		windowHandle = window;

		if (windowProcedure != nullptr) {
			::SetLastError(ERROR_SUCCESS);
			if (SetWindowLongPtrA(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(windowProcedure)) == 0 &&
			    ::GetLastError() != ERROR_SUCCESS) {
				Log(LogCategory::Initialization, "SetWindowLongPtrA failed (Win32 error %lu)", ::GetLastError());
				DestroyD3D11Context();
				SetLastError(DriverError::CREATE_CONTEXT_FAIL);
				return;
			}
		}

		DXGI_SWAP_CHAIN_DESC swapChainDescription{};
		swapChainDescription.BufferDesc.Width = static_cast<UINT>(mode.width);
		swapChainDescription.BufferDesc.Height = static_cast<UINT>(mode.height);
		swapChainDescription.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		swapChainDescription.SampleDesc.Count = 1;
		swapChainDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		swapChainDescription.BufferCount = 1;
		swapChainDescription.OutputWindow = window;
		swapChainDescription.Windowed = TRUE;
		swapChainDescription.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

		UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifndef NDEBUG
		creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
		D3D_FEATURE_LEVEL const requestedLevels[] = {
			D3D_FEATURE_LEVEL_11_1,
			D3D_FEATURE_LEVEL_11_0,
			D3D_FEATURE_LEVEL_10_1,
			D3D_FEATURE_LEVEL_10_0
		};
		D3D_FEATURE_LEVEL const legacyRequestedLevels[] = {
			D3D_FEATURE_LEVEL_11_0,
			D3D_FEATURE_LEVEL_10_1,
			D3D_FEATURE_LEVEL_10_0
		};

		auto createDevice = [&](D3D_FEATURE_LEVEL const *levels, UINT levelCount) {
			swapChain.Reset();
			d3dContext.Reset();
			d3dDevice.Reset();
			return D3D11CreateDeviceAndSwapChain(
				nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, creationFlags,
				levels, levelCount, D3D11_SDK_VERSION, &swapChainDescription,
				&swapChain, &d3dDevice, &featureLevel, &d3dContext);
		};
		auto createForAvailableRuntime = [&]() {
			HRESULT createResult = createDevice(
				requestedLevels, static_cast<UINT>(sizeof(requestedLevels) / sizeof(requestedLevels[0])));
			if (createResult == E_INVALIDARG) {
				Log(LogCategory::Initialization, "D3D11.1 runtime unavailable; retrying without feature level 11_1");
				createResult = createDevice(
					legacyRequestedLevels,
					static_cast<UINT>(sizeof(legacyRequestedLevels) / sizeof(legacyRequestedLevels[0])));
			}
			return createResult;
		};

		HRESULT result = createForAvailableRuntime();
#ifndef NDEBUG
		if (result == DXGI_ERROR_SDK_COMPONENT_MISSING) {
			Log(LogCategory::Initialization, "D3D11 debug layer unavailable; retrying without it");
			creationFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
			result = createForAvailableRuntime();
		}
#endif
		if (FAILED(result)) {
			LogHRESULT(LogCategory::Initialization, "D3D11CreateDeviceAndSwapChain", result);
			DestroyD3D11Context();
			SetLastError(DriverError::CREATE_CONTEXT_FAIL);
			return;
		}

		result = CreateGeometryPipeline();
		if (FAILED(result)) {
			DestroyD3D11Context();
			SetLastError(DriverError::CREATE_CONTEXT_FAIL);
			return;
		}

		result = CreateBackBufferTargets(static_cast<uint32_t>(mode.width), static_cast<uint32_t>(mode.height));
		if (FAILED(result)) {
			DestroyD3D11Context();
			SetLastError(DriverError::CREATE_CONTEXT_FAIL);
			return;
		}
		deviceGeneration = NextD3D11DeviceGeneration();

		currentVideoMode = newModeIndex;
		Log(LogCategory::Capabilities, "D3D feature level 0x%04X, BGRA support enabled", featureLevel);
		// SC4 manages visibility itself and may pass showWindow=false; the proven OpenGL
		// driver always showed its window, so do the same.
		ShowWindow(window, SW_SHOWNORMAL);
		UpdateWindow(window);
		SetLastError(DriverError::OK);
	}

	bool cGDriver::IsDeviceReady(void) {
		return d3dDevice && d3dContext && swapChain && renderTargetView && depthStencilView;
	}

	void cGDriver::Flush(void) {
		if (!IsDeviceReady()) {
			return;
		}

		HRESULT result = ResizeBackBufferIfNeeded();
		if (result == S_FALSE) {
			return;
		}
		if (FAILED(result)) {
			SetLastError(DriverError::CREATE_CONTEXT_FAIL);
			return;
		}

		ID3D11RenderTargetView *renderTarget = renderTargetView.Get();
		d3dContext->OMSetRenderTargets(1, &renderTarget, depthStencilView.Get());
		SCGLD3D11FrameContext const frame{
			sizeof(frame), 1, SCGL_D3D11_EVENT_RENDER, deviceGeneration,
			d3dDevice.Get(), d3dContext.Get(), swapChain.Get(), renderTargetView.Get(), static_cast<HWND>(windowHandle)
		};
		InvokeD3D11FrameCallback(frame);
		// The callback owns the immediate context for the duration of the event.
		// Clear all of its bindings, then restore the output state that SCGL owns.
		d3dContext->ClearState();
		InvalidateD3D11StateCache();
		ID3D11RenderTargetView *restoredRenderTarget = renderTargetView.Get();
		d3dContext->OMSetRenderTargets(1, &restoredRenderTarget, depthStencilView.Get());
		if (scissorEnabled) SetViewport(viewportX, viewportY, viewportWidth, viewportHeight);
		else SetViewport();

		static bool const vsyncEnabled = std::strstr(GetCommandLineA(), "-VSync:off") == nullptr;
		result = swapChain->Present(vsyncEnabled ? 1 : 0, 0);
#ifndef NDEBUG
		LogDebugLayerMessages(d3dDevice.Get());
#endif
		if (FAILED(result)) {
			LogHRESULT(LogCategory::SwapChain, "IDXGISwapChain::Present", result);
			if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET) {
				LogHRESULT(LogCategory::Resource, "ID3D11Device::GetDeviceRemovedReason",
				           d3dDevice->GetDeviceRemovedReason());
				DestroyD3D11Context();
			}
			SetLastError(DriverError::CREATE_CONTEXT_FAIL);
		}
	}

	void cGDriver::SetViewport(void) {
		scissorEnabled = false;
		viewportX = viewportY = 0;
		viewportWidth = windowWidth;
		viewportHeight = windowHeight;
		if (d3dContext) {
			D3D11_VIEWPORT viewport{
				0.0f, 0.0f,
				static_cast<float>(windowWidth), static_cast<float>(windowHeight),
				0.0f, 1.0f
			};
			d3dContext->RSSetViewports(1, &viewport);
		}
	}

	void cGDriver::SetViewport(int32_t x, int32_t y, int32_t width, int32_t height) {
		if (x < 0 || y < 0 || width < 0 || height < 0) {
			SetLastError(DriverError::INVALID_VALUE);
			return;
		}
		scissorEnabled = true;
		viewportX = x;
		viewportY = y;
		viewportWidth = width;
		viewportHeight = height;

		if (d3dContext) {
			int32_t const top = D3D11TopLeftY(windowHeight, y, height);
			D3D11_VIEWPORT viewport{};
			viewport.TopLeftX = static_cast<float>(x);
			viewport.TopLeftY = static_cast<float>(top);
			viewport.Width = static_cast<float>(width);
			viewport.Height = static_cast<float>(height);
			viewport.MinDepth = 0.0f;
			viewport.MaxDepth = 1.0f;
			d3dContext->RSSetViewports(1, &viewport);
			D3D11_RECT const scissor{x, top, x + width, top + height};
			d3dContext->RSSetScissorRects(1, &scissor);
		}
	}

	void cGDriver::GetViewport(int32_t dimensions[4]) {
		dimensions[0] = viewportX;
		dimensions[1] = viewportY;
		dimensions[2] = viewportX + viewportWidth;
		dimensions[3] = viewportY + viewportHeight;
	}
}
