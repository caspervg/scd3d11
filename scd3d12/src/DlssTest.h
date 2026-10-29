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

#include "Dlss.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>

namespace nSCD3D12 {
	// Replay aids for trying DLSS before the server draws the city at a render size of its own.

	// A render size frame made from a full size one: each render pixel takes the full frame's colour (bilinear) and
	// depth (nearest) at its centre plus the jitter. That is what drawing at the render size with that jitter would
	// give, apart from the slight blur of the bilinear sample and the depth being up to half a full pixel off.
	class Downsampler {
	public:
		explicit Downsampler(ID3D12Device *device);
		~Downsampler();
		// Makes the render size targets. False when that or the pass failed.
		bool Resize(uint32_t width, uint32_t height);
		// Records the pass. color is in RENDER_TARGET and depth (D24S8) in DEPTH_WRITE, and both are left so.
		// Afterwards Color() is in RENDER_TARGET and Depth() (R32F) in NON_PIXEL_SHADER_RESOURCE.
		void Record(ID3D12GraphicsCommandList *list, ID3D12Resource *color, ID3D12Resource *depth, float const jitter[2]);
		ID3D12Resource *Color() const;
		ID3D12Resource *Depth() const;

	private:
		struct Impl;
		std::unique_ptr<Impl> impl;
	};

	// Shows the replay in a window of its own, through one of three kinds of flip model swap chain, with frame
	// generation when Dlss has it: whether it works on the swap chain SCD3D11's composition mode uses is what
	// this is for.
	class ReplayWindow {
	public:
		enum class Kind : uint8_t {
			// CreateSwapChainForHwnd, made through Streamline's factory.
			Window,
			// CreateSwapChainForComposition, shown by a DirectComposition visual on the window.
			Composition,
			// IDXGIFactoryMedia::CreateSwapChainForCompositionSurfaceHandle on a DirectComposition surface the
			// visual shows, as ServerPresent=composition does with SC4's window.
			Surface,
		};

		// into is another process's window to present into (standing in for SC4's), or null for a window of our
		// own. queue is the driver's. dlss may be null.
		ReplayWindow(Kind kind, HWND into, ID3D12Device *device, ID3D12CommandQueue *queue, Dlss *dlss);
		~ReplayWindow();
		// Copies image (RENDER_TARGET) to the window and presents it, with frame generation when generate is set.
		// False once the window is closed or the swap chain failed.
		bool Present(ID3D12Resource *image, bool generate);
		void PrintTotals() const;

	private:
		struct Impl;
		std::unique_ptr<Impl> impl;
	};

	bool ParseReplayWindowKind(wchar_t const *text, ReplayWindow::Kind &kind);
}
