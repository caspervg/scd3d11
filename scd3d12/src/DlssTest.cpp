/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#include "DlssTest.h"

#include <windows.h>
#include <d3dcompiler.h>
#include <dcomp.h>
#include <dxgi1_6.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace nSCD3D12 {
	namespace {
		constexpr uint32_t kSlots = 3;

		char const kDownsampleShader[] = R"(
cbuffer Constants : register(b0)
{
	float2 jitter;
	float2 scale;
	float2 inverseFull;
	float2 fullSize;
};

Texture2D<float4> color : register(t0);
Texture2D<float> depth : register(t1);
RWTexture2D<float4> colorOut : register(u0);
RWTexture2D<float> depthOut : register(u1);
SamplerState linearClamp : register(s0);

[numthreads(8, 8, 1)]
void Main(uint3 id : SV_DispatchThreadID)
{
	uint width, height;
	colorOut.GetDimensions(width, height);
	if (id.x >= width || id.y >= height) return;
	float2 position = (float2(id.xy) + 0.5f + jitter) * scale;
	colorOut[id.xy] = color.SampleLevel(linearClamp, position * inverseFull, 0);
	depthOut[id.xy] = depth.Load(int3(min(int2(position), int2(fullSize) - 1), 0));
}
)";

		void Barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
		             D3D12_RESOURCE_STATES after) {
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
			list->ResourceBarrier(1, &barrier);
		}

		// The tap of Alt that brings the window to the foreground would otherwise open its window menu, which then
		// waits for keys and stops the replay.
		LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
			if (message == WM_SYSCOMMAND && (wParam & 0xFFF0) == SC_KEYMENU) return 0;
			return DefWindowProcW(window, message, wParam, lParam);
		}
	}

	bool ParseReplayWindowKind(wchar_t const *text, ReplayWindow::Kind &kind) {
		std::wstring const value = text;
		if (value == L"window") kind = ReplayWindow::Kind::Window;
		else if (value == L"composition") kind = ReplayWindow::Kind::Composition;
		else if (value == L"surface") kind = ReplayWindow::Kind::Surface;
		else return false;
		return true;
	}

	// Downsampler

	struct Downsampler::Impl {
		ID3D12Device *device;
		ComPtr<ID3D12RootSignature> rootSignature;
		ComPtr<ID3D12PipelineState> pipeline;
		ComPtr<ID3D12DescriptorHeap> heap;
		uint32_t increment = 0;
		uint32_t slot = 0;
		ComPtr<ID3D12Resource> color, depth;
		// Targets replaced by a resize are kept until the end, since the GPU may still read them and resizes are rare.
		std::vector<ComPtr<ID3D12Resource> > retired;
		uint32_t width = 0, height = 0;
		bool ready = false;

		explicit Impl(ID3D12Device *device) : device(device) { ready = Create(); }

		bool Create() {
			D3D12_DESCRIPTOR_RANGE const ranges[]{
				{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0},
				{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 2},
			};
			D3D12_ROOT_PARAMETER parameters[2]{};
			parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			parameters[0].Constants = {0, 0, 8};
			parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			parameters[1].DescriptorTable = {2, ranges};
			D3D12_STATIC_SAMPLER_DESC sampler{};
			sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			sampler.MaxLOD = D3D12_FLOAT32_MAX;
			D3D12_ROOT_SIGNATURE_DESC const signature{2, parameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
			ComPtr<ID3DBlob> serialized, errors, code;
			if (FAILED(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)) ||
			    FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
			                                       IID_PPV_ARGS(&rootSignature)))) return false;
			if (FAILED(D3DCompile(kDownsampleShader, sizeof(kDownsampleShader) - 1, "Downsample", nullptr, nullptr,
			                      "Main", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors))) {
				if (errors) std::printf("%s\n", static_cast<char const *>(errors->GetBufferPointer()));
				return false;
			}
			D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDescription{};
			pipelineDescription.pRootSignature = rootSignature.Get();
			pipelineDescription.CS = {code->GetBufferPointer(), code->GetBufferSize()};
			if (FAILED(device->CreateComputePipelineState(&pipelineDescription, IID_PPV_ARGS(&pipeline)))) return false;
			D3D12_DESCRIPTOR_HEAP_DESC const heapDescription{
				D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSlots * 4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
			};
			if (FAILED(device->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&heap)))) return false;
			increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			return true;
		}

		bool Targets(uint32_t w, uint32_t h) {
			if (color && width == w && height == h) return true;
			if (color) retired.push_back(std::move(color));
			if (depth) retired.push_back(std::move(depth));
			D3D12_HEAP_PROPERTIES const heapProperties{D3D12_HEAP_TYPE_DEFAULT};
			D3D12_RESOURCE_DESC description{};
			description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			description.Width = w;
			description.Height = h;
			description.DepthOrArraySize = description.MipLevels = 1;
			description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			description.SampleDesc.Count = 1;
			description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
			if (FAILED(device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &description,
			                                           D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
			                                           IID_PPV_ARGS(&color)))) return false;
			description.Format = DXGI_FORMAT_R32_FLOAT;
			description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
			if (FAILED(device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &description,
			                                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
			                                           IID_PPV_ARGS(&depth)))) return false;
			width = w;
			height = h;
			return true;
		}
	};

	Downsampler::Downsampler(ID3D12Device *device) : impl(std::make_unique<Impl>(device)) {}
	Downsampler::~Downsampler() = default;
	ID3D12Resource *Downsampler::Color() const { return impl->color.Get(); }
	ID3D12Resource *Downsampler::Depth() const { return impl->depth.Get(); }

	bool Downsampler::Resize(uint32_t width, uint32_t height) { return impl->ready && impl->Targets(width, height); }

	void Downsampler::Record(ID3D12GraphicsCommandList *list, ID3D12Resource *color, ID3D12Resource *depth,
	                         float const jitter[2]) {
		Impl &d = *impl;
		uint32_t const width = d.width, height = d.height;
		D3D12_RESOURCE_DESC const full = color->GetDesc();
		d.slot = (d.slot + 1) % kSlots;
		D3D12_CPU_DESCRIPTOR_HANDLE cpu = d.heap->GetCPUDescriptorHandleForHeapStart();
		cpu.ptr += static_cast<SIZE_T>(d.slot) * 4 * d.increment;
		D3D12_SHADER_RESOURCE_VIEW_DESC view{};
		view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		view.Texture2D.MipLevels = 1;
		d.device->CreateShaderResourceView(color, &view, cpu);
		cpu.ptr += d.increment;
		view.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
		d.device->CreateShaderResourceView(depth, &view, cpu);
		cpu.ptr += d.increment;
		D3D12_UNORDERED_ACCESS_VIEW_DESC access{};
		access.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		access.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		d.device->CreateUnorderedAccessView(d.color.Get(), nullptr, &access, cpu);
		cpu.ptr += d.increment;
		access.Format = DXGI_FORMAT_R32_FLOAT;
		d.device->CreateUnorderedAccessView(d.depth.Get(), nullptr, &access, cpu);
		D3D12_GPU_DESCRIPTOR_HANDLE gpu = d.heap->GetGPUDescriptorHandleForHeapStart();
		gpu.ptr += static_cast<UINT64>(d.slot) * 4 * d.increment;

		float const constants[8]{
			jitter[0], jitter[1],
			static_cast<float>(full.Width) / static_cast<float>(width), static_cast<float>(full.Height) / static_cast<float>(height),
			1.0f / static_cast<float>(full.Width), 1.0f / static_cast<float>(full.Height),
			static_cast<float>(full.Width), static_cast<float>(full.Height),
		};
		Barrier(list, color, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		Barrier(list, depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		Barrier(list, d.color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		Barrier(list, d.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		ID3D12DescriptorHeap *const heaps[]{d.heap.Get()};
		list->SetDescriptorHeaps(1, heaps);
		list->SetComputeRootSignature(d.rootSignature.Get());
		list->SetComputeRoot32BitConstants(0, 8, constants, 0);
		list->SetComputeRootDescriptorTable(1, gpu);
		list->SetPipelineState(d.pipeline.Get());
		list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
		Barrier(list, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
		Barrier(list, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		Barrier(list, d.color.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RENDER_TARGET);
		Barrier(list, d.depth.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	}

	// ReplayWindow

	struct ReplayWindow::Impl {
		Kind kind;
		// Another process's window to present into, or null for one of our own.
		HWND into;
		ID3D12Device *device;
		// The driver's queue, and the one presents go through (Streamline's with frame generation).
		ID3D12CommandQueue *queue;
		ID3D12CommandQueue *presentQueue = nullptr;
		Dlss *dlss;
		bool generation = false;
		HWND window = nullptr;
		ComPtr<IDXGISwapChain3> swapChain;
		ComPtr<IDCompositionDesktopDevice> composition;
		ComPtr<IDCompositionTarget> target;
		ComPtr<IDCompositionVisual2> visual;
		ComPtr<IUnknown> surface;
		uint32_t width = 0, height = 0;
		bool failed = false;
		bool generating = false;

		ComPtr<ID3D12CommandAllocator> allocators[kSlots];
		ComPtr<ID3D12GraphicsCommandList> list;
		ComPtr<ID3D12Fence> drawn, presented;
		HANDLE event = nullptr;
		uint64_t frame = 0;
		uint64_t slotValue[kSlots]{};

		// Once a second: frames drawn, frames frame generation says it presented, the swap chain's present count.
		std::chrono::steady_clock::time_point reportStart{};
		uint64_t reportFrames = 0, reportGenerated = 0;
		UINT reportPresents = 0;
		uint32_t lastStatus = UINT32_MAX;
		std::vector<double> drawnRates, presentedRates, countedRates;

		Impl(Kind kind, HWND into, ID3D12Device *device, ID3D12CommandQueue *queue, Dlss *dlss)
			: kind(kind), into(into), device(device), queue(queue), dlss(dlss) {
			generation = dlss != nullptr && dlss->FrameGeneration();
		}

		~Impl() {
			if (presented) Wait(frame);
			if (generating) dlss->EnableFrameGeneration(false);
			swapChain.Reset();
			if (window != nullptr && window != into) DestroyWindow(window);
			if (event != nullptr) CloseHandle(event);
		}

		void Wait(uint64_t value) {
			if (presented->GetCompletedValue() >= value) return;
			presented->SetEventOnCompletion(value, event);
			WaitForSingleObject(event, INFINITE);
		}

		bool CreateWindowOnce() {
			if (window != nullptr) return true;
			if (into != nullptr) {
				window = into;
			} else {
				WNDCLASSW windowClass{};
				windowClass.lpfnWndProc = WindowProc;
				windowClass.hInstance = GetModuleHandleW(nullptr);
				windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
				windowClass.lpszClassName = L"SCD3D12Replay";
				RegisterClassW(&windowClass);
				RECT rect{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
				AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
				wchar_t const *const titles[]{L"SCD3D12 replay: window", L"SCD3D12 replay: composition",
				                              L"SCD3D12 replay: composition surface"};
				window = CreateWindowExW(0, windowClass.lpszClassName, titles[static_cast<int>(kind)],
				                         WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
				                         rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
				                         windowClass.hInstance, nullptr);
			}
			if (window == nullptr) return false;
			// Frame generation only generates frames for the window in the foreground, so the window takes it. Windows
			// lets a process do that right after a key press, hence the tap of Alt.
			INPUT alt[2]{};
			alt[0].type = alt[1].type = INPUT_KEYBOARD;
			alt[0].ki.wVk = alt[1].ki.wVk = VK_MENU;
			alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
			SendInput(2, alt, sizeof(INPUT));
			SetForegroundWindow(GetAncestor(window, GA_ROOT));
			for (auto &allocator: allocators) {
				if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))) return false;
			}
			if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0].Get(), nullptr,
			                                     IID_PPV_ARGS(&list)))) return false;
			list->Close();
			if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&drawn))) ||
			    FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&presented)))) return false;
			event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			return event != nullptr;
		}

		bool Check(HRESULT result, char const *what) {
			if (SUCCEEDED(result)) return true;
			std::printf("window: %s failed: 0x%08lX\n", what, static_cast<unsigned long>(result));
			std::fflush(stdout);
			return false;
		}

		bool CreateSwapChain() {
			presentQueue = generation ? dlss->PresentQueue() : queue;
			ID3D12CommandQueue *const nativeQueue = generation ? dlss->NativePresentQueue() : queue;
			if (presentQueue == nullptr || nativeQueue == nullptr) return false;
			DXGI_SWAP_CHAIN_DESC1 description{};
			description.Width = width;
			description.Height = height;
			description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			description.SampleDesc.Count = 1;
			description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			description.BufferCount = 2;
			description.Scaling = DXGI_SCALING_STRETCH;
			description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
			description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
			IDXGIFactory2 *factory = nullptr;
			if (!Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2")) return false;
			IDXGISwapChain1 *created = nullptr;
			HANDLE surfaceHandle = nullptr;
			bool ok = true;
			if (kind == Kind::Window) {
				// Frame generation sets itself up while Streamline's factory makes the swap chain. One made first and
				// wrapped by Streamline afterwards never gets a generated frame.
				if (generation) ok = dlss->Upgrade(factory);
				ok = ok && Check(factory->CreateSwapChainForHwnd(presentQueue, window, &description, nullptr, nullptr,
				                                                 &created), "CreateSwapChainForHwnd");
			} else {
				ok = Check(DCompositionCreateDevice2(nullptr, IID_PPV_ARGS(&composition)), "DCompositionCreateDevice2") &&
				     Check(composition->CreateTargetForHwnd(window, TRUE, &target), "CreateTargetForHwnd") &&
				     Check(composition->CreateVisual(&visual), "CreateVisual") &&
				     Check(target->SetRoot(visual.Get()), "SetRoot");
				if (ok && kind == Kind::Composition) {
					ok = Check(factory->CreateSwapChainForComposition(nativeQueue, &description, nullptr, &created),
					           "CreateSwapChainForComposition") &&
					     Check(visual->SetContent(created), "SetContent");
				} else if (ok) {
					ComPtr<IDXGIFactoryMedia> media;
					ok = Check(DCompositionCreateSurfaceHandle(COMPOSITIONOBJECT_ALL_ACCESS, nullptr, &surfaceHandle),
					           "DCompositionCreateSurfaceHandle") &&
					     Check(factory->QueryInterface(IID_PPV_ARGS(&media)), "IDXGIFactoryMedia") &&
					     Check(media->CreateSwapChainForCompositionSurfaceHandle(nativeQueue, surfaceHandle, &description,
					                                                             nullptr, &created),
					           "CreateSwapChainForCompositionSurfaceHandle") &&
					     Check(composition->CreateSurfaceFromHandle(surfaceHandle, &surface), "CreateSurfaceFromHandle") &&
					     Check(visual->SetContent(surface.Get()), "SetContent");
				}
				ok = ok && Check(composition->Commit(), "Commit");
				// Streamline's factory passes composition swap chains straight to DXGI, without the hook frame
				// generation sets itself up in, and has no IDXGIFactoryMedia. So they are made directly and wrapped
				// afterwards, and frame generation never generates a frame on them.
				if (ok && generation) ok = dlss->Upgrade(created);
			}
			if (surfaceHandle != nullptr) CloseHandle(surfaceHandle);
			factory->Release();
			if (ok) ok = Check(created->QueryInterface(IID_PPV_ARGS(&swapChain)), "IDXGISwapChain3");
			if (created != nullptr) created->Release();
			if (!ok) return false;
			std::printf("window: presenting %ux%u through %s%s\n", width, height,
			            kind == Kind::Window ? "a window swap chain" :
			            kind == Kind::Composition ? "a composition swap chain" : "a composition surface swap chain",
			            generation ? " with frame generation" : "");
			std::fflush(stdout);
			return true;
		}

		void Report(bool force) {
			auto const now = std::chrono::steady_clock::now();
			if (reportStart.time_since_epoch().count() == 0) reportStart = now;
			double const seconds = std::chrono::duration<double>(now - reportStart).count();
			if (seconds < 1.0 && !force) return;
			if (seconds <= 0.0) return;
			UINT presents = 0;
			swapChain->GetLastPresentCount(&presents);
			double const drawnRate = static_cast<double>(reportFrames) / seconds;
			double const presentedRate = static_cast<double>(reportGenerated) / seconds;
			double const countedRate = static_cast<double>(presents - reportPresents) / seconds;
			DXGI_FRAME_STATISTICS statistics{};
			HRESULT const statisticsResult = swapChain->GetFrameStatistics(&statistics);
			std::printf("window: %.1f frames drawn per second, %.1f presented by frame generation, %.1f presents counted "
			            "by DXGI (refresh count %u, 0x%08lX)\n", drawnRate, presentedRate, countedRate,
			            statistics.PresentRefreshCount, static_cast<unsigned long>(statisticsResult));
			std::fflush(stdout);
			drawnRates.push_back(drawnRate);
			presentedRates.push_back(presentedRate);
			countedRates.push_back(countedRate);
			reportStart = now;
			reportFrames = reportGenerated = 0;
			reportPresents = presents;
		}
	};

	ReplayWindow::ReplayWindow(Kind kind, HWND into, ID3D12Device *device, ID3D12CommandQueue *queue, Dlss *dlss)
		: impl(std::make_unique<Impl>(kind, into, device, queue, dlss)) {}
	ReplayWindow::~ReplayWindow() = default;

	bool ReplayWindow::Present(ID3D12Resource *image, bool generate) {
		Impl &d = *impl;
		if (d.failed) return false;
		D3D12_RESOURCE_DESC const description = image->GetDesc();
		if (!d.swapChain || d.width != description.Width || d.height != description.Height) {
			if (d.generating) d.dlss->EnableFrameGeneration(false);
			d.generating = false;
			if (d.presented) d.Wait(d.frame);
			d.swapChain.Reset();
			d.surface.Reset();
			d.visual.Reset();
			d.target.Reset();
			d.composition.Reset();
			d.width = static_cast<uint32_t>(description.Width);
			d.height = description.Height;
			if (d.window != nullptr && d.window != d.into) {
				RECT rect{0, 0, static_cast<LONG>(d.width), static_cast<LONG>(d.height)};
				AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
				SetWindowPos(d.window, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
				             SWP_NOMOVE | SWP_NOZORDER);
			}
			if (!d.CreateWindowOnce() || !d.CreateSwapChain()) {
				d.failed = true;
				return false;
			}
		}
		MSG message;
		while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}
		if (!IsWindow(d.window)) return false;
		if (d.generation && generate != d.generating) {
			d.dlss->EnableFrameGeneration(generate);
			d.generating = generate;
			std::printf("window: frame generation %s at frame %llu, window in the foreground: %s\n", generate ? "on" : "off",
			            static_cast<unsigned long long>(d.frame), GetForegroundWindow() == GetAncestor(d.window, GA_ROOT) ? "yes" : "no");
		}

		uint64_t const value = ++d.frame;
		uint32_t const slot = static_cast<uint32_t>(value % kSlots);
		d.Wait(d.slotValue[slot]);
		bool const separate = d.presentQueue != d.queue;
		if (separate) {
			d.queue->Signal(d.drawn.Get(), value);
			d.presentQueue->Wait(d.drawn.Get(), value);
		}
		ComPtr<ID3D12Resource> buffer;
		if (FAILED(d.swapChain->GetBuffer(d.swapChain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&buffer)))) {
			d.failed = true;
			return false;
		}
		d.allocators[slot]->Reset();
		d.list->Reset(d.allocators[slot].Get(), nullptr);
		Barrier(d.list.Get(), image, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
		Barrier(d.list.Get(), buffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
		d.list->CopyResource(buffer.Get(), image);
		Barrier(d.list.Get(), image, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
		Barrier(d.list.Get(), buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
		d.list->Close();
		ID3D12CommandList *const lists[]{d.list.Get()};
		d.presentQueue->ExecuteCommandLists(1, lists);
		if (d.dlss != nullptr) d.dlss->PresentStart();
		HRESULT const result = d.swapChain->Present(0, 0);
		if (d.dlss != nullptr) d.dlss->PresentEnd();
		d.presentQueue->Signal(d.presented.Get(), value);
		d.slotValue[slot] = value;
		// The driver draws the next frame into what was just copied and tagged only once this present is done.
		if (separate) d.queue->Wait(d.presented.Get(), value);
		if (FAILED(result)) {
			d.Check(result, "Present");
			d.failed = true;
			return false;
		}

		++d.reportFrames;
		if (d.generation) {
			uint32_t generated = 0, status = 0;
			d.dlss->FrameGenerationState(generated, status);
			d.reportGenerated += generated;
			if (status != d.lastStatus) {
				std::printf("window: frame generation status 0x%X\n", status);
				d.lastStatus = status;
			}
		}
		d.Report(false);
		return true;
	}

	void ReplayWindow::PrintTotals() const {
		Impl const &d = *impl;
		auto const average = [](std::vector<double> const &values) {
			// The first and last seconds are partial or loading.
			if (values.size() < 3) return 0.0;
			double total = 0.0;
			for (size_t i = 1; i + 1 < values.size(); ++i) total += values[i];
			return total / static_cast<double>(values.size() - 2);
		};
		std::printf("window: on average %.1f frames drawn, %.1f presented by frame generation and %.1f presents "
		            "counted by DXGI per second, over %zu seconds\n", average(d.drawnRates), average(d.presentedRates),
		            average(d.countedRates), d.drawnRates.size());
	}
}
