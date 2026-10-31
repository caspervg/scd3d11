/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#include "cGDriverClient.h"

#include "Diagnostics.h"
#include "Settings.h"
#include "ThumbnailFocusGuard.h"
#include "VertexFormatUtils.h"
#include "VideoModeUtils.h"
#include "cIGZBuffer.h"

#include <GZServPtrs.h>
#include <cIGZGraphicSystem.h>
#include <cIGZMessageServer2.h>
#include <cRZMessage2Standard.h>

#include <dxgi1_6.h>

#include <algorithm>

using nSCD3D11::Log;
using nSCD3D11::LogCategory;
using nSCD3D11::LogHRESULT;

namespace nSCD3D12 {
	namespace {
		char const *const kWindowClassName = "GDriverClass--Direct3D12";
		// SC4's drivers name their windows GDriverWindow--<name>, which SC4GraphicsOptions looks for.
		char const *const kWindowName = "GDriverWindow--Direct3D12";
		// SC4's terrain draws reserve up to this many vertices of the one vertex buffer the extension has.
		constexpr uint32_t kTerrainVertices = 32768;
		constexpr size_t kBufferRegions = 8;
		constexpr ULONGLONG kSnapshotTimeoutMs = 2000;

		bool TearingSupported() {
			Microsoft::WRL::ComPtr<IDXGIFactory5> factory;
			BOOL allowed = FALSE;
			return SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
			       SUCCEEDED(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowed, sizeof(allowed))) &&
			       allowed;
		}

		// GPU=default: the system's choice. GPU=<n>: that one in DXGI's order. Otherwise the high-performance one,
		// which on a laptop with two is usually the one the server picks too.
		Microsoft::WRL::ComPtr<IDXGIAdapter> SelectAdapter() {
			nSCD3D11::Settings::GpuChoice const gpu = nSCD3D11::Settings::Get().gpu;
			if (gpu.systemDefault) return nullptr;
			Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
			Microsoft::WRL::ComPtr<IDXGIFactory6> factory6;
			Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
			DXGI_ADAPTER_DESC1 description{};
			if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;
			if (gpu.number != 0) {
				uint32_t count = 0;
				for (UINT index = 0; SUCCEEDED(factory->EnumAdapters1(index, &adapter)); ++index) {
					if (FAILED(adapter->GetDesc1(&description)) || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) continue;
					if (++count == gpu.number) return adapter;
				}
			}
			if (FAILED(factory.As(&factory6)) ||
			    FAILED(factory6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter))) ||
			    FAILED(adapter->GetDesc1(&description)) || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
				return nullptr;
			}
			return adapter;
		}

		RECT PrimaryMonitorRectangle() {
			MONITORINFO info{sizeof(info)};
			HMONITOR const monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
			if (monitor != nullptr && GetMonitorInfoA(monitor, &info)) return info.rcMonitor;
			return RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
		}

		// What SC4's DirectX driver posts after it restores a lost device: 0x8ACAA95F uploads every texture again,
		// 0x6ACB92A9 redraws the whole view. Static, as the message server only keeps a pointer.
		constexpr uint32_t kRestoreTextures = 0x8ACAA95F, kFullRedraw = 0x6ACB92A9;

		void PostGameMessage(uint32_t type) {
			static cRZMessage2Standard restoreTextures;
			static cRZMessage2Standard fullRedraw;
			cIGZMessageServer2Ptr messageServer;
			if (!messageServer) return;
			cRZMessage2Standard &message = type == kRestoreTextures ? restoreTextures : fullRedraw;
			message.SetType(type);
			messageServer->MessagePost(static_cast<cIGZMessage2Standard *>(&message), false);
		}
	}

	cGDriverClient::cGDriverClient() = default;

	cGDriverClient::~cGDriverClient() {
		Shutdown();
	}

	std::unique_ptr<nSCDWire::Output> cGDriverClient::Connect(std::shared_ptr<std::FILE> file) {
		record = std::move(file);
		auto server = std::make_unique<nSCD3D11::RemoteRenderer>();
		if (server->Start()) {
			remote = std::move(server);
			return remote->MakeOutput(record);
		}
		Log(LogCategory::Initialization, "server: not running, trying again in a few seconds");
		nextRestart = GetTickCount64() + 2000;
		// Until one runs, the stream goes nowhere (and a recording gets it alone).
		std::shared_ptr<std::FILE> const target = record;
		return std::make_unique<nSCDWire::SinkOutput>([target](void const *data, size_t size) {
			return target == nullptr ||
			       (std::fwrite(data, 1, size, target.get()) == size && std::fflush(target.get()) == 0);
		});
	}

	void cGDriverClient::BackBufferSize(int32_t &outWidth, int32_t &outHeight) const {
		outWidth = width;
		outHeight = height;
	}

	bool cGDriverClient::RemoteOwnsView() const {
		return remote != nullptr && remote->ServerOwnsView();
	}

	std::unique_ptr<nSCDWire::Output> cGDriverClient::TakeRestartedOutput() {
		return std::move(restartedOutput);
	}

	// cIGZUnknown

	bool cGDriverClient::QueryInterface(uint32_t riid, void **ppvObj) {
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

	uint32_t cGDriverClient::AddRef(void) { return cRZBaseUnknown::AddRef(); }
	uint32_t cGDriverClient::Release(void) { return cRZBaseUnknown::Release(); }

	// Vertex formats, which are SC4's own.

	uint32_t cGDriverClient::MakeVertexFormat(uint32_t, intptr_t) { return UINT32_MAX; }
	uint32_t cGDriverClient::MakeVertexFormat(uint32_t format) { return RZMakeVertexFormat(format); }
	uint32_t cGDriverClient::VertexFormatStride(uint32_t format) { return RZVertexFormatStride(format); }
	uint32_t cGDriverClient::VertexFormatElementOffset(uint32_t format, uint32_t element, uint32_t index) {
		return RZVertexFormatElementOffset(format, element, index);
	}
	uint32_t cGDriverClient::VertexFormatNumElements(uint32_t format, uint32_t element) {
		return RZVertexFormatNumElements(format, element);
	}

	// Textures: names, bindings and whether an image was given, as cGDriver answers them.

	void cGDriverClient::GenTextures(int32_t count, uint32_t *names) {
		if (count < 0 || (count > 0 && names == nullptr)) return Fail(Error::INVALID_VALUE);
		for (int32_t i = 0; i < count; ++i) {
			while (nextTexture == 0 || IsName(nextTexture)) ++nextTexture;
			if (nextTexture >= textures.size()) textures.resize((std::max)(static_cast<size_t>(nextTexture) + 1, textures.size() * 2));
			names[i] = nextTexture;
			textures[nextTexture++] = 1;
		}
	}

	void cGDriverClient::DeleteTextures(int32_t count, uint32_t const *names) {
		if (count < 0 || (count > 0 && names == nullptr)) return Fail(Error::INVALID_VALUE);
		for (int32_t i = 0; i < count; ++i) {
			for (uint32_t &bound: boundTextures) if (bound == names[i]) bound = 0;
			if (names[i] < textures.size()) textures[names[i]] = 0;
		}
	}

	bool cGDriverClient::IsTexture(uint32_t name) { return name < textures.size() && textures[name] == 2; }

	bool cGDriverClient::AreTexturesResident(int32_t count, uint32_t const *names, bool *resident) {
		if (count < 0 || (count > 0 && (names == nullptr || resident == nullptr))) {
			Fail(Error::INVALID_VALUE);
			return false;
		}
		bool all = true;
		for (int32_t i = 0; i < count; ++i) all &= resident[i] = IsTexture(names[i]);
		return all;
	}

	void cGDriverClient::BindTexture(uint32_t target, uint32_t texture) {
		if (target != 0 || (texture != 0 && !IsName(texture))) return Fail(Error::INVALID_VALUE);
		boundTextures[activeStage] = texture;
	}

	void cGDriverClient::TexImage2D(uint32_t target, int32_t level, int32_t, int32_t imageWidth, int32_t imageHeight,
	                                int32_t border, uint32_t, uint32_t, void const *) {
		uint32_t const bound = boundTextures[activeStage];
		if (target != 0 || level < 0 || border != 0 || imageWidth <= 0 || imageHeight <= 0 || !IsName(bound)) {
			return Fail(Error::INVALID_VALUE);
		}
		if (level == 0) textures[bound] = 2;
	}

	void cGDriverClient::PixelStore(uint32_t parameter, int32_t value) {
		if (parameter != 0 || value < 0) return Fail(Error::INVALID_VALUE);
		rowLength = value;
	}

	void cGDriverClient::TexStage(uint32_t stage) {
		if (stage >= 2) return Fail(Error::INVALID_VALUE);
		activeStage = stage;
	}

	void cGDriverClient::SetTexture(uint32_t texture, uint32_t stage) {
		if (stage >= 2 || (texture != 0 && !IsName(texture))) return Fail(Error::INVALID_VALUE);
		boundTextures[stage] = texture;
	}

	intptr_t cGDriverClient::GetTexture(uint32_t stage) {
		if (stage >= 2) {
			Fail(Error::INVALID_VALUE);
			return 0;
		}
		return boundTextures[stage];
	}

	intptr_t cGDriverClient::CreateTexture(uint32_t, uint32_t textureWidth, uint32_t textureHeight, uint32_t levels,
	                                       uint32_t) {
		if (textureWidth == 0 || textureHeight == 0 || levels == 0) {
			Fail(Error::CREATE_CONTEXT_FAIL);
			return 0;
		}
		uint32_t name = 0;
		GenTextures(1, &name);
		textures[name] = 2;
		boundTextures[activeStage] = name;
		return name;
	}

	// Switches and queries.

	void cGDriverClient::SetCapability(uint32_t state, bool enabled) {
		if (state >= kGDNumCapabilities || state == kGDCapability_Unused0) return Fail(Error::INVALID_ENUM);
		(state == kGDCapability_Texture2D ? stageTextures[activeStage] : capabilities[state]) = enabled;
	}

	bool cGDriverClient::IsEnabled(uint32_t state) {
		if (state >= kGDNumCapabilities || state == kGDCapability_Unused0) {
			Fail(Error::INVALID_ENUM);
			return false;
		}
		return state == kGDCapability_Texture2D ? stageTextures[activeStage] : capabilities[state];
	}

	void cGDriverClient::GetBoolean(uint32_t parameter, bool *value) {
		if (parameter != 0 || value == nullptr) return Fail(Error::INVALID_VALUE);
		*value = rowLength != 0;
	}

	void cGDriverClient::GetInteger(uint32_t parameter, int32_t *value) {
		if (parameter != 0 || value == nullptr) return Fail(Error::INVALID_VALUE);
		*value = rowLength;
	}

	void cGDriverClient::GetFloat(uint32_t parameter, float *value) {
		if (parameter != 0 || value == nullptr) return Fail(Error::INVALID_VALUE);
		*value = static_cast<float>(rowLength);
	}

	uint32_t cGDriverClient::GetError(void) {
		Error const error = lastError;
		lastError = Error::OK;
		return static_cast<uint32_t>(error);
	}

	char const *cGDriverClient::GetDriverInfo(void) const { return driverInfo.c_str(); }

	void cGDriverClient::SetViewport(int32_t x, int32_t y, int32_t viewportWidth, int32_t viewportHeight) {
		if (x < 0 || y < 0 || viewportWidth < 0 || viewportHeight < 0) return Fail(Error::INVALID_VALUE);
		viewport[0] = x;
		viewport[1] = y;
		viewport[2] = viewportWidth;
		viewport[3] = viewportHeight;
	}

	void cGDriverClient::SetViewport(void) {
		viewport[0] = viewport[1] = 0;
		viewport[2] = width;
		viewport[3] = height;
	}

	void cGDriverClient::GetViewport(int32_t dimensions[4]) {
		dimensions[0] = viewport[0];
		dimensions[1] = viewport[1];
		dimensions[2] = viewport[0] + viewport[2];
		dimensions[3] = viewport[1] + viewport[3];
	}

	// The terrain vertex buffer.

	char const *cGDriverClient::GetVertexBufferName(uint32_t) {
		// SC4 takes no name as the one terrain vertex buffer, name 0.
		return nullptr;
	}

	uint32_t cGDriverClient::VertexBufferType(uint32_t name) {
		return name == 0 ? kGDVertexFormat_V3F_C4UB_2T2F : UINT32_MAX;
	}

	uint32_t cGDriverClient::MaxVertices(uint32_t name) { return name == 0 ? kTerrainVertices : 0; }

	uint32_t cGDriverClient::GetVertices(int32_t name, uint32_t count) {
		uint32_t const stride = RZVertexFormatStride(kGDVertexFormat_V3F_C4UB_2T2F);
		if (name != 0 || count == 0 || count > kTerrainVertices || verticesLocked) return 0;
		if (vertexCursor + count > kTerrainVertices) vertexCursor = 0;
		if (vertices.empty()) vertices.resize(static_cast<size_t>(kTerrainVertices) * stride);
		uint8_t *const start = vertices.data() + static_cast<size_t>(vertexCursor) * stride;
		vertexCursor += count;
		verticesLocked = true;
		return reinterpret_cast<uint32_t>(start);
	}

	uint32_t cGDriverClient::ContinueVertices(uint32_t name, uint32_t count) {
		uint32_t const stride = RZVertexFormatStride(kGDVertexFormat_V3F_C4UB_2T2F);
		if (name != 0 || !verticesLocked || count == 0 || count > kTerrainVertices - vertexCursor) return 0;
		uint8_t *const start = vertices.data() + static_cast<size_t>(vertexCursor) * stride;
		vertexCursor += count;
		return reinterpret_cast<uint32_t>(start);
	}

	void cGDriverClient::ReleaseVertices(uint32_t name) {
		if (name == 0) verticesLocked = false;
	}

	void cGDriverClient::Reset(void) {
		vertexCursor = 0;
		verticesLocked = false;
	}

	// Buffer regions: numbers 1 to 8, as cGDriver hands them out.

	uint32_t cGDriverClient::NewBufferRegion(int32_t type) {
		if (type < 0 || type > 1) {
			Fail(Error::INVALID_ENUM);
			return 0;
		}
		for (uint32_t index = 0; index < kBufferRegions; ++index) {
			if ((bufferRegions & (1u << index)) != 0) continue;
			bufferRegions |= static_cast<uint8_t>(1u << index);
			return index + 1;
		}
		Fail(Error::OUT_OF_RANGE);
		return 0;
	}

	bool cGDriverClient::DeleteBufferRegion(int32_t region) {
		if (region < 1 || region > static_cast<int32_t>(kBufferRegions)) {
			Fail(Error::OUT_OF_RANGE);
			return false;
		}
		bufferRegions &= static_cast<uint8_t>(~(1u << (region - 1)));
		return true;
	}

	bool cGDriverClient::IsBufferRegion(uint32_t region) {
		return region >= 1 && region <= kBufferRegions && (bufferRegions & (1u << (region - 1))) != 0;
	}

	bool cGDriverClient::DeleteAllBufferRegions(void) {
		bufferRegions = 0;
		return true;
	}

	// Video modes, the window and the swap chain.

	bool cGDriverClient::Init(void) {
		if (initialized) return true;
		WNDCLASSA windowClass{};
		windowClass.style = CS_OWNDC;
		windowClass.lpfnWndProc = WindowProcedure;
		windowClass.hInstance = GetModuleHandleA(nullptr);
		windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
		windowClass.lpszClassName = kWindowClassName;
		if (!RegisterClassA(&windowClass) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
			Log(LogCategory::Initialization, "RegisterClassA failed (Win32 error %lu)", ::GetLastError());
			Fail(Error::CREATE_CONTEXT_FAIL);
			return false;
		}
		// The server draws with Direct3D 12, which has stencil and block compressed textures everywhere.
		videoModes.clear();
		DEVMODEA display{};
		display.dmSize = sizeof(display);
		for (DWORD index = 0; EnumDisplaySettingsA(nullptr, index, &display); ++index) {
			if (display.dmBitsPerPel < 24) continue;
			nSCD3D11::AppendVideoMode(videoModes, display.dmPelsWidth, display.dmPelsHeight, 32, true, true, true);
			nSCD3D11::AppendVideoMode(videoModes, display.dmPelsWidth, display.dmPelsHeight, 32, false, true, true);
		}
		for (auto const &size: {std::pair{1920u, 1080u}, {2048u, 1152u}, {2560u, 1600u}, {3200u, 1800u}}) {
			nSCD3D11::AppendVideoMode(videoModes, size.first, size.second, 32, false, true, true);
		}
		driverInfo = "Maxis 3D GDriver\nDirect3D 12\n12.0\n";
		initialized = true;
		Log(LogCategory::Initialization, "initialized with %zu display modes", videoModes.size());
		if (nSCD3D11::Settings::Get().borderless) {
			// Borderless needs no -f, so switch SC4 to full screen before it picks its screen mode.
			cIGZGraphicSystemPtr graphics;
			if (graphics && !graphics->IsFullScreenMode()) graphics->PreInitSetWindowedMode(false);
		}
		lastError = Error::OK;
		return true;
	}

	bool cGDriverClient::Shutdown(void) {
		if (!initialized) return true;
		DestroyDevice();
		if (window != nullptr) {
			DestroyWindow(window);
			window = nullptr;
		}
		UnregisterClassA(kWindowClassName, GetModuleHandleA(nullptr));
		videoModes.clear();
		currentVideoMode = -1;
		width = height = 0;
		initialized = false;
		Log(LogCategory::Initialization, "shutdown complete");
		return true;
	}

	uint32_t cGDriverClient::CountVideoModes(void) const { return static_cast<uint32_t>(videoModes.size()); }

	void cGDriverClient::GetVideoModeInfo(uint32_t index, sGDMode &mode) {
		if (index >= videoModes.size()) return Fail(Error::OUT_OF_RANGE);
		mode = videoModes[index];
	}

	void cGDriverClient::GetVideoModeInfo(sGDMode &mode) { GetVideoModeInfo(static_cast<uint32_t>(currentVideoMode), mode); }

	LRESULT CALLBACK cGDriverClient::WindowProcedure(HWND handle, UINT message, WPARAM wParam, LPARAM lParam) {
		auto *client = reinterpret_cast<cGDriverClient *>(GetWindowLongPtrA(handle, GWLP_USERDATA));
		if (message == WM_NCCREATE) {
			auto const *creation = reinterpret_cast<CREATESTRUCTA const *>(lParam);
			client = creation == nullptr ? nullptr : static_cast<cGDriverClient *>(creation->lpCreateParams);
			SetWindowLongPtrA(handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(client));
		}
		if (client != nullptr && (client->window == handle || client->window == nullptr)) {
			nSCD3D11::ThumbnailFocusGuard::NoteWindowMessage(message, wParam);
		}
		// SC4's own procedure, once it has handed one over and while its code is still there.
		auto const procedure = client == nullptr ? nullptr : reinterpret_cast<WNDPROC>(client->gameWindowProcedure);
		if (procedure != nullptr) {
			MEMORY_BASIC_INFORMATION memory{};
			if (VirtualQuery(reinterpret_cast<void *>(procedure), &memory, sizeof(memory)) == sizeof(memory)) {
				DWORD const protection = memory.Protect & 0xff;
				if (memory.State == MEM_COMMIT && (protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
				                                   protection == PAGE_EXECUTE_READWRITE ||
				                                   protection == PAGE_EXECUTE_WRITECOPY)) {
					return CallWindowProcA(procedure, handle, message, wParam, lParam);
				}
			}
		}
		return DefWindowProcA(handle, message, wParam, lParam);
	}

	void cGDriverClient::SetVideoMode(int32_t index, void *procedure, bool show, bool) {
		generationFrame = frame;
		if (index == -1) {
			DestroyDevice();
			if (window != nullptr) DestroyWindow(window);
			window = nullptr;
			currentVideoMode = -1;
			width = height = 0;
			lastError = Error::OK;
			return;
		}
		if (index < 0 || static_cast<size_t>(index) >= videoModes.size()) return Fail(Error::OUT_OF_RANGE);
		sGDMode const mode = videoModes[index];
		if (headless) {
			currentVideoMode = index;
			width = static_cast<int32_t>(mode.width);
			height = static_cast<int32_t>(mode.height);
			lastError = Error::OK;
			return;
		}
		DestroyDevice();
		if (window != nullptr) {
			DestroyWindow(window);
			window = nullptr;
		}
		presentation = !mode.isFullscreen ? Presentation::Windowed
		               : nSCD3D11::Settings::Get().borderless ? Presentation::Borderless : Presentation::Exclusive;
		bool const windowed = presentation == Presentation::Windowed;
		RECT const monitor = PrimaryMonitorRectangle();
		// Visible from creation, like SC4's DirectX driver: DXGI restores the style it saved on entering full
		// screen, and one saved without WS_VISIBLE would hide the window for good.
		DWORD const style = windowed ? WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN
		                             : WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
		DWORD const extendedStyle = windowed ? WS_EX_APPWINDOW | WS_EX_WINDOWEDGE : WS_EX_APPWINDOW;
		RECT bounds = presentation == Presentation::Borderless
			              ? monitor
			              : RECT{0, 0, static_cast<LONG>(mode.width), static_cast<LONG>(mode.height)};
		if (windowed) AdjustWindowRectEx(&bounds, style, FALSE, extendedStyle);
		gameWindowProcedure = procedure;
		window = CreateWindowExA(extendedStyle, kWindowClassName, kWindowName, style,
		                         windowed ? CW_USEDEFAULT : monitor.left, windowed ? CW_USEDEFAULT : monitor.top,
		                         bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr, nullptr,
		                         GetModuleHandleA(nullptr), this);
		if (window == nullptr) {
			Log(LogCategory::Initialization, "CreateWindowExA failed (Win32 error %lu)", ::GetLastError());
			return Fail(Error::CREATE_CONTEXT_FAIL);
		}
		nSCD3D11::ThumbnailFocusGuard::Install(window);
		if (!CreateDevice(mode.width, mode.height, window)) return Fail(Error::CREATE_CONTEXT_FAIL);
		currentVideoMode = index;
		Log(LogCategory::Capabilities, "%s at %dx%d", windowed ? "windowed" : presentation == Presentation::Borderless
			                                                                   ? "borderless fullscreen"
			                                                                   : "exclusive fullscreen", width, height);
		if (show) {
			ShowWindow(window, SW_SHOWNORMAL);
			UpdateWindow(window);
		}
		lastError = Error::OK;
	}

	bool cGDriverClient::CreateDevice(uint32_t bufferWidth, uint32_t bufferHeight, HWND target) {
		bool const exclusive = presentation == Presentation::Exclusive;
		bool const flip = !exclusive;
		DXGI_SWAP_CHAIN_DESC description{};
		description.BufferDesc.Width = bufferWidth;
		description.BufferDesc.Height = bufferHeight;
		description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		description.SampleDesc.Count = 1;
		description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		description.BufferCount = flip ? 2 : 1;
		description.OutputWindow = target;
		description.Windowed = TRUE;
		description.SwapEffect = flip ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_DISCARD;
		swapChainFlags = exclusive ? DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH : 0;
		// VSync=off on a flip model swap chain still waits for the display unless it may tear.
		if (flip && !nSCD3D11::Settings::Get().vsync && TearingSupported()) {
			swapChainFlags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
		}
		description.Flags = swapChainFlags;
		D3D_FEATURE_LEVEL const levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
		Microsoft::WRL::ComPtr<IDXGIAdapter> const preferred = SelectAdapter();
		HRESULT result = D3D11CreateDeviceAndSwapChain(
			preferred.Get(), preferred ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
			D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
			&description, &swapChain, &device, nullptr, &context);
		if (FAILED(result)) {
			LogHRESULT(LogCategory::Initialization, "D3D11CreateDeviceAndSwapChain", result);
			DestroyDevice();
			return false;
		}
		Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
		Microsoft::WRL::ComPtr<IDXGIAdapter> dxgiAdapter;
		Microsoft::WRL::ComPtr<IDXGIFactory> factory;
		if (SUCCEEDED(device.As(&dxgiDevice)) && SUCCEEDED(dxgiDevice->GetAdapter(&dxgiAdapter))) {
			if (SUCCEEDED(dxgiAdapter->GetParent(IID_PPV_ARGS(&factory)))) {
				factory->MakeWindowAssociation(target, DXGI_MWA_NO_ALT_ENTER);
			}
			DXGI_ADAPTER_DESC adapterDescription{};
			if (SUCCEEDED(dxgiAdapter->GetDesc(&adapterDescription))) {
				adapter = adapterDescription.AdapterLuid;
				adapterKnown = true;
				// The server opens its frames on the same adapter, so it waits for this one.
				if (remote != nullptr) remote->SetAdapter(adapter);
				char name[sizeof(adapterDescription.Description)]{};
				WideCharToMultiByte(CP_UTF8, 0, adapterDescription.Description, -1, name, sizeof(name) - 1, nullptr,
				                    nullptr);
				Log(LogCategory::Initialization, "adapter: %s", name);
			}
		}
		if (exclusive) {
			DXGI_MODE_DESC targetMode = description.BufferDesc;
			result = swapChain->ResizeTarget(&targetMode);
			if (SUCCEEDED(result)) result = swapChain->SetFullscreenState(TRUE, nullptr);
			// Refused while another window is in front: start in the window and try again from Flush.
			exclusivePending = result == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
			if (exclusivePending) result = S_OK;
			else if (SUCCEEDED(result)) {
				result = swapChain->ResizeBuffers(0, bufferWidth, bufferHeight, DXGI_FORMAT_UNKNOWN, swapChainFlags);
			}
			if (FAILED(result)) {
				LogHRESULT(LogCategory::SwapChain, "enter exclusive fullscreen", result);
				DestroyDevice();
				return false;
			}
		}
		if (FAILED(CreateFrameTarget(bufferWidth, bufferHeight))) {
			DestroyDevice();
			return false;
		}
		return true;
	}

	HRESULT cGDriverClient::CreateFrameTarget(uint32_t targetWidth, uint32_t targetHeight) {
		D3D11_TEXTURE2D_DESC description{};
		description.Width = targetWidth;
		description.Height = targetHeight;
		description.MipLevels = 1;
		description.ArraySize = 1;
		description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		description.SampleDesc.Count = 1;
		description.Usage = D3D11_USAGE_DEFAULT;
		frameTarget.Reset();
		HRESULT const result = device->CreateTexture2D(&description, nullptr, &frameTarget);
		if (FAILED(result)) {
			LogHRESULT(LogCategory::Resource, "ID3D11Device::CreateTexture2D(frame)", result);
			return result;
		}
		// Black until the server's first frame at this size arrives.
		Microsoft::WRL::ComPtr<ID3D11RenderTargetView> view;
		if (SUCCEEDED(device->CreateRenderTargetView(frameTarget.Get(), nullptr, &view))) {
			float const black[4]{0, 0, 0, 1};
			context->ClearRenderTargetView(view.Get(), black);
		}
		width = static_cast<int32_t>(targetWidth);
		height = static_cast<int32_t>(targetHeight);
		frameShown = UINT64_MAX;
		return S_OK;
	}

	void cGDriverClient::DestroyDevice() {
		if (swapChain && presentation == Presentation::Exclusive) swapChain->SetFullscreenState(FALSE, nullptr);
		frameTarget.Reset();
		swapChain.Reset();
		if (context) {
			context->ClearState();
			context->Flush();
		}
		context.Reset();
		device.Reset();
	}

	bool cGDriverClient::IsDeviceReady(void) { return device && swapChain && frameTarget; }

	bool cGDriverClient::FollowWindowSize() {
		// Borderless leaves scaling the chosen size to the monitor to DXGI.
		if (presentation == Presentation::Borderless) return true;
		RECT client{};
		if (!GetClientRect(window, &client)) return false;
		uint32_t const clientWidth = static_cast<uint32_t>(client.right - client.left);
		uint32_t const clientHeight = static_cast<uint32_t>(client.bottom - client.top);
		if (clientWidth == 0 || clientHeight == 0) return false;
		if (static_cast<int32_t>(clientWidth) == width && static_cast<int32_t>(clientHeight) == height) return true;
		Log(LogCategory::SwapChain, "resizing from %dx%d to %ux%u", width, height, clientWidth, clientHeight);
		context->ClearState();
		HRESULT const result = swapChain->ResizeBuffers(0, clientWidth, clientHeight, DXGI_FORMAT_UNKNOWN, swapChainFlags);
		if (FAILED(result)) {
			LogHRESULT(LogCategory::SwapChain, "IDXGISwapChain::ResizeBuffers", result);
			return false;
		}
		// The server draws at the new size from the frame the stream tells it about it.
		generationFrame = frame + 1;
		return SUCCEEDED(CreateFrameTarget(clientWidth, clientHeight));
	}

	bool cGDriverClient::CopyServerFrame(uint64_t serverFrame) {
		if (remote == nullptr || !remote->Active() || serverFrame < generationFrame) return false;
		if (frameShown == serverFrame) return true;
		if (!remote->CopyFrame(device.Get(), context.Get(), frameTarget.Get(), serverFrame)) return false;
		frameShown = serverFrame;
		return true;
	}

	void cGDriverClient::Flush(void) {
		// The stream to a server that stopped was replaced at the last Flush.
		retired.reset();
		if (IsDeviceReady()) {
			if (exclusivePending && GetTickCount64() >= nextExclusiveAttempt && GetForegroundWindow() == window &&
			    !IsIconic(window)) {
				nextExclusiveAttempt = GetTickCount64() + 1000;
				HRESULT const entered = swapChain->SetFullscreenState(TRUE, nullptr);
				if (entered != DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) exclusivePending = false;
			}
			if (FollowWindowSize()) {
				// The server's last finished frame: it draws one frame while the game builds the next.
				if (frame > 0) CopyServerFrame(frame - 1);
				Microsoft::WRL::ComPtr<ID3D11Texture2D> buffer;
				if (SUCCEEDED(swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer)))) context->CopyResource(buffer.Get(), frameTarget.Get());
				bool const composited = remote != nullptr && remote->Active() && remote->Composite(window);
				if (!composited) {
					bool const vsync = nSCD3D11::Settings::Get().vsync;
					HRESULT const result = swapChain->Present(
						vsync ? 1 : 0,
						!vsync && (swapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0 ? DXGI_PRESENT_ALLOW_TEARING : 0);
					if (FAILED(result)) LogHRESULT(LogCategory::SwapChain, "IDXGISwapChain::Present", result);
				}
			}
			if (remote != nullptr && frame > 0) remote->EndFrame(device.Get(), context.Get(), frame - 1);
		}
		++frame;
		// The server's scene started after SC4's last full redraw, so it would hold only what SC4 draws from now.
		if (remote != nullptr && remote->Active() && remote->RedrawRequests() != redrawRequestsSeen) {
			redrawRequestsSeen = remote->RedrawRequests();
			PostGameMessage(kFullRedraw);
		}
		RestartServer();
	}

	void cGDriverClient::RestartServer() {
		if (headless || (remote != nullptr && remote->Active())) return;
		ULONGLONG const now = GetTickCount64();
		if (now < nextRestart) return;
		// Two seconds, then longer between attempts that keep failing, up to half a minute.
		nextRestart = now + (std::min)(2000ull << (std::min)(restarts, 4u), 30000ull);
		++restarts;
		auto server = std::make_unique<nSCD3D11::RemoteRenderer>();
		if (!server->Start()) {
			Log(LogCategory::Initialization, "server: could not start it again");
			return;
		}
		if (adapterKnown) server->SetAdapter(adapter);
		if (record != nullptr) {
			Log(LogCategory::Initialization, "record: stopped, the server was started again");
			record.reset();
		}
		// The writer still holds the stream to the old one until it takes the new one.
		retired = std::move(remote);
		restartedOutput = server->MakeOutput(nullptr);
		remote = std::move(server);
		frame = generationFrame = 0;
		frameShown = UINT64_MAX;
		redrawRequestsSeen = 0;
		Log(LogCategory::Initialization, "server: started again; asking the game for its textures and a full redraw");
		PostGameMessage(kRestoreTextures);
		PostGameMessage(kFullRedraw);
	}

	// The snapshot: the server's last finished frame, read back through a staging copy.

	cIGZBuffer *cGDriverClient::CopyColorBuffer(int32_t x, int32_t y, int32_t copyWidth, int32_t copyHeight,
	                                            cIGZBuffer *buffer) {
		if (!IsDeviceReady() || copyWidth <= 0 || copyHeight <= 0) return nullptr;
		if (frame > 0) CopyServerFrame(frame - 1);
		int32_t const left = (std::max)(x, 0), top = (std::max)(y, 0);
		int32_t const right = static_cast<int32_t>((std::min)(static_cast<int64_t>(x) + copyWidth, int64_t{width}));
		int32_t const bottom = static_cast<int32_t>((std::min)(static_cast<int64_t>(y) + copyHeight, int64_t{height}));
		if (right <= left || bottom <= top) return nullptr;
		copyWidth = right - left;
		copyHeight = bottom - top;
		bool created = false;
		auto const fail = [&]() -> cIGZBuffer * {
			if (created && buffer != nullptr) buffer->Release();
			return nullptr;
		};
		if (buffer == nullptr || !buffer->IsReady()) {
			cIGZGraphicSystemPtr graphics;
			buffer = nullptr;
			if (!graphics || !graphics->CreateBuffer(&buffer) || buffer == nullptr) return nullptr;
			created = true;
			if (!buffer->Init(copyWidth, copyHeight, cGZBufferColorType::A8R8G8B8, 32)) return fail();
		} else if (buffer->GetColorType() != cGZBufferColorType::A8R8G8B8 ||
		           buffer->Width() != static_cast<uint32_t>(copyWidth) ||
		           buffer->Height() != static_cast<uint32_t>(copyHeight)) {
			return nullptr;
		}
		D3D11_TEXTURE2D_DESC description{};
		description.Width = static_cast<UINT>(copyWidth);
		description.Height = static_cast<UINT>(copyHeight);
		description.MipLevels = 1;
		description.ArraySize = 1;
		description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		description.SampleDesc.Count = 1;
		description.Usage = D3D11_USAGE_STAGING;
		description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
		HRESULT result = device->CreateTexture2D(&description, nullptr, &staging);
		if (FAILED(result)) return fail();
		D3D11_BOX const box{static_cast<UINT>(left), static_cast<UINT>(top), 0, static_cast<UINT>(right),
		                    static_cast<UINT>(bottom), 1};
		context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, frameTarget.Get(), 0, &box);
		context->Flush();
		// A copy that does not finish costs a missing thumbnail, which SC4 replaces with its own, not a hung save.
		D3D11_MAPPED_SUBRESOURCE mapping{};
		ULONGLONG const deadline = GetTickCount64() + kSnapshotTimeoutMs;
		while ((result = context->Map(staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapping)) ==
		       DXGI_ERROR_WAS_STILL_DRAWING) {
			if (GetTickCount64() >= deadline) return fail();
			Sleep(1);
		}
		if (FAILED(result)) return fail();
		bool const locked = buffer->Lock(cIGZBuffer::eLockFlags::IsDirtyUpdate);
		if (locked) {
			for (int32_t row = 0; row < copyHeight; ++row) {
				uint8_t const *source = static_cast<uint8_t const *>(mapping.pData) + static_cast<size_t>(row) * mapping.RowPitch;
				for (int32_t column = 0; column < copyWidth; ++column) {
					buffer->SetPixel(column, row, 0xff000000u | static_cast<uint32_t>(source[column * 4]) << 16 |
					                              static_cast<uint32_t>(source[column * 4 + 1]) << 8 |
					                              source[column * 4 + 2]);
				}
			}
			buffer->Unlock(cIGZBuffer::eLockFlags::IsDirtyUpdate);
		}
		context->Unmap(staging.Get(), 0);
		return locked ? buffer : fail();
	}
}
