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

		char const *SystemCommandName(WPARAM command) {
			switch (command & 0xFFF0) {
				case SC_MINIMIZE: return "SC_MINIMIZE";
				case SC_MAXIMIZE: return "SC_MAXIMIZE";
				case SC_RESTORE: return "SC_RESTORE";
				case SC_CLOSE: return "SC_CLOSE";
				case SC_KEYMENU: return "SC_KEYMENU";
				case SC_TASKLIST: return "SC_TASKLIST";
				case SC_SCREENSAVE: return "SC_SCREENSAVE";
				case SC_MONITORPOWER: return "SC_MONITORPOWER";
				case SC_MOVE: return "SC_MOVE";
				case SC_SIZE: return "SC_SIZE";
				default: return "other";
			}
		}

		// Everything that can move focus, visibility or size of the driver window, for diagnosing
		// "the game vanished but is still running" reports.
		void LogWindowMessage(HWND window, bool current, UINT message, WPARAM wParam, LPARAM lParam) {
			char const *const stale = current ? "" : " (not the current driver window)";
			switch (message) {
				case WM_ACTIVATEAPP:
					Log(LogCategory::Window, "%p WM_ACTIVATEAPP %s (other thread %lu)%s", window,
					    wParam ? "activated" : "deactivated", static_cast<unsigned long>(lParam), stale);
					break;
				case WM_ACTIVATE: {
					WORD const state = LOWORD(wParam);
					Log(LogCategory::Window, "%p WM_ACTIVATE %s minimized=%u other=%p%s", window,
					    state == WA_INACTIVE ? "WA_INACTIVE" : state == WA_CLICKACTIVE ? "WA_CLICKACTIVE" : "WA_ACTIVE",
					    HIWORD(wParam) != 0 ? 1u : 0u, reinterpret_cast<HWND>(lParam), stale);
					break;
				}
				case WM_NCACTIVATE:
					Log(LogCategory::Window, "%p WM_NCACTIVATE %s%s", window, wParam ? "active" : "inactive", stale);
					break;
				case WM_SETFOCUS:
					Log(LogCategory::Window, "%p WM_SETFOCUS (from %p)%s", window, reinterpret_cast<HWND>(wParam), stale);
					break;
				case WM_KILLFOCUS:
					Log(LogCategory::Window, "%p WM_KILLFOCUS (to %p)%s", window, reinterpret_cast<HWND>(wParam), stale);
					break;
				case WM_ENABLE:
					Log(LogCategory::Window, "%p WM_ENABLE %u%s", window, wParam ? 1u : 0u, stale);
					break;
				case WM_CANCELMODE:
					Log(LogCategory::Window, "%p WM_CANCELMODE%s", window, stale);
					break;
				case WM_SHOWWINDOW:
					Log(LogCategory::Window, "%p WM_SHOWWINDOW show=%u status=%ld%s", window, wParam ? 1u : 0u,
					    static_cast<long>(lParam), stale);
					break;
				case WM_SIZE: {
					static char const *const types[] = {"restored", "minimized", "maximized", "maxshow", "maxhide"};
					Log(LogCategory::Window, "%p WM_SIZE %s %ux%u%s", window, wParam < 5 ? types[wParam] : "unknown",
					    LOWORD(lParam), HIWORD(lParam), stale);
					break;
				}
				case WM_WINDOWPOSCHANGED: {
					WINDOWPOS const *const position = reinterpret_cast<WINDOWPOS const *>(lParam);
					if (position == nullptr) break;
					UINT const flags = position->flags;
					bool const interesting = (flags & (SWP_SHOWWINDOW | SWP_HIDEWINDOW)) != 0 ||
					                         (flags & SWP_NOSIZE) == 0 || (flags & SWP_NOMOVE) == 0;
					(interesting ? Log : LogTrace)(
						LogCategory::Window, "%p WM_WINDOWPOSCHANGED %d,%d %dx%d flags=0x%04X after=%p%s", window,
						position->x, position->y, position->cx, position->cy, flags, position->hwndInsertAfter, stale);
					break;
				}
				case WM_SYSCOMMAND:
					Log(LogCategory::Window, "%p WM_SYSCOMMAND %s (0x%04X)%s", window, SystemCommandName(wParam),
					    static_cast<unsigned>(wParam), stale);
					break;
				case WM_ENTERSIZEMOVE:
				case WM_EXITSIZEMOVE:
					Log(LogCategory::Window, "%p %s%s", window,
					    message == WM_ENTERSIZEMOVE ? "WM_ENTERSIZEMOVE" : "WM_EXITSIZEMOVE", stale);
					break;
				case WM_DISPLAYCHANGE:
					Log(LogCategory::Window, "%p WM_DISPLAYCHANGE %ux%u %u bpp%s", window, LOWORD(lParam), HIWORD(lParam),
					    static_cast<unsigned>(wParam), stale);
					break;
				case WM_DPICHANGED:
					Log(LogCategory::Window, "%p WM_DPICHANGED %u%s", window, HIWORD(wParam), stale);
					break;
				case WM_DWMCOMPOSITIONCHANGED:
					Log(LogCategory::Window, "%p WM_DWMCOMPOSITIONCHANGED%s", window, stale);
					break;
				case WM_POWERBROADCAST:
					Log(LogCategory::Window, "%p WM_POWERBROADCAST 0x%04X%s", window, static_cast<unsigned>(wParam), stale);
					break;
				case WM_CLOSE:
				case WM_DESTROY:
				case WM_NCDESTROY:
					Log(LogCategory::Window, "%p %s%s", window,
					    message == WM_CLOSE ? "WM_CLOSE" : message == WM_DESTROY ? "WM_DESTROY" : "WM_NCDESTROY", stale);
					break;
				case WM_QUERYENDSESSION:
				case WM_ENDSESSION:
					Log(LogCategory::Window, "%p %s 0x%lX%s", window,
					    message == WM_ENDSESSION ? "WM_ENDSESSION" : "WM_QUERYENDSESSION",
					    static_cast<unsigned long>(lParam), stale);
					break;
				case WM_KEYDOWN:
				case WM_KEYUP:
				case WM_SYSKEYDOWN:
				case WM_SYSKEYUP: {
					bool const system = message == WM_SYSKEYDOWN || message == WM_SYSKEYUP;
					bool const up = message == WM_KEYUP || message == WM_SYSKEYUP;
					char const *const key = wParam == VK_LWIN ? "LWIN" : wParam == VK_RWIN ? "RWIN" :
					                        system && wParam == VK_TAB ? "Alt+Tab" :
					                        system && wParam == VK_F4 ? "Alt+F4" :
					                        system && wParam == VK_ESCAPE ? "Alt+Esc" : nullptr;
					if (key != nullptr) Log(LogCategory::Window, "%p key %s %s%s", window, key, up ? "up" : "down", stale);
					break;
				}
				case WM_CAPTURECHANGED:
					LogTrace(LogCategory::Window, "%p WM_CAPTURECHANGED (to %p)%s", window,
					         reinterpret_cast<HWND>(lParam), stale);
					break;
				case WM_MOUSEACTIVATE:
					LogTrace(LogCategory::Window, "%p WM_MOUSEACTIVATE%s", window, stale);
					break;
				default:
					break;
			}
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
		// windowHandle is only assigned once CreateWindowExA returns.
		LogWindowMessage(window, driver != nullptr && (driver->windowHandle == window || driver->windowHandle == nullptr),
		                 message, wParam, lParam);

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
			StopRenderWatchdog();
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
		StopRenderWatchdog();
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
		constantBufferCache.clear();
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
			Log(LogCategory::Window, "%p destroying driver window", windowHandle);
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
