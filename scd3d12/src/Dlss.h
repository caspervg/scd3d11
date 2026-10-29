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

#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace nSCD3D12 {
	// What the upscaler and frame generation need from a frame. Whoever draws the city fills it: the retained
	// scene for now, the server's own view later.
	struct UpscalerInputs {
		// The frame as drawn, at the render size, in the RENDER_TARGET state.
		ID3D12Resource *color = nullptr;
		// Its depth at the render size, D24S8 or R32F, 0 near, in depthState.
		ID3D12Resource *depth = nullptr;
		D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
		// RG16F or RG32F: each pixel's position in the previous frame minus its position now, in pixels of the
		// texture's own size (the render size, or the output size), y down. RENDER_TARGET.
		ID3D12Resource *motion = nullptr;
		// The city before the UI at the output size, RENDER_TARGET. Null when there is none.
		ID3D12Resource *hudless = nullptr;
		uint32_t renderWidth = 0, renderHeight = 0;
		uint32_t outputWidth = 0, outputHeight = 0;
		// Where each pixel's sample lay from its centre when the frame was drawn, in render pixels, x right, y down.
		float jitter[2]{};
		// The camera as OpenGL matrices (column major): projection and world to view, now and in the previous
		// frame. Without jitter.
		float projection[16]{};
		float view[16]{};
		float previousProjection[16]{};
		float previousView[16]{};
		// Nothing carries over from the previous frame: a new view, a new video mode, the first frame.
		bool reset = true;
	};

	enum class DlssMode : uint8_t { Off, Dlaa, Quality, Balanced, Performance, UltraPerformance };

	// NVIDIA DLSS through Streamline: DLAA and super resolution, and frame generation on a swap chain of ours.
	//
	// Streamline is loaded from the folder this program is in (its DLLs are copied next to it at build time),
	// with manual hooking: the driver keeps its own device and queue and Streamline only sees what is handed to
	// it. Start before the device exists, SetDevice right after.
	class Dlss {
	public:
		// Null, with the reason printed, when the DLLs, the driver or the GPU cannot do what was asked.
		static std::unique_ptr<Dlss> Start(DlssMode mode, bool frameGeneration);
		~Dlss();

		// After the device exists. False when DLSS (or frame generation) is not supported on its adapter.
		bool SetDevice(ID3D12Device *device, ID3D12CommandQueue *queue);
		DlssMode Mode() const;
		bool FrameGeneration() const;
		// The render size DLSS wants for an output size (the same size for DLAA).
		void RenderSize(uint32_t outputWidth, uint32_t outputHeight, uint32_t &renderWidth, uint32_t &renderHeight);
		// The jitter to draw the next frame with, in render pixels, as UpscalerInputs::jitter has it.
		void NextJitter(float &x, float &y);

		// Starts a frame: a new frame token and the latency markers before its draws.
		void BeginFrame(uint64_t frame);
		// Hands Streamline the frame's inputs and constants, and upscales when DLSS is on. Records on a command
		// list of its own and runs it on the driver's queue, after what prepare records on it. Returns the
		// upscaled frame (RENDER_TARGET, output size), or the colour input when DLSS is off.
		ID3D12Resource *Evaluate(UpscalerInputs const &inputs,
		                         std::function<void(ID3D12GraphicsCommandList *)> const &prepare = {});
		// The GPU time of the DLSS pass per frame since the last call, and how many frames.
		void TakeGpuTime(double &milliseconds, uint32_t &frames);

		// For presenting: a command queue made through Streamline's device, which frame generation needs to see.
		ID3D12CommandQueue *PresentQueue();
		// The same queue as DXGI's own factories need it, without Streamline's wrapper.
		ID3D12CommandQueue *NativePresentQueue();
		// Swaps a swap chain for Streamline's, so frame generation can attach. Factories as well.
		bool Upgrade(IDXGISwapChain1 *&swapChain);
		bool Upgrade(IDXGIFactory2 *&factory);
		// Around a Present on the upgraded swap chain.
		void PresentStart();
		void PresentEnd();
		// Frame generation on or off, for resizes.
		void EnableFrameGeneration(bool on);
		// Frames frame generation presented since the last call, and its status flags.
		void FrameGenerationState(uint32_t &presented, uint32_t &status);
		// Caps the frame rate through Reflex (0 for none). BeginFrame then sleeps until the frame should start.
		// Only with frame generation, which loads Reflex.
		void LimitFrameRate(uint32_t framesPerSecond);

	private:
		Dlss();
		struct Impl;
		std::unique_ptr<Impl> impl;
	};

	// --dlss off|dlaa|quality|balanced|performance|ultra. False for anything else.
	bool ParseDlssMode(std::wstring const &text, DlssMode &mode);
}
