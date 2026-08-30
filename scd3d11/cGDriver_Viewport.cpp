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

#include <cstdlib>
#include <cstring>
#include <string>

#ifndef NDEBUG
#include <d3d11sdklayers.h>
#include <vector>
#endif

namespace nSCD3D11 {
	namespace {
		char const *kWindowClassName = "GDriverClass--Direct3D11";

		// Present is the frame loop's only throttle, so every path that skips it has to idle
		// explicitly or SC4 spins a core at full speed with nothing on screen.
		DWORD const kIdleFrameSleepMilliseconds = 16;

		// Device recreations attempted before the driver stops trying. See deviceRecoveryFailures.
		uint32_t const kMaxDeviceRecoveryFailures = 3;

		std::string const &LowercaseCommandLine() {
			static std::string const commandLine = [] {
				std::string text = GetCommandLineA();
				for (char &character: text) {
					if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
				}
				return text;
			}();
			return commandLine;
		}

		bool BorderlessFullscreenRequested() {
			std::string const &commandLine = LowercaseCommandLine();
			return commandLine.find("-borderless") != std::string::npos ||
			       commandLine.find("-fullscreenmode:borderless") != std::string::npos;
		}

		// `-Monitor:<n>` picks a 1-based display for the fullscreen modes. 0 means "the primary".
		uint32_t RequestedMonitorIndex() {
			std::string const &commandLine = LowercaseCommandLine();
			size_t const position = commandLine.find("-monitor:");
			if (position == std::string::npos) return 0;
			return static_cast<uint32_t>(std::strtoul(commandLine.c_str() + position + 9, nullptr, 10));
		}

		struct MonitorSearch {
			uint32_t wanted;
			uint32_t seen;
			RECT rectangle;
		};

		BOOL CALLBACK SelectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM parameter) {
			MonitorSearch *const search = reinterpret_cast<MonitorSearch *>(parameter);
			MONITORINFO monitorInfo{sizeof(monitorInfo)};
			if (!GetMonitorInfoA(monitor, &monitorInfo)) return TRUE;
			if (++search->seen != search->wanted) return TRUE;
			search->rectangle = monitorInfo.rcMonitor;
			return FALSE;
		}

		RECT PrimaryMonitorRectangle() {
			MONITORINFO monitorInfo{sizeof(monitorInfo)};
			HMONITOR const monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
			if (monitor != nullptr && GetMonitorInfoA(monitor, &monitorInfo)) return monitorInfo.rcMonitor;
			return RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
		}

		// The display the fullscreen modes should occupy. Falls back to the primary monitor when
		// no monitor was requested, or when the requested one does not exist.
		RECT TargetMonitorRectangle() {
			uint32_t const wanted = RequestedMonitorIndex();
			if (wanted == 0) return PrimaryMonitorRectangle();

			MonitorSearch search{wanted, 0, PrimaryMonitorRectangle()};
			EnumDisplayMonitors(nullptr, nullptr, SelectMonitor, reinterpret_cast<LPARAM>(&search));
			return search.rectangle;
		}

		// A client area of exactly the requested mode, centred on the monitor. Both fullscreen
		// modes keep the client area the size the game renders at and believes it is presenting
		// to, so window-space mouse coordinates line up with the game's own UI layout; a mode
		// smaller than the monitor is centred rather than stretched to fit.
		RECT CentredClientRectangle(RECT const &monitor, uint32_t width, uint32_t height) {
			LONG const left = monitor.left + ((monitor.right - monitor.left) - static_cast<LONG>(width)) / 2;
			LONG const top = monitor.top + ((monitor.bottom - monitor.top) - static_cast<LONG>(height)) / 2;
			return RECT{left, top, left + static_cast<LONG>(width), top + static_cast<LONG>(height)};
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
		depthDescription.Format = depthStencilFormat;
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
		result = startupOverlay.Initialize(d3dDevice.Get(), width, height);
		if (FAILED(result)) {
			LogHRESULT(LogCategory::Resource, "StartupOverlay::Initialize", result);
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
		// This covers state a frame callback may have installed outside SCD3D11's caches.
		d3dContext->ClearState();
		InvalidateD3D11StateCache();
		depthStencilView.Reset();
		depthStencilTexture.Reset();
		renderTargetView.Reset();
		backBufferTexture.Reset();

		HRESULT const result = swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, swapChainFlags);
		if (FAILED(result)) {
			LogHRESULT(LogCategory::SwapChain, "IDXGISwapChain::ResizeBuffers", result);
			// Keep rendering at the old size after a transient resize failure.
			CreateBackBufferTargets(static_cast<uint32_t>(windowWidth), static_cast<uint32_t>(windowHeight));
			return result;
		}
		return CreateBackBufferTargets(width, height);
	}

	bool cGDriver::RecoverD3D11Device() {
		if (recoveringDevice || currentVideoMode < 0 || currentVideoMode >= videoModeCount) return false;
		if (deviceRecoveryFailures >= kMaxDeviceRecoveryFailures) return false;

		// If the exclusive-fullscreen mode switch is what keeps failing, a plain window is still
		// playable, so spend the last attempt on one.
		if (deviceRecoveryFailures + 1 == kMaxDeviceRecoveryFailures) fallbackToWindowed = true;

		recoveringDevice = true;
		int32_t const mode = currentVideoMode;
		void *const procedure = windowProcedure;
		bool const show = showDriverWindow;
		Log(LogCategory::Initialization, "recreating D3D11 device after device loss (attempt %u of %u)",
		    deviceRecoveryFailures + 1, kMaxDeviceRecoveryFailures);
		SetVideoMode(mode, procedure, show, false);
		recoveringDevice = false;

		if (IsDeviceReady()) {
			deviceRecoveryFailures = 0;
			return true;
		}
		if (++deviceRecoveryFailures >= kMaxDeviceRecoveryFailures) {
			Log(LogCategory::Initialization, "device recovery abandoned after %u attempts",
			    deviceRecoveryFailures);
		}
		return false;
	}

	// DXGI leaves exclusive fullscreen on its own when the window loses focus, when another
	// application takes the output, or after a foreign display mode change, without telling the
	// driver. Returns false while the swap chain must not be resized or presented to.
	bool cGDriver::SyncExclusiveFullscreenState() {
		if (presentationMode != PresentationMode::ExclusiveFullscreen) return true;

		BOOL fullscreen = FALSE;
		if (FAILED(swapChain->GetFullscreenState(&fullscreen, nullptr))) return false;
		if (fullscreen) return true;

		// Only reclaim the display once the game is the foreground window again, so the driver
		// does not fight the user for the output while they are working in another application.
		HWND const window = static_cast<HWND>(windowHandle);
		if (IsIconic(window) || GetForegroundWindow() != window) return false;

		HRESULT const result = swapChain->SetFullscreenState(TRUE, nullptr);
		if (FAILED(result)) {
			LogHRESULT(LogCategory::SwapChain, "IDXGISwapChain::SetFullscreenState(restore)", result);
			return false;
		}
		Log(LogCategory::SwapChain, "exclusive fullscreen reclaimed after DXGI dropped it");
		return true;
	}

	// A resolution or monitor-layout change moves the target display out from under a borderless
	// window; put it back where it belongs instead of leaving it at the stale rectangle.
	void cGDriver::RecentreBorderlessWindow() {
		if (presentationMode != PresentationMode::BorderlessFullscreen || windowHandle == nullptr) return;
		// The message can arrive between window creation and the first back buffer, when the
		// driver does not yet know what size to centre.
		if (windowWidth <= 0 || windowHeight <= 0) return;

		RECT const rectangle = CentredClientRectangle(
			TargetMonitorRectangle(), static_cast<uint32_t>(windowWidth), static_cast<uint32_t>(windowHeight));
		SetWindowPos(
			static_cast<HWND>(windowHandle), nullptr, rectangle.left, rectangle.top,
			rectangle.right - rectangle.left, rectangle.bottom - rectangle.top,
			SWP_NOZORDER | SWP_NOACTIVATE);
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

		// A mode set the game asked for clears any fullscreen fallback a previous recovery imposed.
		if (!recoveringDevice) fallbackToWindowed = false;

		sGDMode const mode = videoModes[newModeIndex];
		PresentationMode const requestedPresentationMode = (!mode.isFullscreen || fallbackToWindowed)
			                                                   ? PresentationMode::Windowed
			                                                   : BorderlessFullscreenRequested()
			                                                     ? PresentationMode::BorderlessFullscreen
			                                                     : PresentationMode::ExclusiveFullscreen;

		DestroyD3D11Context(recoveringDevice);
		presentationMode = requestedPresentationMode;

		bool const windowed = presentationMode == PresentationMode::Windowed;
		DWORD const style = windowed
			                        ? WS_OVERLAPPEDWINDOW | WS_CLIPSIBLINGS | WS_CLIPCHILDREN
			                        : WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
		DWORD const extendedStyle = windowed ? WS_EX_APPWINDOW | WS_EX_WINDOWEDGE : WS_EX_APPWINDOW;
		// Both fullscreen modes start out as a mode-sized window on the target display: borderless
		// stays that way, and exclusive fullscreen needs the window on the display it is claiming
		// because SetFullscreenState picks the output the window sits on.
		RECT windowRectangle = windowed
			                       ? RECT{0, 0, static_cast<LONG>(mode.width), static_cast<LONG>(mode.height)}
			                       : CentredClientRectangle(TargetMonitorRectangle(), mode.width, mode.height);
		if (windowed && !AdjustWindowRectEx(&windowRectangle, style, FALSE, extendedStyle)) {
			Log(LogCategory::Initialization, "AdjustWindowRectEx failed (Win32 error %lu)", ::GetLastError());
			SetLastError(DriverError::CREATE_CONTEXT_FAIL);
			return;
		}
		this->windowProcedure = windowProcedure;
		showDriverWindow = showWindow;
		Log(LogCategory::Initialization, "SetVideoMode(%d, wndProc=%p, show=%u)",
		    newModeIndex, windowProcedure, showWindow ? 1u : 0u);
		int const windowX = windowed ? CW_USEDEFAULT : windowRectangle.left;
		int const windowY = windowed ? CW_USEDEFAULT : windowRectangle.top;

		HWND const window = CreateWindowExA(
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

		if (presentationMode == PresentationMode::ExclusiveFullscreen) {
			DXGI_MODE_DESC targetMode = swapChainDescription.BufferDesc;
			targetMode.RefreshRate = DXGI_RATIONAL{0, 0};
			result = swapChain->ResizeTarget(&targetMode);
			if (SUCCEEDED(result)) result = swapChain->SetFullscreenState(TRUE, nullptr);
			// DXGI best practice: repeat ResizeTarget with a zeroed refresh rate after the
			// transition. Without it DXGI can settle on a rate that does not match the monitor's
			// real one and falls back to blitting instead of flipping in fullscreen.
			if (SUCCEEDED(result)) result = swapChain->ResizeTarget(&targetMode);
			// Zero dimensions adopt whatever DXGI actually settled on. ResizeTarget picks the
			// closest matching mode, which is not necessarily the one that was requested.
			if (SUCCEEDED(result)) result = swapChain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, swapChainFlags);
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

		// Size the targets from the swap chain rather than the requested mode: entering exclusive
		// fullscreen can land on the closest matching mode instead of the exact one, and the depth
		// buffer has to match the back buffer that came back.
		DXGI_SWAP_CHAIN_DESC settledDescription{};
		result = swapChain->GetDesc(&settledDescription);
		if (FAILED(result)) {
			LogHRESULT(LogCategory::SwapChain, "IDXGISwapChain::GetDesc", result);
			DestroyD3D11Context(recoveringDevice);
			SetLastError(DriverError::CREATE_CONTEXT_FAIL);
			return;
		}
		result = CreateBackBufferTargets(settledDescription.BufferDesc.Width, settledDescription.BufferDesc.Height);
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

		currentVideoMode = newModeIndex;
		char const *modeName = presentationMode == PresentationMode::Windowed ? "windowed" :
		                       presentationMode == PresentationMode::BorderlessFullscreen ? "borderless fullscreen" :
		                       "exclusive fullscreen";
		Log(LogCategory::Capabilities, "D3D feature level 0x%04X, %s at %dx%d", featureLevel, modeName,
		    mode.width, mode.height);
		// SC4 passes showWindow=false on the only SetVideoMode call it makes, but its own DirectX
		// driver (and upstream SCGL) put the window on screen as soon as the mode is set. Honouring
		// the flag leaves nothing visible until the game shows the window itself, which does not
		// happen until loading finishes - the game looks like it failed to start.
		(void) showWindow;
		// A device-loss recovery mid-game must not drop back to the startup notice.
		if (!recoveringDevice) {
			presentedFirstFrame = false;
			startupWindowMessages = 0;
		}
		startupOverlay.SetActive(ShouldShowStartupOverlay());
		// SW_SHOWNORMAL restores the window to its "normal" placement, which undoes the size and
		// position DXGI just applied when entering exclusive fullscreen. SW_SHOW leaves the
		// current placement alone.
		ShowWindow(window, windowed ? SW_SHOWNORMAL : SW_SHOW);
		SetForegroundWindow(window);
		// UpdateWindow dispatches WM_PAINT directly, so the notice appears even though the game is
		// not pumping its message queue yet.
		UpdateWindow(window);
		if (FAILED(PresentStartupOverlay())) PaintStartupNotice(window);
		SetLastError(DriverError::OK);
	}

	bool cGDriver::IsDeviceReady(void) {
		return d3dDevice && d3dContext && swapChain && renderTargetView && depthStencilView;
	}

	HRESULT cGDriver::PresentStartupOverlay() {
		if (!IsDeviceReady() || !startupOverlay.IsActive()) return S_FALSE;

		d3dContext->ClearState();
		InvalidateD3D11StateCache();
		ID3D11RenderTargetView *renderTarget = renderTargetView.Get();
		d3dContext->OMSetRenderTargets(1, &renderTarget, nullptr);
		SetViewport();
		startupOverlay.Draw(d3dContext.Get());
		static bool startupOverlayFlushLogged = false;
		if (startupOverlay.IsActive() && !startupOverlayFlushLogged) {
			Log(LogCategory::SwapChain, "startup overlay rendered during game Flush");
			startupOverlayFlushLogged = true;
		}

		HRESULT const result = swapChain->Present(0, 0);
		if (SUCCEEDED(result)) Log(LogCategory::SwapChain, "initial startup overlay presented");
		else LogHRESULT(LogCategory::SwapChain, "Present(initial startup overlay)", result);
		return result;
	}

	void cGDriver::Flush(void) {
		if (!IsDeviceReady()) {
			if (!RecoverD3D11Device()) Sleep(kIdleFrameSleepMilliseconds);
			return;
		}

		// DXGI minimizes an exclusive-fullscreen window when it loses focus, so both of these
		// idle paths are on the alt-tab route and have to throttle in Present's place.
		if (!SyncExclusiveFullscreenState()) {
			Sleep(kIdleFrameSleepMilliseconds);
			return;
		}

		HRESULT result = ResizeBackBufferIfNeeded();
		if (result == S_FALSE) {
			Sleep(kIdleFrameSleepMilliseconds);
			return;
		}
		if (FAILED(result)) {
			SetLastError(DriverError::CREATE_CONTEXT_FAIL);
			return;
		}

		ID3D11RenderTargetView *renderTarget = renderTargetView.Get();
		d3dContext->OMSetRenderTargets(1, &renderTarget, depthStencilView.Get());
		SCD3D11FrameContext const frame{
			sizeof(frame), 1, SCD3D11_EVENT_RENDER, deviceGeneration,
			d3dDevice.Get(), d3dContext.Get(), swapChain.Get(), renderTargetView.Get(), static_cast<HWND>(windowHandle)
		};
		InvokeD3D11FrameCallback(frame);
		// The callback owns the immediate context for the duration of the event.
		// Clear all of its bindings, then restore the output state that SCD3D11 owns.
		d3dContext->ClearState();
		InvalidateD3D11StateCache();
		ID3D11RenderTargetView *restoredRenderTarget = renderTargetView.Get();
		d3dContext->OMSetRenderTargets(1, &restoredRenderTarget, depthStencilView.Get());
		if (scissorEnabled) SetViewport(viewportX, viewportY, viewportWidth, viewportHeight);
		else SetViewport();
		startupOverlay.Draw(d3dContext.Get());

		static bool const vsyncEnabled = std::strstr(GetCommandLineA(), "-VSync:off") == nullptr;
		result = swapChain->Present(vsyncEnabled ? 1 : 0, 0);
#ifndef NDEBUG
		LogDebugLayerMessages(d3dDevice.Get());
#endif
		// A success code, but nothing reached the screen and the vsync wait was skipped, so this
		// is a third path that would otherwise spin.
		if (result == DXGI_STATUS_OCCLUDED) {
			Sleep(kIdleFrameSleepMilliseconds);
			return;
		}
		if (FAILED(result)) {
			LogHRESULT(LogCategory::SwapChain, "IDXGISwapChain::Present", result);
			if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET) {
				LogHRESULT(LogCategory::Resource, "ID3D11Device::GetDeviceRemovedReason",
				           d3dDevice->GetDeviceRemovedReason());
				DestroyD3D11Context(true);
				RecoverD3D11Device();
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
