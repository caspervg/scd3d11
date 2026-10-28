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
#include "D3D11Conversions.h"
#include "Diagnostics.h"
#include "SCD3D11Service.h"
#include "Settings.h"
#include "ShadowRegistry.h"
#include "ThumbnailFocusGuard.h"

#include <GZServPtrs.h>
#include <cIGZMessageServer2.h>
#include <cRZMessage2Standard.h>

#include <cstring>
#include <string>

#ifndef NDEBUG
#include <d3d11sdklayers.h>
#include <vector>
#endif

namespace nSCD3D11 {
	namespace {
		char const *kWindowClassName = "GDriverClass--Direct3D11";

		bool BorderlessFullscreenRequested() {
			return Settings::Get().borderless;
		}

		// SC4's own DX7 driver posts these after it restores a lost device. The first makes the game
		// upload its textures again, the second redraws the whole view. The messages are static
		// because the server only holds them by pointer and cRZUnknown never frees itself.
		void PostDeviceRestoredMessages() {
			static cRZMessage2Standard restoreTextures;
			static cRZMessage2Standard forceFullRedraw;
			cIGZMessageServer2Ptr messageServer;
			if (!messageServer) return;
			restoreTextures.SetType(0x8ACAA95F);
			forceFullRedraw.SetType(0x6ACB92A9);
			messageServer->MessagePost(static_cast<cIGZMessage2Standard *>(&restoreTextures), false);
			messageServer->MessagePost(static_cast<cIGZMessage2Standard *>(&forceFullRedraw), false);
		}

		RECT PrimaryMonitorRectangle() {
			MONITORINFO monitorInfo{sizeof(monitorInfo)};
			HMONITOR const monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
			if (monitor != nullptr && GetMonitorInfoA(monitor, &monitorInfo)) return monitorInfo.rcMonitor;
			return RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
		}

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

		HRESULT result = swapChain->GetBuffer(0, IID_PPV_ARGS(&swapChainBuffer));
		if (FAILED(result)) {
			LogHRESULT(LogCategory::Resource, "IDXGISwapChain::GetBuffer", result);
			return result;
		}

		D3D11_TEXTURE2D_DESC colorDescription{};
		swapChainBuffer->GetDesc(&colorDescription);
		colorDescription.BindFlags = D3D11_BIND_RENDER_TARGET;
		colorDescription.MiscFlags = 0;
		// Typeless so ReShade can also get an sRGB view. Copies to the UNORM swap chain buffer stay legal.
		colorDescription.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
		result = d3dDevice->CreateTexture2D(&colorDescription, nullptr, &backBuffer);
		if (FAILED(result)) {
			LogHRESULT(LogCategory::Resource, "ID3D11Device::CreateTexture2D(color)", result);
			swapChainBuffer.Reset();
			return result;
		}

		D3D11_RENDER_TARGET_VIEW_DESC colorViewDescription{};
		colorViewDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		colorViewDescription.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		result = d3dDevice->CreateRenderTargetView(backBuffer.Get(), &colorViewDescription, &renderTargetView);
		if (SUCCEEDED(result)) {
			colorViewDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
			result = d3dDevice->CreateRenderTargetView(backBuffer.Get(), &colorViewDescription, &renderTargetViewSrgb);
		}
		if (FAILED(result)) {
			LogHRESULT(LogCategory::Resource, "ID3D11Device::CreateRenderTargetView", result);
			renderTargetView.Reset();
			swapChainBuffer.Reset();
			return result;
		}

		D3D11_TEXTURE2D_DESC depthDescription{};
		depthDescription.Width = width;
		depthDescription.Height = height;
		depthDescription.MipLevels = 1;
		depthDescription.ArraySize = 1;
		// Typeless with a shader view so the scene depth can be handed to ReShade.
		bool const stencil = depthStencilFormat == DXGI_FORMAT_D24_UNORM_S8_UINT;
		depthDescription.Format = stencil ? DXGI_FORMAT_R24G8_TYPELESS : DXGI_FORMAT_R32_TYPELESS;
		depthDescription.SampleDesc.Count = 1;
		depthDescription.Usage = D3D11_USAGE_DEFAULT;
		depthDescription.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

		result = d3dDevice->CreateTexture2D(&depthDescription, nullptr, &depthStencilTexture);
		if (FAILED(result)) {
			LogHRESULT(LogCategory::Resource, "ID3D11Device::CreateTexture2D(depth)", result);
			renderTargetViewSrgb.Reset();
			renderTargetView.Reset();
			swapChainBuffer.Reset();
			return result;
		}

		D3D11_SHADER_RESOURCE_VIEW_DESC depthViewDescription{};
		depthViewDescription.Format = stencil ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : DXGI_FORMAT_R32_FLOAT;
		depthViewDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		depthViewDescription.Texture2D.MipLevels = 1;
		result = d3dDevice->CreateShaderResourceView(depthStencilTexture.Get(), &depthViewDescription, &depthShaderView);
		if (FAILED(result)) LogHRESULT(LogCategory::Resource, "ID3D11Device::CreateShaderResourceView(depth)", result);

		D3D11_DEPTH_STENCIL_VIEW_DESC depthStencilDescription{};
		depthStencilDescription.Format = depthStencilFormat;
		depthStencilDescription.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
		result = d3dDevice->CreateDepthStencilView(depthStencilTexture.Get(), &depthStencilDescription, &depthStencilView);
		// Window size changed. Force the depth region scratch to be recreated at the new size.
		depthRegionScratch.Reset();
		depthRegionScratchValid = false;
		if (FAILED(result)) {
			LogHRESULT(LogCategory::Resource, "ID3D11Device::CreateDepthStencilView", result);
			depthShaderView.Reset();
			depthStencilTexture.Reset();
			renderTargetViewSrgb.Reset();
			renderTargetView.Reset();
			swapChainBuffer.Reset();
			return result;
		}

		ID3D11RenderTargetView *renderTarget = renderTargetView.Get();
		d3dContext->OMSetRenderTargets(1, &renderTarget, depthStencilView.Get());
		backBufferTexture = backBuffer;
		reshadeEffectsInBackBuffer = false;

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

		// Borderless mode intentionally allows DXGI to scale the selected render
		// resolution to the monitor-sized client area.
		if (presentationMode == PresentationMode::BorderlessFullscreen) return S_OK;

		RECT client{};
		if (!GetClientRect(static_cast<HWND>(windowHandle), &client)) {
			HRESULT const result = HRESULT_FROM_WIN32(::GetLastError());
			LogHRESULT(LogCategory::SwapChain, "GetClientRect", result);
			return result;
		}

		uint32_t const width = static_cast<uint32_t>(client.right - client.left);
		uint32_t const height = static_cast<uint32_t>(client.bottom - client.top);
		if (width == 0 || height == 0) {
			if (!presentationPaused) {
				HWND const window = static_cast<HWND>(windowHandle);
				Log(LogCategory::SwapChain, "presentation paused: client area %ux%u (iconic=%d visible=%d)", width, height,
				    IsIconic(window) ? 1 : 0, IsWindowVisible(window) ? 1 : 0);
				presentationPaused = true;
			}
			return S_FALSE;
		}
		if (renderTargetView && depthStencilView &&
		    width == static_cast<uint32_t>(windowWidth) && height == static_cast<uint32_t>(windowHeight)) {
			return S_OK;
		}
		Log(LogCategory::SwapChain, "resizing swap chain buffers from %dx%d to %ux%u", windowWidth, windowHeight, width,
		    height);

		// Clear every direct context binding before releasing backbuffer views.
		// This covers state a frame callback may have installed outside SCD3D11's caches.
		d3dContext->ClearState();
		InvalidateD3D11StateCache();
		depthStencilView.Reset();
		depthShaderView.Reset();
		depthStencilTexture.Reset();
		renderTargetViewSrgb.Reset();
		renderTargetView.Reset();
		backBufferTexture.Reset();
		swapChainBuffer.Reset();

		HRESULT result = TakeInjectedFault(FAULT_RESIZE);
		if (SUCCEEDED(result)) result = swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, swapChainFlags);
		if (FAILED(result)) {
			LogHRESULT(LogCategory::SwapChain, "IDXGISwapChain::ResizeBuffers", result);
			// A removed device cannot back new targets. Flush recovers it instead.
			if (NoteDeviceLoss(result)) return result;
			// Keep rendering at the old size after a transient resize failure.
			NoteDeviceLoss(CreateBackBufferTargets(static_cast<uint32_t>(windowWidth), static_cast<uint32_t>(windowHeight)));
			return result;
		}
		result = CreateBackBufferTargets(width, height);
		NoteDeviceLoss(result);
		return result;
	}

	bool cGDriver::NoteDeviceLoss(HRESULT result) {
		if (SUCCEEDED(result) || !d3dDevice) return false;
		HRESULT const reason = d3dDevice->GetDeviceRemovedReason();
		bool const lost = result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET ||
		                  result == DXGI_ERROR_DEVICE_HUNG || FAILED(reason);
		if (lost && !deviceLost) {
			Log(LogCategory::Resource, "D3D11 device lost (HRESULT 0x%08lX, removed reason 0x%08lX)",
			    static_cast<unsigned long>(result), static_cast<unsigned long>(reason));
			deviceLost = true;
		}
		return lost;
	}

	void cGDriver::RecoverFromDeviceLoss() {
		ULONGLONG const now = GetTickCount64();
		if (recoveringDevice || now < nextDeviceRecovery) return;
		if (RecoverD3D11Device()) {
			deviceLost = false;
			deviceRecoveryFailures = 0;
			nextDeviceRecovery = 0;
			PostDeviceRestoredMessages();
			return;
		}
		// Back off from 0.5 s to 8 s so an unusable adapter does not stall every frame.
		deviceLost = true;
		if (deviceRecoveryFailures < 5) ++deviceRecoveryFailures;
		nextDeviceRecovery = now + (250ull << deviceRecoveryFailures);
		Log(LogCategory::Initialization, "device recovery failed, retrying in %llu ms",
		    static_cast<unsigned long long>(250ull << deviceRecoveryFailures));
	}

	bool cGDriver::RecoverD3D11Device() {
		if (recoveringDevice || currentVideoMode < 0 || currentVideoMode >= videoModeCount) return false;
		recoveringDevice = true;
		int32_t const mode = currentVideoMode;
		void *const procedure = windowProcedure;
		bool const show = showDriverWindow;
		Log(LogCategory::Initialization, "recreating D3D11 device after device loss");
		// Destroying notifies frame callbacks once for the old generation. recoveringDevice keeps
		// anything they call back into from starting another recovery.
		DestroyD3D11Context(true);
		SetVideoMode(mode, procedure, show, false);
		recoveringDevice = false;
		return IsDeviceReady();
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
		PresentationMode const requestedPresentationMode = !mode.isFullscreen
			                                                   ? PresentationMode::Windowed
			                                                   : BorderlessFullscreenRequested()
			                                                     ? PresentationMode::BorderlessFullscreen
			                                                     : PresentationMode::ExclusiveFullscreen;

		DestroyD3D11Context(recoveringDevice);
		presentationMode = requestedPresentationMode;

		bool const windowed = presentationMode == PresentationMode::Windowed;
		RECT const monitorRectangle = PrimaryMonitorRectangle();
		// Visible from creation, like the stock DirectX driver (0x10CF0000). DXGI saves the window style at
		// SetFullscreenState(TRUE) and silently restores it when it drops out of fullscreen (Win key, Alt+Tab):
		// a style saved without WS_VISIBLE hides the window for good, taskbar entry included. Also, SC4 loads
		// plugins on this thread before it shows the window itself.
		DWORD const style = windowed
			                        ? WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN
			                        : WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
		DWORD const extendedStyle = windowed ? WS_EX_APPWINDOW | WS_EX_WINDOWEDGE : WS_EX_APPWINDOW;
		RECT windowRectangle = presentationMode == PresentationMode::BorderlessFullscreen
			                       ? monitorRectangle
			                       : RECT{0, 0, static_cast<LONG>(mode.width), static_cast<LONG>(mode.height)};
		if (windowed && !AdjustWindowRectEx(&windowRectangle, style, FALSE, extendedStyle)) {
			Log(LogCategory::Initialization, "AdjustWindowRectEx failed (Win32 error %lu)", ::GetLastError());
			SetLastError(DriverError::CREATE_CONTEXT_FAIL);
			return;
		}
		this->windowProcedure = windowProcedure;
		showDriverWindow = showWindow;
		Log(LogCategory::Initialization, "SetVideoMode(%d, wndProc=%p, show=%u)",
		    newModeIndex, windowProcedure, showWindow ? 1u : 0u);
		int const windowX = windowed ? CW_USEDEFAULT : monitorRectangle.left;
		int const windowY = windowed ? CW_USEDEFAULT : monitorRectangle.top;

		uint32_t bufferWidth = mode.width;
		uint32_t bufferHeight = mode.height;
		HWND window = recoveringDevice ? static_cast<HWND>(windowHandle) : nullptr;
		if (window != nullptr) {
			Log(LogCategory::Window, "%p keeping the driver window for the new device", window);
			// The player may have resized a game window since the mode was set. Match its size now so the
			// first frame does not have to resize the new swap chain.
			RECT client{};
			if (windowed && GetClientRect(window, &client) && client.right > 0 && client.bottom > 0) {
				bufferWidth = static_cast<uint32_t>(client.right);
				bufferHeight = static_cast<uint32_t>(client.bottom);
			}
		} else {
			window = CreateWindowExA(
				extendedStyle,
				kWindowClassName,
				"SimCity 4 (Direct3D 11)",
				style,
				windowX,
				windowY,
				windowRectangle.right - windowRectangle.left,
				windowRectangle.bottom - windowRectangle.top,
				nullptr,
				nullptr,
				GetModuleHandleA(nullptr),
				this);
			if (window == nullptr) {
				Log(LogCategory::Initialization, "CreateWindowExA failed (Win32 error %lu)", ::GetLastError());
				SetLastError(DriverError::CREATE_CONTEXT_FAIL);
				return;
			}
			windowHandle = window;
			Log(LogCategory::Window, "%p driver window created (%d,%d %ldx%ld, style 0x%08lX)", window, windowX,
			    windowY, windowRectangle.right - windowRectangle.left, windowRectangle.bottom - windowRectangle.top,
			    static_cast<unsigned long>(style));
		}
		ThumbnailFocusGuard::Install(window);
		lastPresentResult = S_OK;
		lastFullscreenState = FALSE;
		presentationPaused = false;

		DXGI_SWAP_CHAIN_DESC swapChainDescription{};
		swapChainDescription.BufferDesc.Width = static_cast<UINT>(bufferWidth);
		swapChainDescription.BufferDesc.Height = static_cast<UINT>(bufferHeight);
		swapChainDescription.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		swapChainDescription.SampleDesc.Count = 1;
		swapChainDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		// Composed presentation (windowed, borderless) uses the flip model: less copying, lower latency and
		// power. SC4 renders into the persistent backBufferTexture either way, so the swap chain's own
		// buffers never have to keep their contents. Exclusive fullscreen keeps the legacy effect.
		bool const flipModel = preferFlipModel && presentationMode != PresentationMode::ExclusiveFullscreen;
		swapChainDescription.BufferCount = flipModel ? 2 : 1;
		swapChainDescription.OutputWindow = window;
		swapChainDescription.Windowed = TRUE;
		swapChainDescription.SwapEffect = flipModel ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_DISCARD;
		swapChainFlags = presentationMode == PresentationMode::ExclusiveFullscreen
			                 ? DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH
			                 : 0;
		swapChainDescription.Flags = swapChainFlags;

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

		// ReShade creates its effect runtime alongside the swap chain, so the add-on must exist first.
		InstallReShadeAddon();
		Microsoft::WRL::ComPtr<IDXGIAdapter> const preferredAdapter = SelectAdapter();
		auto createDevice = [&](D3D_FEATURE_LEVEL const *levels, UINT levelCount) {
			swapChain.Reset();
			d3dContext.Reset();
			d3dDevice.Reset();
			return D3D11CreateDeviceAndSwapChain(
				preferredAdapter.Get(), preferredAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
				nullptr, creationFlags,
				levels, levelCount, D3D11_SDK_VERSION, &swapChainDescription,
				&swapChain, &d3dDevice, &featureLevel, &d3dContext);
		};
		auto createForAvailableRuntime = [&]() {
			HRESULT createResult = createDevice(
				requestedLevels, static_cast<UINT>(sizeof(requestedLevels) / sizeof(requestedLevels[0])));
			if (createResult == E_INVALIDARG) {
				Log(LogCategory::Initialization, "D3D11.1 runtime unavailable, retrying without feature level 11_1");
				createResult = createDevice(
					legacyRequestedLevels,
					static_cast<UINT>(sizeof(legacyRequestedLevels) / sizeof(legacyRequestedLevels[0])));
			}
			return createResult;
		};

		HRESULT result = createForAvailableRuntime();
#ifndef NDEBUG
		if (result == DXGI_ERROR_SDK_COMPONENT_MISSING) {
			Log(LogCategory::Initialization, "D3D11 debug layer unavailable, retrying without it");
			creationFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
			result = createForAvailableRuntime();
		}
#endif
		if (FAILED(result) && flipModel) {
			// FLIP_DISCARD needs Windows 10.
			LogHRESULT(LogCategory::SwapChain, "flip model swap chain", result);
			Log(LogCategory::SwapChain, "using the legacy swap effect instead");
			swapChainDescription.BufferCount = 1;
			swapChainDescription.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
			result = createForAvailableRuntime();
		}
		if (FAILED(result)) {
			LogHRESULT(LogCategory::Initialization, "D3D11CreateDeviceAndSwapChain", result);
			DestroyD3D11Context(recoveringDevice);
			SetLastError(DriverError::CREATE_CONTEXT_FAIL);
			return;
		}

		Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
		Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
		Microsoft::WRL::ComPtr<IDXGIFactory> factory;
		if (SUCCEEDED(d3dDevice.As(&dxgiDevice)) && SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
		    SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
			factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
		}
		DXGI_ADAPTER_DESC adapterDescription{};
		if (adapter && SUCCEEDED(adapter->GetDesc(&adapterDescription))) {
			char name[sizeof(adapterDescription.Description)]{};
			WideCharToMultiByte(CP_UTF8, 0, adapterDescription.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
			// The user-mode driver version, packed as four 16-bit parts, from most to least significant.
			LARGE_INTEGER driverVersion{};
			if (FAILED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driverVersion))) driverVersion.QuadPart = 0;
			Log(LogCategory::Initialization,
			    "adapter: %s (vendor 0x%04X, device 0x%04X, driver %u.%u.%u.%u, %u MB dedicated, %s)",
			    name, adapterDescription.VendorId, adapterDescription.DeviceId,
			    HIWORD(driverVersion.HighPart), LOWORD(driverVersion.HighPart),
			    HIWORD(driverVersion.LowPart), LOWORD(driverVersion.LowPart),
			    static_cast<unsigned>(adapterDescription.DedicatedVideoMemory / (1024 * 1024)),
			    preferredAdapter ? "high-performance preference" : "system default");
			if (adapterDescription.VendorId == 0x1AF4) {
				Log(LogCategory::Initialization,
				    "adapter: this is the VirtIO GPU driver of a virtual machine. Its 3D support is still experimental, "
				    "so keep the VM's guest tools up to date if the game crashes or freezes");
			}
		}

		if (presentationMode == PresentationMode::ExclusiveFullscreen) {
			DXGI_MODE_DESC targetMode = swapChainDescription.BufferDesc;
			targetMode.RefreshRate = DXGI_RATIONAL{0, 0};
			result = swapChain->ResizeTarget(&targetMode);
			if (SUCCEEDED(result)) result = swapChain->SetFullscreenState(TRUE, nullptr);
			if (SUCCEEDED(result)) lastFullscreenState = TRUE;
			if (SUCCEEDED(result)) {
				result = swapChain->ResizeBuffers(
					0, static_cast<UINT>(mode.width), static_cast<UINT>(mode.height),
					DXGI_FORMAT_UNKNOWN, swapChainFlags);
			}
			if (FAILED(result)) {
				LogHRESULT(LogCategory::SwapChain, "enter exclusive fullscreen", result);
				DestroyD3D11Context(recoveringDevice);
				SetLastError(DriverError::CREATE_CONTEXT_FAIL);
				return;
			}
		}

		result = CreateGeometryPipeline();
		if (FAILED(result)) {
			DestroyD3D11Context(recoveringDevice);
			SetLastError(DriverError::CREATE_CONTEXT_FAIL);
			return;
		}

		result = CreateBackBufferTargets(bufferWidth, bufferHeight);
		if (FAILED(result)) {
			DestroyD3D11Context(recoveringDevice);
			SetLastError(DriverError::CREATE_CONTEXT_FAIL);
			return;
		}
		if (recoveringDevice && FAILED(RecreateTextureResources())) {
			DestroyD3D11Context(true);
			SetLastError(DriverError::CREATE_CONTEXT_FAIL);
			return;
		}
		deviceGeneration = NextD3D11DeviceGeneration();
		deviceLost = false;
		raiseWindowAfterPresent = presentationMode == PresentationMode::BorderlessFullscreen;

		currentVideoMode = newModeIndex;
		char const *modeName = presentationMode == PresentationMode::Windowed ? "windowed" :
		                       presentationMode == PresentationMode::BorderlessFullscreen ? "borderless fullscreen" :
		                       "exclusive fullscreen";
		Log(LogCategory::Capabilities, "D3D feature level 0x%04X, %s at %dx%d, %s swap chain", featureLevel, modeName,
		    bufferWidth, bufferHeight,
		    swapChainDescription.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ? "flip model" : "legacy");
		if (showWindow) {
			ShowWindow(window, SW_SHOWNORMAL);
			UpdateWindow(window);
		}
		StartRenderWatchdog(window);
		SetLastError(DriverError::OK);
	}

	bool cGDriver::IsDeviceReady(void) {
		return d3dDevice && d3dContext && swapChain && renderTargetView && depthStencilView;
	}

	void cGDriver::Flush(void) {
		// Every Flush counts as a frame for the watchdog, even one that skips Present.
		NoteRenderFrame();
		FlushFrame();
		ReportFrameStats();
		NoteRenderFrame();
		NoteRenderPhase("game (between Flush calls)");
	}

	void cGDriver::FlushFrame(void) {
		// The frame boundary is where a lost device is torn down and recreated, whichever call noticed it.
		if (recoveringDevice) return;
		if (deviceLost || !IsDeviceReady()) {
			NoteRenderPhase("Flush: device recovery");
			RecoverFromDeviceLoss();
			return;
		}

		if (presentationMode == PresentationMode::ExclusiveFullscreen) {
			NoteRenderPhase("Flush: GetFullscreenState");
			BOOL fullscreen = FALSE;
			if (SUCCEEDED(swapChain->GetFullscreenState(&fullscreen, nullptr)) && fullscreen != lastFullscreenState) {
				HWND const window = static_cast<HWND>(windowHandle);
				Log(LogCategory::SwapChain, "exclusive fullscreen state changed to %s (iconic=%d foreground=%d)",
				    fullscreen ? "fullscreen" : "windowed", IsIconic(window) ? 1 : 0,
				    GetForegroundWindow() == window ? 1 : 0);
				lastFullscreenState = fullscreen;
			}
		}

		NoteRenderPhase("Flush: resize check");
		HRESULT result = ResizeBackBufferIfNeeded();
		if (result == S_FALSE) {
			return;
		}
		if (presentationPaused) {
			Log(LogCategory::SwapChain, "presentation resumed at %dx%d", windowWidth, windowHeight);
			presentationPaused = false;
		}
		if (FAILED(result)) {
			SetLastError(DriverError::CREATE_CONTEXT_FAIL);
			if (deviceLost) RecoverFromDeviceLoss();
			return;
		}

		ID3D11RenderTargetView *renderTarget = renderTargetView.Get();
		d3dContext->OMSetRenderTargets(1, &renderTarget, depthStencilView.Get());
		SCD3D11FrameContext const frame{
			sizeof(frame), 1, SCD3D11_EVENT_RENDER, deviceGeneration,
			d3dDevice.Get(), d3dContext.Get(), swapChain.Get(), renderTargetView.Get(), static_cast<HWND>(windowHandle)
		};
		NoteRenderPhase("Flush: frame callback");
		if (InvokeD3D11FrameCallback(frame)) {
			// The callback owns the immediate context for the duration of the event.
			// Clear all of its bindings, then restore the output state that SCD3D11 owns.
			d3dContext->ClearState();
			InvalidateD3D11StateCache();
			ID3D11RenderTargetView *restoredRenderTarget = renderTargetView.Get();
			d3dContext->OMSetRenderTargets(1, &restoredRenderTarget, depthStencilView.Get());
			if (scissorEnabled) SetViewport(viewportX, viewportY, viewportWidth, viewportHeight);
			else SetViewport();
		}

		bool const vsyncEnabled = Settings::Get().vsync;
		NoteRenderPhase("Flush: ReShade effects");
		FinishReShadeFrame();
		d3dContext->CopyResource(swapChainBuffer.Get(), backBufferTexture.Get());
		NoteRenderPhase("Flush: Present");
		result = TakeInjectedFault(FAULT_PRESENT);
		if (SUCCEEDED(result)) result = swapChain->Present(vsyncEnabled ? 1 : 0, 0);
		NoteRenderPhase("Flush: after Present");
#ifndef NDEBUG
		LogDebugLayerMessages(d3dDevice.Get());
#endif
		if (result != lastPresentResult) {
			// Success codes matter too: DXGI_STATUS_OCCLUDED (0x087A0001) means nothing is visible.
			HWND const window = static_cast<HWND>(windowHandle);
			Log(LogCategory::SwapChain, "Present returned 0x%08lX (was 0x%08lX, iconic=%d visible=%d foreground=%d)",
			    static_cast<unsigned long>(result), static_cast<unsigned long>(lastPresentResult),
			    IsIconic(window) ? 1 : 0, IsWindowVisible(window) ? 1 : 0, GetForegroundWindow() == window ? 1 : 0);
			lastPresentResult = result;
		}
		if (FAILED(result)) {
			LogHRESULT(LogCategory::SwapChain, "IDXGISwapChain::Present", result);
			SetLastError(DriverError::CREATE_CONTEXT_FAIL);
		} else if (raiseWindowAfterPresent) {
			// Windows keeps the taskbar above a monitor-sized window that is not the foreground one, and
			// the window can lose activation while SC4 is still starting up. Once it shows a real frame,
			// put it on top and ask for the foreground. The foreground lock refuses that if the user has
			// since switched to another application.
			raiseWindowAfterPresent = false;
			HWND const window = static_cast<HWND>(windowHandle);
			RECT const monitor = PrimaryMonitorRectangle();
			BOOL const raised = SetWindowPos(window, HWND_TOP, monitor.left, monitor.top, monitor.right - monitor.left,
			                                 monitor.bottom - monitor.top, SWP_SHOWWINDOW | SWP_NOACTIVATE);
			BOOL const foreground = SetForegroundWindow(window);
			Log(LogCategory::Window, "%p raised after first present (z-order %s, foreground %s)", window,
			    raised ? "ok" : "failed", foreground ? "granted" : "refused");
		}
		if (NoteDeviceLoss(result) || deviceLost) RecoverFromDeviceLoss();
	}

	void cGDriver::SetViewport(void) {
		scissorEnabled = false;
		viewportX = viewportY = 0;
		viewportWidth = windowWidth;
		viewportHeight = windowHeight;
		UpdateViewportCull();
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
		// The scissor rectangle's right and bottom edges must stay representable.
		if (x < 0 || y < 0 || width < 0 || height < 0 ||
		    !RangeFits(static_cast<uint32_t>(x), static_cast<uint32_t>(width), INT32_MAX) ||
		    !RangeFits(static_cast<uint32_t>(y), static_cast<uint32_t>(height), INT32_MAX)) {
			SetLastError(DriverError::INVALID_VALUE);
			return;
		}
		scissorEnabled = true;
		viewportX = x;
		viewportY = y;
		viewportWidth = width;
		viewportHeight = height;
		UpdateViewportCull();
		++frameStats.viewports;
		frameStats.viewport[0] = x;
		frameStats.viewport[1] = y;
		frameStats.viewport[2] = width;
		frameStats.viewport[3] = height;
		frameStats.viewportPixels += static_cast<uint64_t>(width) * static_cast<uint64_t>(height);

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

	void cGDriver::UpdateViewportCull(void) {
		bool const disabled = !Settings::Get().viewportCull;
		// The scissor rectangle is always the viewport itself, so the viewport is all that can be rasterized.
		uint64_t const area = static_cast<uint64_t>(viewportWidth) * static_cast<uint64_t>(viewportHeight);
		uint64_t const window = static_cast<uint64_t>(windowWidth) * static_cast<uint64_t>(windowHeight);
		viewportCullActive = !disabled && scissorEnabled && windowWidth > 0 && windowHeight > 0 &&
		                     area * 10 < window * 9 &&
		                     MakeClipBounds(viewportX, viewportY, viewportWidth, viewportHeight,
		                                    viewportX, viewportY, viewportWidth, viewportHeight, 1.0f,
		                                    viewportCullBounds);
	}

	void cGDriver::ReportFrameStats(void) {
		if (!DrawStatsEnabled()) {
			frameStats = FrameStats{};
			return;
		}
		static LARGE_INTEGER const frequency = [] {
			LARGE_INTEGER value{};
			QueryPerformanceFrequency(&value);
			return value;
		}();
		struct Summary {
			LARGE_INTEGER start{};
			uint32_t frames = 0;
			uint32_t slowFrames = 0;
			uint32_t scrollFrames = 0;
			uint32_t fullFrames = 0;
			double totalMilliseconds = 0.0;
			double maxMilliseconds = 0.0;
			uint64_t draws = 0;
			uint64_t culledDraws = 0;
			uint64_t scrollDraws = 0;
			uint64_t scrollCulled = 0;
			uint64_t textureCreates = 0;
			uint64_t textureDeletes = 0;
			// Live shadow fields summed over the second. shadowStaticMaxMs and shadowWorldCasters are maxima.
			FrameStats shadows;
			uint32_t shadowFrames = 0;
			ShadowRegistry::Stats registry;
		};
		static Summary summary;

		LARGE_INTEGER now{};
		QueryPerformanceCounter(&now);
		double const milliseconds = lastFrameTime.QuadPart == 0 || frequency.QuadPart == 0
			                            ? 0.0
			                            : static_cast<double>(now.QuadPart - lastFrameTime.QuadPart) * 1000.0 /
			                              static_cast<double>(frequency.QuadPart);
		lastFrameTime = now;
		FrameStats const &frame = frameStats;
		// A scroll shifts the backing store with an offset region copy and redraws the exposed strip. A full
		// redraw clears the whole view first.
		bool const scroll = frame.offsetCopies != 0;
		bool const full = !scroll && frame.fullClears != 0;
		bool const shadowFrame = frame.shadowFullPasses + frame.shadowPartialPasses + frame.shadowComposites != 0;
		if (milliseconds > 100.0 && shadowFrame) {
			Log(LogCategory::Stats,
			    "slow frame %.1f ms: live shadows %u full + %u partial static passes, forced redraws %u, "
			    "static %.1f ms (max pass %.1f), dynamic %.1f ms, %u casters + %u registry",
			    milliseconds, frame.shadowFullPasses, frame.shadowPartialPasses, frame.shadowForcedRedraws,
			    frame.shadowStaticMs, frame.shadowStaticMaxMs, frame.shadowDynamicMs, frame.shadowCasters,
			    frame.shadowRegistryCasters);
		}
		if (milliseconds > 100.0) {
			Log(LogCategory::Stats,
			    "slow frame %.1f ms (%s): draws %u, culled %u, clears %u (full %u), region reads %u draws %u, "
			    "offset copies %u (last %+d,%+d), viewports %u (last %d,%d %dx%d, %llu px total), textures +%u -%u",
			    milliseconds, scroll ? "scroll" : full ? "full redraw" : "other", frame.draws, frame.culledDraws,
			    frame.colorClears, frame.fullClears, frame.regionReads, frame.regionDraws, frame.offsetCopies,
			    frame.offsetX, frame.offsetY, frame.viewports, frame.viewport[0], frame.viewport[1],
			    frame.viewport[2], frame.viewport[3], static_cast<unsigned long long>(frame.viewportPixels),
			    frame.textureCreates, frame.textureDeletes);
		}

		if (summary.start.QuadPart == 0) summary.start = now;
		++summary.frames;
		if (milliseconds > 100.0) ++summary.slowFrames;
		if (scroll) {
			++summary.scrollFrames;
			summary.scrollDraws += frame.draws;
			summary.scrollCulled += frame.culledDraws;
		}
		if (full) ++summary.fullFrames;
		summary.totalMilliseconds += milliseconds;
		if (milliseconds > summary.maxMilliseconds) summary.maxMilliseconds = milliseconds;
		summary.draws += frame.draws;
		summary.culledDraws += frame.culledDraws;
		summary.textureCreates += frame.textureCreates;
		summary.textureDeletes += frame.textureDeletes;
		if (shadowFrame) {
			FrameStats &sum = summary.shadows;
			++summary.shadowFrames;
			sum.shadowFullPasses += frame.shadowFullPasses;
			sum.shadowPartialPasses += frame.shadowPartialPasses;
			sum.shadowUntrackedPasses += frame.shadowUntrackedPasses;
			sum.shadowForcedRedraws += frame.shadowForcedRedraws;
			for (uint32_t cause = 0; cause < kLiveShadowRedrawCauses; ++cause)
				sum.shadowForcedBy[cause] += frame.shadowForcedBy[cause];
			sum.shadowRedisplays += frame.shadowRedisplays;
			sum.shadowMapRenders += frame.shadowMapRenders;
			sum.shadowCasters += frame.shadowCasters;
			sum.shadowRegistryCasters += frame.shadowRegistryCasters;
			sum.shadowPassCasters += frame.shadowPassCasters;
			sum.shadowWorldCasters = (std::max)(sum.shadowWorldCasters, frame.shadowWorldCasters);
			sum.shadowArenaRebuilds += frame.shadowArenaRebuilds;
			sum.shadowComposites += frame.shadowComposites;
			sum.shadowCompositePixels += frame.shadowCompositePixels;
			sum.shadowStaticMs += frame.shadowStaticMs;
			sum.shadowStaticMaxMs = (std::max)(sum.shadowStaticMaxMs, frame.shadowStaticMaxMs);
			sum.shadowDynamicMs += frame.shadowDynamicMs;
			sum.shadowTrackMs += frame.shadowTrackMs;
			sum.shadowTerrainMs += frame.shadowTerrainMs;
		}
		if (shadowFrame) {
			FrameStats &sum = summary.shadows;
			sum.shadowCullMs += frame.shadowCullMs;
			sum.shadowUploadMs += frame.shadowUploadMs;
			sum.shadowSubmitMs += frame.shadowSubmitMs;
			sum.shadowMeshUploads += frame.shadowMeshUploads;
			sum.shadowUploadBytes += frame.shadowUploadBytes;
			sum.shadowProjectionMismatches += frame.shadowProjectionMismatches;
		}
		ShadowRegistry::Stats const registry = ShadowRegistry::TakeStats();
		summary.registry.registrations += registry.registrations;
		summary.registry.offThreadRegistrations += registry.offThreadRegistrations;
		summary.registry.registerMs += registry.registerMs;
		summary.registry.gatherMs += registry.gatherMs;
		summary.registry.nativeDrawMs += registry.nativeDrawMs;
		// Captures and hashing happen in every draw, whether or not a shadow pass ran this frame.
		summary.shadows.shadowCaptures += frame.shadowCaptures;
		summary.shadows.shadowCaptureMs += frame.shadowCaptureMs;
		summary.shadows.shadowHashedDraws += frame.shadowHashedDraws;
		summary.shadows.shadowHashedVertices += frame.shadowHashedVertices;
		summary.shadows.shadowHashMs += frame.shadowHashMs;
		summary.shadows.shadowGpuMapMs += frame.shadowGpuMapMs;
		summary.shadows.shadowGpuCompositeMs += frame.shadowGpuCompositeMs;
		summary.shadows.shadowGpuSamples += frame.shadowGpuSamples;
		if (now.QuadPart - summary.start.QuadPart >= frequency.QuadPart) {
			Log(LogCategory::Stats,
			    "%u frames, avg %.1f ms, max %.1f ms, %u over 100 ms; draws %llu, culled %llu; "
			    "%u scroll frames (avg %llu draws, %llu culled), %u full redraws; textures +%llu -%llu",
			    summary.frames, summary.totalMilliseconds / summary.frames, summary.maxMilliseconds,
			    summary.slowFrames, static_cast<unsigned long long>(summary.draws),
			    static_cast<unsigned long long>(summary.culledDraws), summary.scrollFrames,
			    static_cast<unsigned long long>(summary.scrollFrames ? summary.scrollDraws / summary.scrollFrames : 0),
			    static_cast<unsigned long long>(summary.scrollFrames ? summary.scrollCulled / summary.scrollFrames : 0),
			    summary.fullFrames, static_cast<unsigned long long>(summary.textureCreates),
			    static_cast<unsigned long long>(summary.textureDeletes));
			FrameStats const &shadow = summary.shadows;
			if (summary.shadowFrames != 0 || shadow.shadowHashedDraws != 0) {
				double const renders = shadow.shadowMapRenders != 0 ? shadow.shadowMapRenders : 1.0;
				double const samples = shadow.shadowGpuSamples != 0 ? shadow.shadowGpuSamples : 1.0;
				Log(LogCategory::Stats,
				    "live shadows: %u frames; static passes %u full + %u partial (%u untracked); "
				    "forced full redraws %u (camera %u, untracked partial %u, terrain %u, translated %u); "
				    "redisplays %u; %u map renders, avg %.0f per-draw + %.0f registry casters (%u gathered, "
				    "%u tracked); %u composites, %.1f Mpx",
				    summary.shadowFrames, shadow.shadowFullPasses, shadow.shadowPartialPasses,
				    shadow.shadowUntrackedPasses, shadow.shadowForcedRedraws,
				    shadow.shadowForcedBy[kRedrawCamera], shadow.shadowForcedBy[kRedrawUntrackedPartial],
				    shadow.shadowForcedBy[kRedrawTerrainChanged], shadow.shadowForcedBy[kRedrawTranslated],
				    shadow.shadowRedisplays, shadow.shadowMapRenders, shadow.shadowCasters / renders,
				    shadow.shadowRegistryCasters / renders, shadow.shadowPassCasters, shadow.shadowWorldCasters,
				    shadow.shadowComposites, static_cast<double>(shadow.shadowCompositePixels) / 1.0e6);
				Log(LogCategory::Stats,
				    "live shadows CPU ms: static %.1f (max pass %.1f), dynamic %.1f, of which track %.1f, "
				    "terrain %.1f; capture %.1f (%u draws), hash %.1f (%u draws, %llu vertices); "
				    "GPU ms per call: map %.2f, composite %.2f (%u samples); arena rebuilds %u",
				    shadow.shadowStaticMs, shadow.shadowStaticMaxMs, shadow.shadowDynamicMs, shadow.shadowTrackMs,
				    shadow.shadowTerrainMs, shadow.shadowCaptureMs, shadow.shadowCaptures, shadow.shadowHashMs,
				    shadow.shadowHashedDraws, static_cast<unsigned long long>(shadow.shadowHashedVertices),
				    shadow.shadowGpuMapMs / samples, shadow.shadowGpuCompositeMs / samples,
				    shadow.shadowGpuSamples, shadow.shadowArenaRebuilds);
				Log(LogCategory::Stats,
				    "live shadows registry ms: cull %.1f, mesh upload %.1f (%u meshes, %.1f MB), submit %.1f; "
				    "AddShadow %.1f (%u records, %u off the draw thread), DrawShadows gather %.1f, native %.1f; "
				    "%u projection mismatches",
				    shadow.shadowCullMs, shadow.shadowUploadMs, shadow.shadowMeshUploads,
				    static_cast<double>(shadow.shadowUploadBytes) / 1.0e6, shadow.shadowSubmitMs,
				    summary.registry.registerMs, summary.registry.registrations,
				    summary.registry.offThreadRegistrations, summary.registry.gatherMs,
				    summary.registry.nativeDrawMs, shadow.shadowProjectionMismatches);
			}
			summary = Summary{};
			summary.start = now;
		}
		frameStats = FrameStats{};
	}

	void cGDriver::GetViewport(int32_t dimensions[4]) {
		dimensions[0] = viewportX;
		dimensions[1] = viewportY;
		dimensions[2] = viewportX + viewportWidth;
		dimensions[3] = viewportY + viewportHeight;
	}
}
