/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#include "cGDriver12.h"

#include "cGDCombiner.h"
#include "VertexFormatUtils.h"

#include <cstring>

using Microsoft::WRL::ComPtr;

// The SDK declares these destructors pure. SCD3D11 defines them in cGDriver.cpp, the server here.
cIGZGBufferRegionExtension::~cIGZGBufferRegionExtension() {}
cIGZGDriverVertexBufferExtension::~cIGZGDriverVertexBufferExtension() {}

namespace nSCD3D12 {
	struct cGDriver12::Impl {
		ComPtr<IDXGIFactory6> factory;
		ComPtr<ID3D12Device> device;
		ComPtr<ID3D12CommandQueue> queue;
		ComPtr<ID3D12CommandAllocator> allocator;
		ComPtr<ID3D12GraphicsCommandList> list;
		ComPtr<ID3D12Fence> fence;
		HANDLE fenceEvent = nullptr;
		uint64_t fenceValue = 0;

		ComPtr<ID3D12DescriptorHeap> rtvHeap;
		ComPtr<ID3D12Resource> backBuffer;
		uint32_t width = 0;
		uint32_t height = 0;
		float clearColor[4]{};

		~Impl() {
			if (fenceEvent != nullptr) CloseHandle(fenceEvent);
		}

		// Submits what was recorded and waits for the GPU, then reopens the list.
		bool Submit() {
			if (FAILED(list->Close())) return false;
			ID3D12CommandList *const lists[]{list.Get()};
			queue->ExecuteCommandLists(1, lists);
			if (FAILED(queue->Signal(fence.Get(), ++fenceValue))) return false;
			if (fence->GetCompletedValue() < fenceValue) {
				fence->SetEventOnCompletion(fenceValue, fenceEvent);
				WaitForSingleObject(fenceEvent, INFINITE);
			}
			return SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.Get(), nullptr));
		}

		D3D12_CPU_DESCRIPTOR_HANDLE BackBufferView() const { return rtvHeap->GetCPUDescriptorHandleForHeapStart(); }
	};

	cGDriver12::cGDriver12() : impl(std::make_unique<Impl>()) {}
	cGDriver12::~cGDriver12() = default;

	bool cGDriver12::Create(bool useWarp) {
		Impl &d = *impl;
		if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&d.factory)))) return false;
		ComPtr<IDXGIAdapter1> adapter;
		if (useWarp) d.factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
		else d.factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
		if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d.device)))) return false;
		D3D12_COMMAND_QUEUE_DESC queue{};
		queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		if (FAILED(d.device->CreateCommandQueue(&queue, IID_PPV_ARGS(&d.queue)))) return false;
		if (FAILED(d.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&d.allocator)))) return false;
		if (FAILED(d.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d.allocator.Get(), nullptr,
		                                       IID_PPV_ARGS(&d.list)))) return false;
		if (FAILED(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d.fence)))) return false;
		d.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		D3D12_DESCRIPTOR_HEAP_DESC rtv{};
		rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		rtv.NumDescriptors = 1;
		return SUCCEEDED(d.device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&d.rtvHeap))) && d.fenceEvent != nullptr;
	}

	ID3D12Device *cGDriver12::Device() const { return impl->device.Get(); }
	ID3D12CommandQueue *cGDriver12::Queue() const { return impl->queue.Get(); }
	ID3D12Resource *cGDriver12::BackBuffer() const { return impl->backBuffer.Get(); }

	void cGDriver12::VideoMode(bool, uint32_t width, uint32_t height, bool) {
		Impl &d = *impl;
		if (!d.device || width == 0 || height == 0) return;
		D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
		D3D12_RESOURCE_DESC description{};
		description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		description.Width = width;
		description.Height = height;
		description.DepthOrArraySize = 1;
		description.MipLevels = 1;
		description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		description.SampleDesc.Count = 1;
		description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		D3D12_CLEAR_VALUE clear{DXGI_FORMAT_R8G8B8A8_UNORM, {0, 0, 0, 1}};
		d.backBuffer.Reset();
		if (FAILED(d.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
		                                              D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
		                                              IID_PPV_ARGS(&d.backBuffer)))) return;
		d.device->CreateRenderTargetView(d.backBuffer.Get(), nullptr, d.BackBufferView());
		d.width = width;
		d.height = height;
	}

	void cGDriver12::Vertices(uint32_t, void const *, uint32_t) {}

	bool cGDriver12::Frame() { return true; }

	bool cGDriver12::ReadBack(std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height) {
		Impl &d = *impl;
		if (!d.backBuffer) return false;
		D3D12_RESOURCE_DESC const description = d.backBuffer->GetDesc();
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
		UINT64 total = 0;
		d.device->GetCopyableFootprints(&description, 0, 1, 0, &footprint, nullptr, nullptr, &total);
		D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
		D3D12_RESOURCE_DESC buffer{};
		buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		buffer.Width = total;
		buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
		buffer.SampleDesc.Count = 1;
		buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		ComPtr<ID3D12Resource> readback;
		if (FAILED(d.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
		                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
		                                              IID_PPV_ARGS(&readback)))) return false;
		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = d.backBuffer.Get();
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
		d.list->ResourceBarrier(1, &barrier);
		D3D12_TEXTURE_COPY_LOCATION destination{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
		destination.PlacedFootprint = footprint;
		D3D12_TEXTURE_COPY_LOCATION source{d.backBuffer.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
		d.list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
		std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
		d.list->ResourceBarrier(1, &barrier);
		if (!d.Submit()) return false;

		width = d.width;
		height = d.height;
		rgba.resize(static_cast<size_t>(width) * height * 4);
		uint8_t *mapped = nullptr;
		if (FAILED(readback->Map(0, nullptr, reinterpret_cast<void **>(&mapped)))) return false;
		for (uint32_t y = 0; y < height; ++y) {
			std::memcpy(rgba.data() + static_cast<size_t>(y) * width * 4,
			            mapped + footprint.Offset + static_cast<size_t>(y) * footprint.Footprint.RowPitch,
			            static_cast<size_t>(width) * 4);
		}
		readback->Unmap(0, nullptr);
		return true;
	}

	bool cGDriver12::QueryInterface(uint32_t riid, void **ppvObj) {
		if (ppvObj == nullptr) return false;
		switch (riid) {
			case GZIID_cIGZUnknown:
			case GZIID_cIGZGDriver: *ppvObj = static_cast<cIGZGDriver *>(this); return true;
			case GZIID_cIGZGBufferRegionExtension: *ppvObj = static_cast<cIGZGBufferRegionExtension *>(this); return true;
			case GZIID_cIGZGDriverLightingExtension: *ppvObj = static_cast<cIGZGDriverLightingExtension *>(this); return true;
			case GZIID_cIGZGDriverVertexBufferExtension:
				*ppvObj = static_cast<cIGZGDriverVertexBufferExtension *>(this);
				return true;
			case GZIID_cIGZGSnapshotExtension: *ppvObj = static_cast<cIGZGSnapshotExtension *>(this); return true;
			default: *ppvObj = nullptr; return false;
		}
	}
	uint32_t cGDriver12::AddRef(void) { return 1; }
	uint32_t cGDriver12::Release(void) { return 1; }

	void cGDriver12::Clear(uint32_t mask) {
		Impl &d = *impl;
		if (d.backBuffer && (mask & 0x4000) != 0) d.list->ClearRenderTargetView(d.BackBufferView(), d.clearColor, 0, nullptr);
	}
	void cGDriver12::ClearColor(float r, float g, float b, float a) {
		impl->clearColor[0] = r;
		impl->clearColor[1] = g;
		impl->clearColor[2] = b;
		impl->clearColor[3] = a;
	}
	void cGDriver12::Flush(void) { impl->Submit(); }

	// Everything below is still to be ported from cGDriver.
	void cGDriver12::DrawArrays(uint32_t, int32_t, int32_t) {}
	void cGDriver12::DrawElements(uint32_t, int32_t, uint32_t, void const *) {}
	void cGDriver12::InterleavedArrays(uint32_t, int32_t, void const *) {}
	uint32_t cGDriver12::MakeVertexFormat(uint32_t, intptr_t) { return 0; }
	uint32_t cGDriver12::MakeVertexFormat(uint32_t format) { return format; }
	uint32_t cGDriver12::VertexFormatStride(uint32_t format) { return RZVertexFormatStride(format); }
	uint32_t cGDriver12::VertexFormatElementOffset(uint32_t format, uint32_t element, uint32_t index) {
		return RZVertexFormatElementOffset(format, element, index);
	}
	uint32_t cGDriver12::VertexFormatNumElements(uint32_t, uint32_t) { return 0; }
	void cGDriver12::ClearDepth(double) {}
	void cGDriver12::ClearStencil(int32_t) {}
	void cGDriver12::ColorMask(bool) {}
	void cGDriver12::DepthFunc(uint32_t) {}
	void cGDriver12::DepthMask(bool) {}
	void cGDriver12::StencilFunc(uint32_t, int32_t, uint32_t) {}
	void cGDriver12::StencilMask(uint32_t) {}
	void cGDriver12::StencilOp(uint32_t, uint32_t, uint32_t) {}
	void cGDriver12::BlendFunc(uint32_t, uint32_t) {}
	void cGDriver12::AlphaFunc(uint32_t, float) {}
	void cGDriver12::ShadeModel(uint32_t) {}
	void cGDriver12::BindTexture(uint32_t, uint32_t) {}
	void cGDriver12::TexImage2D(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, void const *) {}
	void cGDriver12::PixelStore(uint32_t, int32_t) {}
	void cGDriver12::TexEnv(uint32_t, uint32_t, int32_t) {}
	void cGDriver12::TexEnv(uint32_t, uint32_t, float const *) {}
	void cGDriver12::TexParameter(uint32_t, uint32_t, int32_t) {}
	void cGDriver12::Fog(uint32_t, uint32_t) {}
	void cGDriver12::Fog(uint32_t, float const *) {}
	void cGDriver12::ColorMultiplier(float, float, float) {}
	void cGDriver12::AlphaMultiplier(float) {}
	void cGDriver12::EnableVertexColors(bool, bool) {}
	void cGDriver12::GenTextures(int32_t count, uint32_t *names) {
		static uint32_t next = 1;
		for (int32_t i = 0; i < count; ++i) names[i] = next++;
	}
	void cGDriver12::DeleteTextures(int32_t, uint32_t const *) {}
	bool cGDriver12::IsTexture(uint32_t) { return false; }
	void cGDriver12::PrioritizeTextures(int32_t, uint32_t const *, float const *) {}
	bool cGDriver12::AreTexturesResident(int32_t, uint32_t const *, bool *) { return true; }
	void cGDriver12::MatrixMode(uint32_t) {}
	void cGDriver12::LoadMatrix(float const *) {}
	void cGDriver12::LoadIdentity(void) {}
	void cGDriver12::Enable(uint32_t) {}
	void cGDriver12::Disable(uint32_t) {}
	bool cGDriver12::IsEnabled(uint32_t) { return false; }
	void cGDriver12::GetBoolean(uint32_t, bool *) {}
	void cGDriver12::GetInteger(uint32_t, int32_t *) {}
	void cGDriver12::GetFloat(uint32_t, float *) {}
	uint32_t cGDriver12::GetError(void) { return 0; }
	void cGDriver12::TexStage(uint32_t) {}
	void cGDriver12::TexStageCoord(uint32_t) {}
	void cGDriver12::TexStageMatrix(float const *, uint32_t, uint32_t, uint32_t) {}
	void cGDriver12::TexStageCombine(eGDTextureStageCombineScaleParamType, eGDTextureStageCombineScaleParam) {}
	void cGDriver12::TexStageCombine(eGDTextureStageCombineOperandType, eGDBlend) {}
	void cGDriver12::TexStageCombine(eGDTextureStageCombineSourceParamType, eGDTextureStageCombineSourceParam) {}
	void cGDriver12::TexStageCombine(eGDTextureStageCombineParamType, eGDTextureStageCombineModeParam) {}
	void cGDriver12::SetTexture(uint32_t, uint32_t) {}
	intptr_t cGDriver12::GetTexture(uint32_t) { return 0; }
	intptr_t cGDriver12::CreateTexture(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {
		uint32_t name = 0;
		GenTextures(1, &name);
		return name;
	}
	void cGDriver12::LoadTextureLevel(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t,
	                                  uint32_t, void const *) {}
	void cGDriver12::SetCombiner(cGDCombiner const &, uint32_t) {}
	uint32_t cGDriver12::CountVideoModes(void) const { return 0; }
	void cGDriver12::GetVideoModeInfo(uint32_t, sGDMode &) {}
	void cGDriver12::GetVideoModeInfo(sGDMode &) {}
	void cGDriver12::SetVideoMode(int32_t, void *, bool, bool) {}
	void cGDriver12::PolygonOffset(int32_t) {}
	void cGDriver12::BitBlt(int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, void const *, bool, void const *) {}
	void cGDriver12::StretchBlt(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, void const *,
	                            bool, void const *) {}
	void cGDriver12::BitBltAlpha(int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, void const *, bool,
	                             void const *, uint32_t) {}
	void cGDriver12::StretchBltAlpha(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t,
	                                 void const *, bool, void const *, uint32_t) {}
	void cGDriver12::BitBltAlphaModulate(int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, void const *, bool,
	                                     void const *, uint32_t) {}
	void cGDriver12::StretchBltAlphaModulate(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t,
	                                         void const *, bool, void const *, uint32_t) {}
	void cGDriver12::SetViewport(int32_t, int32_t, int32_t, int32_t) {}
	void cGDriver12::SetViewport(void) {}
	void cGDriver12::GetViewport(int32_t dimensions[4]) { std::memset(dimensions, 0, 4 * sizeof(int32_t)); }
	char const *cGDriver12::GetDriverInfo(void) const { return "SCD3D12"; }
	uint32_t cGDriver12::GetGZCLSID(void) const { return 0xBADB6906; }
	bool cGDriver12::Init(void) { return true; }
	bool cGDriver12::Shutdown(void) { return true; }
	bool cGDriver12::IsDeviceReady(void) { return impl->backBuffer != nullptr; }
	bool cGDriver12::Punt(uint32_t, void *) { return false; }
	char const *cGDriver12::GetVertexBufferName(uint32_t) { return nullptr; }
	uint32_t cGDriver12::VertexBufferType(uint32_t name) { return name == 0 ? kGDVertexFormat_V3F_C4UB_2T2F : UINT32_MAX; }
	uint32_t cGDriver12::MaxVertices(uint32_t name) { return name == 0 ? 32768 : 0; }
	uint32_t cGDriver12::GetVertices(int32_t, uint32_t) { return 0; }
	uint32_t cGDriver12::ContinueVertices(uint32_t, uint32_t) { return 0; }
	void cGDriver12::ReleaseVertices(uint32_t) {}
	void cGDriver12::DrawPrims(uint32_t, uint32_t, void *, uint32_t) {}
	void cGDriver12::DrawPrimsIndexed(uint32_t, uint32_t, uint32_t, uint16_t *) {}
	void cGDriver12::Reset(void) {}
	bool cGDriver12::BufferRegionEnabled(void) { return true; }
	uint32_t cGDriver12::NewBufferRegion(int32_t) { return 0; }
	bool cGDriver12::DeleteBufferRegion(int32_t) { return true; }
	bool cGDriver12::ReadBufferRegion(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t) { return true; }
	bool cGDriver12::DrawBufferRegion(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t) { return true; }
	bool cGDriver12::IsBufferRegion(uint32_t) { return false; }
	bool cGDriver12::CanDoPartialRegionWrites(void) { return true; }
	bool cGDriver12::CanDoOffsetReads(void) { return true; }
	bool cGDriver12::FinalRelease(void) { return true; }
	bool cGDriver12::DeleteAllBufferRegions(void) { return true; }
	cIGZBuffer *cGDriver12::CopyColorBuffer(int32_t, int32_t, int32_t, int32_t, cIGZBuffer *buffer) { return buffer; }
	void cGDriver12::EnableLighting(bool) {}
	void cGDriver12::EnableLight(uint32_t, bool) {}
	void cGDriver12::LightModelAmbient(float, float, float, float) {}
	void cGDriver12::LightColor(uint32_t, uint32_t, float const *) {}
	void cGDriver12::LightColor(uint32_t, float const *, float const *, float const *) {}
	void cGDriver12::LightPosition(uint32_t, float const *) {}
	void cGDriver12::LightDirection(uint32_t, float const *) {}
	void cGDriver12::MaterialColor(uint32_t, float const *) {}
	void cGDriver12::MaterialColor(float const *, float const *, float const *, float const *, float) {}
}
