/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// City-view draw diagnostics, written to the log with DrawStats.
//
// For a city that loads but never appears: the region view's last frame stays on screen, scrolling smears
// it, and the UI still draws. The reports here tell whether SC4 sends the city's draws at all, whether
// SCD3D11 drops them, and where the ones it issues go (viewport, scissor, targets, depth state).
//
// cSC43DRender::Draw (0x007CB530) runs RestoreBackingStore or a full static pass, then the dynamic pass.
// DrawHook reports SC4's backing-store state before it, DrawPostStaticViewHook reports each static pass,
// and DrawHook reports the whole call after it. The fields and globals below are SimCity 4 1.1.641's.

#include "cGDriver.h"
#include "D3D11Conversions.h"
#include "Diagnostics.h"
#include "GameVersion.h"

#include <cstdio>
#include <cstring>

namespace nSCD3D11 {
	namespace {
		// cSC43DRender's backing store, shared by every renderer (CreateBackingStore 0x007C27A0).
		constexpr uintptr_t kBackingStoreEnabled = 0x00B0DC19; // byte
		constexpr uintptr_t kBackingStoreColourRegion = 0x00B4E1E4; // region handle
		constexpr uintptr_t kBackingStoreDepthRegion = 0x00B4E1E8; // region handle
		constexpr uintptr_t kBackingStoreExtension = 0x00B4E1F0; // cIGZGBufferRegionExtension*

		struct RenderState {
			bool globals = false;
			uint8_t storeEnabled = 0;
			uint32_t colourRegion = 0;
			uint32_t depthRegion = 0;
			void *extension = nullptr;
			// cSC43DRender +0x60..+0x65: +0x63 CanDoPartialRegionWrites, +0x64 partial saves allowed,
			// +0x65 mbBackingStoreValid.
			uint8_t flags[6]{};
			int32_t viewWidth = 0; // +0x68
			int32_t viewHeight = 0; // +0x6c
			int32_t scrollX = 0; // +0xE0, pending translation
			int32_t scrollY = 0; // +0xE4
			uint8_t inDraw = 0; // +0x108, Draw's re-entrancy guard
			uint8_t fullRedraw = 0; // +0x109
			bool camera = false;
			int32_t zoom = 0; // cSC4CameraControl +0x108
			int32_t rotation = 0; // +0x10c
			float yaw = 0.0f; // +0x118, written by camera plugins
			float pitch = 0.0f; // +0x11c
		};

		bool HasGameGlobals() {
			static bool const known = IsGameVersion641();
			return known;
		}

		RenderState ReadRenderState(void *render) {
			RenderState state;
			auto const *const bytes = static_cast<uint8_t const *>(render);
			if (HasGameGlobals()) {
				state.globals = true;
				state.storeEnabled = *reinterpret_cast<uint8_t const *>(kBackingStoreEnabled);
				state.colourRegion = *reinterpret_cast<uint32_t const *>(kBackingStoreColourRegion);
				state.depthRegion = *reinterpret_cast<uint32_t const *>(kBackingStoreDepthRegion);
				state.extension = *reinterpret_cast<void *const *>(kBackingStoreExtension);
			}
			std::memcpy(state.flags, bytes + 0x60, sizeof(state.flags));
			std::memcpy(&state.viewWidth, bytes + 0x68, sizeof(int32_t));
			std::memcpy(&state.viewHeight, bytes + 0x6C, sizeof(int32_t));
			std::memcpy(&state.scrollX, bytes + 0xE0, sizeof(int32_t));
			std::memcpy(&state.scrollY, bytes + 0xE4, sizeof(int32_t));
			state.inDraw = bytes[0x108];
			state.fullRedraw = bytes[0x109];
			uint8_t const *camera = nullptr;
			std::memcpy(&camera, bytes + 0x8C, sizeof(camera));
			if (camera != nullptr) {
				state.camera = true;
				std::memcpy(&state.zoom, camera + 0x108, sizeof(int32_t));
				std::memcpy(&state.rotation, camera + 0x10C, sizeof(int32_t));
				std::memcpy(&state.yaw, camera + 0x118, sizeof(float));
				std::memcpy(&state.pitch, camera + 0x11C, sizeof(float));
			}
			return state;
		}

		// What decides how Draw runs. The pending scroll and the free camera angles change all the time
		// and are only printed.
		bool SameDecisions(RenderState const &a, RenderState const &b) {
			return a.storeEnabled == b.storeEnabled && a.colourRegion == b.colourRegion &&
			       a.depthRegion == b.depthRegion && a.extension == b.extension &&
			       std::memcmp(a.flags, b.flags, sizeof(a.flags)) == 0 && a.viewWidth == b.viewWidth &&
			       a.viewHeight == b.viewHeight && a.inDraw == b.inDraw && a.zoom == b.zoom &&
			       a.rotation == b.rotation;
		}

		void LogRenderState(char const *heading, RenderState const &state, void const *driverExtension,
		                    int32_t windowWidth, int32_t windowHeight) {
			Log(LogCategory::Diag,
			    "%s: backing store enabled %u valid %u, regions colour %u depth %u, extension %p (%s), "
			    "partial writes %u saves %u, flags +60..62 %u/%u/%u, view %dx%d (window %dx%d), pending scroll %d,%d, "
			    "in draw %u, full redraw %u, camera %s zoom %d rotation %d yaw %.3f pitch %.3f%s",
			    heading, state.storeEnabled, state.flags[5], state.colourRegion, state.depthRegion, state.extension,
			    state.extension == driverExtension ? "SCD3D11" : state.extension == nullptr ? "none" : "another driver",
			    state.flags[3], state.flags[4], state.flags[0], state.flags[1], state.flags[2], state.viewWidth,
			    state.viewHeight, windowWidth, windowHeight, state.scrollX, state.scrollY, state.inDraw,
			    state.fullRedraw, state.camera ? "yes" : "none", state.zoom, state.rotation, state.yaw, state.pitch,
			    state.globals ? "" : " (not SimCity 4 1.1.641: globals unread)");
		}
	} // namespace

	void cGDriver::DiagnoseDrawBegin(void *render) {
		drawDiagnosticsAtDraw = drawDiagnostics;
		drawDiagnosticsMark = drawDiagnostics;
		insideCityDraw = true;
		if (!DrawStatsEnabled() || render == nullptr) return;
		++diagnosedDraws;
		if (diagnosedDraws == 1) DiagnoseWindowGeometry("at the first city-view Draw");

		static RenderState last;
		static bool haveLast = false;
		RenderState const state = ReadRenderState(render);
		bool const changed = !haveLast || !SameDecisions(state, last);
		last = state;
		haveLast = true;
		// The first few, then whenever something that decides the path changes.
		if (diagnosedDraws > 3 && !changed) return;
		char heading[64]{};
		sprintf_s(heading, "Draw #%u begins%s", diagnosedDraws, diagnosedDraws > 3 ? " (state changed)" : "");
		LogRenderState(heading, state, static_cast<cIGZGBufferRegionExtension *>(this), windowWidth, windowHeight);
	}

	void cGDriver::DiagnoseStaticPass(void *render) {
		DrawDiagnosticCounts const from = drawDiagnosticsMark;
		DrawDiagnosticCounts const &now = drawDiagnostics;
		drawDiagnosticsMark = drawDiagnostics;
		if (!DrawStatsEnabled() || render == nullptr) return;
		++diagnosedStaticPasses;

		uint64_t const received = now.received - from.received;
		uint64_t const culled = now.culled - from.culled;
		uint64_t const issued = now.issued - from.issued;
		uint64_t const bindFailures = now.bindFailures - from.bindFailures;
		int64_t const dropped = static_cast<int64_t>(received) - static_cast<int64_t>(culled + issued + bindFailures);
		// A pass that got draws and sent none of them on is the city that never appears.
		bool const nothingIssued = received > 0 && issued == 0;
		static uint32_t loggedNothingIssued = 0;
		if (diagnosedStaticPasses > 60 && !(nothingIssued && loggedNothingIssued++ < 100)) return;

		// The pipeline as the pass left it.
		D3D11_VIEWPORT viewport{};
		D3D11_RECT scissor{};
		bool scissorTest = false;
		bool ourTarget = false;
		bool ourDepth = false;
		bool depthTest = false;
		bool depthWrite = false;
		unsigned depthFunction = 0;
		unsigned colourMask = 0;
		if (d3dContext) {
			UINT count = 1;
			d3dContext->RSGetViewports(&count, &viewport);
			count = 1;
			d3dContext->RSGetScissorRects(&count, &scissor);
			Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer;
			d3dContext->RSGetState(&rasterizer);
			if (rasterizer) {
				D3D11_RASTERIZER_DESC description{};
				rasterizer->GetDesc(&description);
				scissorTest = description.ScissorEnable != FALSE;
			}
			Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
			Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depth;
			d3dContext->OMGetRenderTargets(1, &target, &depth);
			ourTarget = target && target.Get() == renderTargetView.Get();
			ourDepth = depth && depth.Get() == depthStencilView.Get();
			Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthState;
			UINT reference = 0;
			d3dContext->OMGetDepthStencilState(&depthState, &reference);
			if (depthState) {
				D3D11_DEPTH_STENCIL_DESC description{};
				depthState->GetDesc(&description);
				depthTest = description.DepthEnable != FALSE;
				depthWrite = description.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO;
				depthFunction = static_cast<unsigned>(description.DepthFunc);
			}
			Microsoft::WRL::ComPtr<ID3D11BlendState> blend;
			float factor[4]{};
			UINT sampleMask = 0;
			d3dContext->OMGetBlendState(&blend, factor, &sampleMask);
			if (blend) {
				D3D11_BLEND_DESC description{};
				blend->GetDesc(&description);
				colourMask = description.RenderTarget[0].RenderTargetWriteMask;
			} else {
				colourMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			}
		}

		Log(LogCategory::Diag,
		    "static pass #%u (Draw #%u)%s: draws received %llu, culled %llu, issued %llu, bind failures %llu, "
		    "dropped before binding %lld; clears colour %llu depth %llu without a view %llu; viewports set %llu "
		    "refused %llu clamped %llu reset %llu; regions read %llu drawn %llu refused %llu; blits %llu; "
		    "SC4 viewport %d,%d %dx%d (%s); D3D viewport %.0f,%.0f %.0fx%.0f depth %.2f-%.2f, scissor %s %ld,%ld-%ld,%ld; "
		    "target %s, depth view %s; depth test %d func %u write %d; colour mask 0x%X; window %dx%d",
		    diagnosedStaticPasses, diagnosedDraws, nothingIssued ? " NOTHING ISSUED" : "",
		    static_cast<unsigned long long>(received), static_cast<unsigned long long>(culled),
		    static_cast<unsigned long long>(issued), static_cast<unsigned long long>(bindFailures),
		    static_cast<long long>(dropped), static_cast<unsigned long long>(now.colorClears - from.colorClears),
		    static_cast<unsigned long long>(now.depthClears - from.depthClears),
		    static_cast<unsigned long long>(now.clearsWithoutView - from.clearsWithoutView),
		    static_cast<unsigned long long>(now.viewports - from.viewports),
		    static_cast<unsigned long long>(now.viewportRejects - from.viewportRejects),
		    static_cast<unsigned long long>(now.viewportClamps - from.viewportClamps),
		    static_cast<unsigned long long>(now.viewportResets - from.viewportResets),
		    static_cast<unsigned long long>(now.regionReads - from.regionReads),
		    static_cast<unsigned long long>(now.regionDraws - from.regionDraws),
		    static_cast<unsigned long long>(now.regionRejects - from.regionRejects),
		    static_cast<unsigned long long>(now.blits - from.blits), viewportX, viewportY, viewportWidth,
		    viewportHeight, scissorEnabled ? "partial" : "full", viewport.TopLeftX, viewport.TopLeftY,
		    viewport.Width, viewport.Height, viewport.MinDepth, viewport.MaxDepth, scissorTest ? "on" : "off",
		    scissor.left, scissor.top, scissor.right, scissor.bottom, ourTarget ? "SCD3D11's" : "OTHER",
		    ourDepth ? "SCD3D11's" : "OTHER", depthTest ? 1 : 0, depthFunction, depthWrite ? 1 : 0, colourMask,
		    windowWidth, windowHeight);
	}

	void cGDriver::DiagnoseDrawEnd(void *render, bool drawn) {
		DrawDiagnosticCounts const &now = drawDiagnostics;
		DrawDiagnosticCounts const whole = drawDiagnosticsAtDraw;
		DrawDiagnosticCounts const tail = drawDiagnosticsMark;
		drawDiagnosticsMark = drawDiagnostics;
		insideCityDraw = false;
		if (!DrawStatsEnabled() || render == nullptr) return;

		uint64_t const received = now.received - whole.received;
		uint64_t const issued = now.issued - whole.issued;
		bool const nothingIssued = received > 0 && issued == 0;
		RenderState const state = ReadRenderState(render);
		// A Draw that only put the backing store back is the steady state and says nothing. One that drew,
		// saved the store or copied a scroll is reported, the first 40 of them, and every odd one after.
		bool const worked = received > 0 || now.regionReads != whole.regionReads ||
		                    now.viewports != whole.viewports || now.colorClears != whole.colorClears ||
		                    now.depthClears != whole.depthClears;
		static uint32_t loggedWorked = 0;
		static uint32_t loggedNothingIssued = 0;
		static uint32_t loggedInvalid = 0;
		bool const invalid = state.flags[5] == 0 && state.storeEnabled != 0;
		if (!(worked && loggedWorked++ < 40) && !(nothingIssued && loggedNothingIssued++ < 100) &&
		    !(invalid && loggedInvalid++ < 50)) {
			return;
		}
		Log(LogCategory::Diag,
		    "Draw #%u ends%s: returned %d, backing store valid %u; whole Draw: draws received %llu culled %llu issued "
		    "%llu bind failures %llu, clears colour %llu depth %llu, regions read %llu drawn %llu refused %llu, "
		    "viewports %llu refused %llu, blits %llu; after the last static pass: received %llu issued %llu",
		    diagnosedDraws, nothingIssued ? " NOTHING ISSUED" : "", drawn ? 1 : 0, state.flags[5],
		    static_cast<unsigned long long>(received), static_cast<unsigned long long>(now.culled - whole.culled),
		    static_cast<unsigned long long>(issued),
		    static_cast<unsigned long long>(now.bindFailures - whole.bindFailures),
		    static_cast<unsigned long long>(now.colorClears - whole.colorClears),
		    static_cast<unsigned long long>(now.depthClears - whole.depthClears),
		    static_cast<unsigned long long>(now.regionReads - whole.regionReads),
		    static_cast<unsigned long long>(now.regionDraws - whole.regionDraws),
		    static_cast<unsigned long long>(now.regionRejects - whole.regionRejects),
		    static_cast<unsigned long long>(now.viewports - whole.viewports),
		    static_cast<unsigned long long>(now.viewportRejects - whole.viewportRejects),
		    static_cast<unsigned long long>(now.blits - whole.blits),
		    static_cast<unsigned long long>(now.received - tail.received),
		    static_cast<unsigned long long>(now.issued - tail.issued));
	}

	void cGDriver::DiagnoseBindFailure(uint32_t primitive) {
		if (!DrawStatsEnabled()) return;
		static uint32_t logged = 0;
		if (logged >= 20) return;
		++logged;
		char const *reason = "a failed constant-buffer map or render-state update";
		if (D3D11Topology(primitive) == D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED) reason = "an unknown primitive";
		else if (deviceLost) reason = "a lost device";
		else if (!d3dDevice || !d3dContext) reason = "no device";
		else if (!swapChain) reason = "no swap chain";
		else if (!renderTargetView) reason = "no render target view";
		else if (!depthStencilView) reason = "no depth view";
		else if (!vertexShader || !pixelShader || !flatPixelShader || !inputLayout) reason = "a missing shader or input layout";
		else if (!stateConstantBuffer || !drawConstantBuffer || !dynamicVertexBuffer) reason = "a missing buffer";
		Log(LogCategory::Diag, "a draw with primitive %u was skipped: %s", primitive, reason);
	}

	void cGDriver::DiagnoseWindowGeometry(char const *when) {
		if (!DrawStatsEnabled()) return;
		HWND const window = static_cast<HWND>(windowHandle);
		RECT client{};
		RECT frame{};
		RECT monitor{};
		if (window != nullptr) {
			GetClientRect(window, &client);
			GetWindowRect(window, &frame);
			MONITORINFO info{sizeof(info)};
			if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info)) monitor = info.rcMonitor;
		}
		// GetDpiForWindow is Windows 10 1607 and later.
		using DpiForWindow = UINT(WINAPI *)(HWND);
		static auto const dpiForWindow = reinterpret_cast<DpiForWindow>(
			reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
		UINT const dpi = window != nullptr && dpiForWindow != nullptr ? dpiForWindow(window) : 0;
		Log(LogCategory::Diag,
		    "window %s: driver %dx%d, client %ldx%ld, window %ld,%ld-%ld,%ld, monitor %ld,%ld-%ld,%ld, "
		    "screen metrics %dx%d, DPI %u, DPI aware %d", when, windowWidth, windowHeight, client.right - client.left,
		    client.bottom - client.top, frame.left, frame.top, frame.right, frame.bottom, monitor.left, monitor.top,
		    monitor.right, monitor.bottom, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), dpi,
		    IsProcessDPIAware() ? 1 : 0);
	}
} // namespace nSCD3D11
