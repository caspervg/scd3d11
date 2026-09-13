/*
 *  SCD3D11 - a free graphics driver for SimCity 4's SimGL interface
 *  Copyright (C) 2025  Nelson Gomez (nsgomez) <nelson@ngomez.me>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#include "../cGDriver.h"
#include "../Diagnostics.h"

namespace nSCD3D11 {
	HRESULT cGDriver::EnsureDepthRegionScratch(void) {
		if (depthRegionScratch) return S_OK;
		if (!d3dDevice || windowWidth <= 0 || windowHeight <= 0) return E_POINTER;

		D3D11_TEXTURE2D_DESC description{};
		description.Width = static_cast<UINT>(windowWidth);
		description.Height = static_cast<UINT>(windowHeight);
		description.MipLevels = 1;
		description.ArraySize = 1;
		description.Format = DXGI_FORMAT_R24G8_TYPELESS;
		description.SampleDesc.Count = 1;
		description.Usage = D3D11_USAGE_DEFAULT;

		HRESULT const result = d3dDevice->CreateTexture2D(&description, nullptr, &depthRegionScratch);
		if (FAILED(result)) LogHRESULT(LogCategory::Resource, "ID3D11Device::CreateTexture2D(depthRegionScratch)",
		                               result);
		return result;
	}

	int cGDriver::FindFreeBufferRegionIndex(void) {
		for (uint32_t index = 0; index < MAX_BUFFER_REGIONS; ++index) {
			if ((bufferRegionFlags & (1u << index)) == 0) return static_cast<int>(index);
		}
		return -1;
	}

	HRESULT cGDriver::CreateBufferRegionResource(uint32_t index, int32_t type) {
		if (!d3dDevice || index >= MAX_BUFFER_REGIONS || type < 0 || type > 1 ||
		    windowWidth <= 0 || windowHeight <= 0) {
			return E_INVALIDARG;
		}

		D3D11_TEXTURE2D_DESC description{};
		description.Width = static_cast<UINT>(windowWidth);
		description.Height = static_cast<UINT>(windowHeight);
		description.MipLevels = 1;
		description.ArraySize = 1;
		// Depth regions stay typeless so partial copies never involve a depth-stencil resource.
		description.Format = type == 0 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R24G8_TYPELESS;
		description.SampleDesc.Count = 1;
		description.Usage = D3D11_USAGE_DEFAULT;

		Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
		HRESULT const result = d3dDevice->CreateTexture2D(&description, nullptr, &texture);
		if (FAILED(result)) {
			LogHRESULT(LogCategory::Resource, "ID3D11Device::CreateTexture2D(buffer region)", result);
			return result;
		}
		bufferRegions[index].texture = texture;
		bufferRegions[index].type = type;
		return S_OK;
	}

	HRESULT cGDriver::RecreateBufferRegions() {
		for (uint32_t index = 0; index < MAX_BUFFER_REGIONS; ++index) {
			if ((bufferRegionFlags & (1u << index)) == 0) continue;
			int32_t const type = bufferRegions[index].type;
			bufferRegions[index].texture.Reset();
			HRESULT const result = CreateBufferRegionResource(index, type);
			if (FAILED(result)) return result;
		}
		return S_OK;
	}

	bool cGDriver::BufferRegionEnabled(void) {
		return d3dDevice != nullptr;
	}

	uint32_t cGDriver::NewBufferRegion(int32_t type) {
		int const index = FindFreeBufferRegionIndex();
		if (index < 0 || type < 0 || type > 1) {
			SetLastError(type < 0 || type > 1 ? DriverError::INVALID_ENUM : DriverError::OUT_OF_RANGE);
			return 0;
		}
		HRESULT const result = CreateBufferRegionResource(static_cast<uint32_t>(index), type);
		if (FAILED(result)) {
			SetLastError(DriverError::CREATE_CONTEXT_FAIL);
			return 0;
		}
		bufferRegionFlags |= static_cast<uint8_t>(1u << index);
		return static_cast<uint32_t>(index + 1);
	}

	bool cGDriver::DeleteBufferRegion(int32_t region) {
		if (region < 1 || region > static_cast<int32_t>(MAX_BUFFER_REGIONS)) {
			SetLastError(DriverError::OUT_OF_RANGE);
			return false;
		}
		uint32_t const index = static_cast<uint32_t>(region - 1);
		bufferRegions[index].texture.Reset();
		bufferRegionFlags &= static_cast<uint8_t>(~(1u << index));
		return true;
	}

	bool cGDriver::ReadBufferRegion(
		uint32_t region, int32_t destinationX, int32_t destinationY, int32_t width, int32_t height,
		int32_t sourceX, int32_t sourceY) {
		if (!IsBufferRegion(region) || width <= 0 || height <= 0 || destinationX < 0 || destinationY < 0 ||
		    sourceX < 0 || sourceY < 0 || destinationX + width > windowWidth || destinationY + height > windowHeight ||
		    sourceX + width > windowWidth || sourceY + height > windowHeight) {
			static bool logged = false;
			if (!logged) {
				Log(LogCategory::Unsupported, "ReadBufferRegion rejected: region %u dst %d,%d src %d,%d %dx%d window %dx%d",
				    region, destinationX, destinationY, sourceX, sourceY, width, height, windowWidth, windowHeight);
				logged = true;
			}
			SetLastError(DriverError::INVALID_VALUE);
			return false;
		}

		uint32_t const index = region - 1;
		ID3D11Resource *source = nullptr;
		if (bufferRegions[index].type == 0) {
			if (!backBufferTexture) return false;
			source = backBufferTexture.Get();
		} else {
			// Partial copies touching a depth-stencil-bound resource are illegal; refresh the
			// plain scratch copy and read from that instead.
			HRESULT const result = EnsureDepthRegionScratch();
			if (FAILED(result)) {
				SetLastError(DriverError::CREATE_CONTEXT_FAIL);
				return false;
			}
			if (!depthRegionScratchValid) {
				d3dContext->CopyResource(depthRegionScratch.Get(), depthStencilTexture.Get());
				depthRegionScratchValid = true;
			}
			source = depthRegionScratch.Get();
		}

		D3D11_BOX const sourceBox{
			static_cast<UINT>(sourceX), static_cast<UINT>(sourceY), 0,
			static_cast<UINT>(sourceX + width), static_cast<UINT>(sourceY + height), 1
		};
		d3dContext->CopySubresourceRegion(
			bufferRegions[index].texture.Get(), 0,
			static_cast<UINT>(destinationX), static_cast<UINT>(destinationY), 0,
			source, 0, &sourceBox);
		return true;
	}

	bool cGDriver::DrawBufferRegion(
		uint32_t region, int32_t sourceX, int32_t sourceY, int32_t width, int32_t height,
		int32_t destinationX, int32_t destinationY) {
		if (!IsBufferRegion(region) || width <= 0 || height <= 0 || destinationX < 0 || destinationY < 0 ||
		    sourceX < 0 || sourceY < 0 || destinationX + width > windowWidth || destinationY + height > windowHeight ||
		    sourceX + width > windowWidth || sourceY + height > windowHeight) {
			static bool logged = false;
			if (!logged) {
				Log(LogCategory::Unsupported, "DrawBufferRegion rejected: region %u src %d,%d dst %d,%d %dx%d window %dx%d",
				    region, sourceX, sourceY, destinationX, destinationY, width, height, windowWidth, windowHeight);
				logged = true;
			}
			SetLastError(DriverError::INVALID_VALUE);
			return false;
		}

		uint32_t const index = region - 1;
		ID3D11Resource *destination = nullptr;
		if (bufferRegions[index].type == 0) {
			if (!backBufferTexture) return false;
			destination = backBufferTexture.Get();
		} else {
			// Partial copies touching a depth-stencil-bound resource are illegal; patch the
			// plain scratch copy instead, then replace the depth buffer wholesale.
			HRESULT const result = EnsureDepthRegionScratch();
			if (FAILED(result)) {
				SetLastError(DriverError::CREATE_CONTEXT_FAIL);
				return false;
			}
			if (!depthRegionScratchValid) {
				d3dContext->CopyResource(depthRegionScratch.Get(), depthStencilTexture.Get());
				depthRegionScratchValid = true;
			}
			destination = depthRegionScratch.Get();
		}

		D3D11_BOX const sourceBox{
			static_cast<UINT>(sourceX), static_cast<UINT>(sourceY), 0,
			static_cast<UINT>(sourceX + width), static_cast<UINT>(sourceY + height), 1
		};
		d3dContext->CopySubresourceRegion(
			destination, 0,
			static_cast<UINT>(destinationX), static_cast<UINT>(destinationY), 0,
			bufferRegions[index].texture.Get(), 0, &sourceBox);
		if (bufferRegions[index].type == 1) {
			d3dContext->CopyResource(depthStencilTexture.Get(), depthRegionScratch.Get());
			depthRegionScratchValid = true;
		}
		return true;
	}

	bool cGDriver::IsBufferRegion(uint32_t region) {
		return region >= 1 && region <= MAX_BUFFER_REGIONS &&
		       (bufferRegionFlags & (1u << (region - 1))) != 0 &&
		       bufferRegions[region - 1].texture != nullptr;
	}

	bool cGDriver::CanDoPartialRegionWrites(void) {
		return true;
	}

	bool cGDriver::CanDoOffsetReads(void) {
		return true;
	}

	bool cGDriver::DeleteAllBufferRegions(void) {
		for (uint32_t index = 0; index < MAX_BUFFER_REGIONS; ++index) {
			bufferRegions[index].texture.Reset();
		}
		bufferRegionFlags = 0;
		return true;
	}
}
