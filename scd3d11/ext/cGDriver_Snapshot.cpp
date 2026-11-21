/*
 *  SCD3D11 - a free graphics driver for SimCity 4's SimGL interface
 *  Copyright (C) 2025  Nelson Gomez (nsgomez) <nelson@ngomez.me>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#include <cRZCOMDllDirector.h>
#include <cRZSysServPtr.h>

#include "../cGDriver.h"
#include "../Diagnostics.h"

#include <algorithm>

static const uint32_t GZIID_cIGZGraphicSystem = 0x73283c;
static const uint32_t RZSRVID_GraphicSystem = 0xc416025c;
static const ULONGLONG kSnapshotTimeoutMilliseconds = 2000;

class cIGZGraphicSystem : public cIGZUnknown {
public:
	virtual bool CreateBuffer(cIGZBuffer **ppvObj) = 0;
};

namespace nSCD3D11 {
	static cIGZBuffer *CreateBufferFromGraphicsSystem() {
		cRZSysServPtr<cIGZGraphicSystem, GZIID_cIGZGraphicSystem, RZSRVID_GraphicSystem> graphicsSystem;
		if (static_cast<cIGZGraphicSystem *>(graphicsSystem) == nullptr) return nullptr;

		cIGZBuffer *buffer = nullptr;
		return graphicsSystem->CreateBuffer(&buffer) ? buffer : nullptr;
	}

	cIGZBuffer *cGDriver::CopyColorBuffer(
		int32_t x, int32_t y, int32_t width, int32_t height, cIGZBuffer *buffer) {
		if (!d3dDevice || !d3dContext || !swapChain || width <= 0 || height <= 0) return nullptr;

		int32_t const left = x < 0 ? 0 : x;
		int32_t const top = y < 0 ? 0 : y;
		int32_t const right = static_cast<int32_t>((std::min)(static_cast<int64_t>(x) + width, int64_t{windowWidth}));
		int32_t const bottom = static_cast<int32_t>((std::min)(static_cast<int64_t>(y) + height, int64_t{windowHeight}));
		if (right <= left || bottom <= top) return nullptr;
		width = right - left;
		height = bottom - top;

		bool createdBuffer = false;
		auto fail = [&]() -> cIGZBuffer * {
			if (createdBuffer && buffer != nullptr) buffer->Release();
			return nullptr;
		};
		if (buffer == nullptr || !buffer->IsReady()) {
			buffer = CreateBufferFromGraphicsSystem();
			if (buffer == nullptr) return nullptr;
			createdBuffer = true;
			if (!buffer->Init(width, height, cGZBufferColorType::A8R8G8B8, 32)) {
				return fail();
			}
		} else if (buffer->GetColorType().bufferType != cGZBufferColorType::A8R8G8B8 ||
		           buffer->Width() != static_cast<uint32_t>(width) || buffer->Height() != static_cast<uint32_t>(
			           height)) {
			return nullptr;
		}

		if (!backBufferTexture) return fail();

		if (!snapshotStaging || snapshotStagingWidth < static_cast<UINT>(width) ||
		    snapshotStagingHeight < static_cast<UINT>(height)) {
			D3D11_TEXTURE2D_DESC stagingDescription{};
			stagingDescription.Width = (std::max)(snapshotStagingWidth, static_cast<UINT>(width));
			stagingDescription.Height = (std::max)(snapshotStagingHeight, static_cast<UINT>(height));
			stagingDescription.MipLevels = 1;
			stagingDescription.ArraySize = 1;
			stagingDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			stagingDescription.SampleDesc.Count = 1;
			stagingDescription.Usage = D3D11_USAGE_STAGING;
			stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			snapshotStaging.Reset();
			snapshotStagingWidth = snapshotStagingHeight = 0;
			HRESULT const created = d3dDevice->CreateTexture2D(&stagingDescription, nullptr, &snapshotStaging);
			if (FAILED(created)) {
				LogHRESULT(LogCategory::Resource, "ID3D11Device::CreateTexture2D(snapshot)", created);
				return fail();
			}
			snapshotStagingWidth = stagingDescription.Width;
			snapshotStagingHeight = stagingDescription.Height;
		}
		ID3D11Texture2D *const staging = snapshotStaging.Get();
		HRESULT result = S_OK;

		D3D11_BOX const sourceBox{
			static_cast<UINT>(left), static_cast<UINT>(top), 0,
			static_cast<UINT>(right), static_cast<UINT>(bottom), 1
		};
		d3dContext->CopySubresourceRegion(staging, 0, 0, 0, 0, backBufferTexture.Get(), 0, &sourceBox);

		// Wait for the copy with a time limit instead of blocking inside Map. A GPU that stalls here then
		// costs a missing thumbnail, which SC4 replaces with its default one, instead of a hung save.
		d3dContext->Flush();
		D3D11_MAPPED_SUBRESOURCE mapping{};
		ULONGLONG const deadline = GetTickCount64() + kSnapshotTimeoutMilliseconds;
		while ((result = d3dContext->Map(staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapping)) ==
		       DXGI_ERROR_WAS_STILL_DRAWING) {
			if (GetTickCount64() >= deadline) {
				Log(LogCategory::Resource, "snapshot: GPU copy not finished after %llu ms, skipping it",
				    static_cast<unsigned long long>(kSnapshotTimeoutMilliseconds));
				return fail();
			}
			// Sleep(1) can take a whole timer tick (15.6 ms by default), far longer than the copy.
			if (!SwitchToThread()) YieldProcessor();
		}
		if (FAILED(result)) {
			LogHRESULT(LogCategory::Resource, "ID3D11DeviceContext::Map(snapshot)", result);
			NoteDeviceLoss(result);
			return fail();
		}

		bool const locked = buffer->Lock(cIGZBuffer::eLockFlags::IsDirtyUpdate);
		if (locked) {
			// cGZBuffer::SetPixel (0x00826560) stores the color at bits + y * stride + x * bytes per pixel,
			// the values GetColorSurfaceBits and GetColorSurfaceStride return. A 32-bit buffer takes whole rows.
			auto *const bits = reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(buffer->GetColorSurfaceBits()));
			uint32_t const stride = buffer->GetColorSurfaceStride();
			bool const rows = bits != nullptr && buffer->GetBytesPerPixel() == 4 &&
			                  stride >= static_cast<uint32_t>(width) * 4;
			for (int32_t row = 0; row < height; ++row) {
				uint8_t const *source = static_cast<uint8_t const *>(mapping.pData) + static_cast<size_t>(row) * mapping
				                        .RowPitch;
				auto *const target = rows ? reinterpret_cast<uint32_t *>(bits + static_cast<size_t>(row) * stride) : nullptr;
				for (int32_t column = 0; column < width; ++column) {
					uint32_t const color = 0xff000000u |
					                       static_cast<uint32_t>(source[column * 4 + 0]) << 16 |
					                       static_cast<uint32_t>(source[column * 4 + 1]) << 8 |
					                       source[column * 4 + 2];
					if (target) target[column] = color;
					else buffer->SetPixel(column, row, color);
				}
			}
			buffer->Unlock(cIGZBuffer::eLockFlags::IsDirtyUpdate);
		}
		d3dContext->Unmap(staging, 0);
		if (!locked) {
			SetLastError(DriverError::CREATE_CONTEXT_FAIL);
			return fail();
		}
		return buffer;
	}
}
