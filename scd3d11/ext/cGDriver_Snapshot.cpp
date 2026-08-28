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

extern cRZCOMSlimDllDirector *RZGetCOMDllDirector();

static const uint32_t GZIID_cIGZGraphicSystem = 0x73283c;
static const uint32_t RZSRVID_GraphicSystem = 0xc416025c;

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
		int32_t const right = x + width > windowWidth ? windowWidth : x + width;
		int32_t const bottom = y + height > windowHeight ? windowHeight : y + height;
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
		} else if (buffer->GetColorType() != cGZBufferColorType::A8R8G8B8 ||
		           buffer->Width() != static_cast<uint32_t>(width) || buffer->Height() != static_cast<uint32_t>(
			           height)) {
			return nullptr;
		}

		if (!backBufferTexture) return fail();

		D3D11_TEXTURE2D_DESC stagingDescription{};
		stagingDescription.Width = static_cast<UINT>(width);
		stagingDescription.Height = static_cast<UINT>(height);
		stagingDescription.MipLevels = 1;
		stagingDescription.ArraySize = 1;
		stagingDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		stagingDescription.SampleDesc.Count = 1;
		stagingDescription.Usage = D3D11_USAGE_STAGING;
		stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

		Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
		HRESULT result = d3dDevice->CreateTexture2D(&stagingDescription, nullptr, &staging);
		if (FAILED(result)) {
			LogHRESULT(LogCategory::Resource, "ID3D11Device::CreateTexture2D(snapshot)", result);
			return fail();
		}

		D3D11_BOX const sourceBox{
			static_cast<UINT>(left), static_cast<UINT>(top), 0,
			static_cast<UINT>(right), static_cast<UINT>(bottom), 1
		};
		d3dContext->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, backBufferTexture.Get(), 0, &sourceBox);

		D3D11_MAPPED_SUBRESOURCE mapping{};
		result = d3dContext->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapping);
		if (FAILED(result)) {
			LogHRESULT(LogCategory::Resource, "ID3D11DeviceContext::Map(snapshot)", result);
			return fail();
		}

		bool const locked = buffer->Lock(cIGZBuffer::eLockFlags::IsDirtyUpdate);
		if (locked) {
			for (int32_t row = 0; row < height; ++row) {
				uint8_t const *source = static_cast<uint8_t const *>(mapping.pData) + static_cast<size_t>(row) * mapping
				                        .RowPitch;
				for (int32_t column = 0; column < width; ++column) {
					uint32_t const color = 0xff000000u |
					                       static_cast<uint32_t>(source[column * 4 + 0]) << 16 |
					                       static_cast<uint32_t>(source[column * 4 + 1]) << 8 |
					                       source[column * 4 + 2];
					buffer->SetPixel(column, row, color);
				}
			}
			buffer->Unlock(cIGZBuffer::eLockFlags::IsDirtyUpdate);
		}
		d3dContext->Unmap(staging.Get(), 0);
		if (!locked) {
			SetLastError(DriverError::CREATE_CONTEXT_FAIL);
			return fail();
		}
		return buffer;
	}
}
