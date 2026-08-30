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
	namespace {
		struct BufferRegionDiagnostics {
			bool availabilityLogged = false;
			uint64_t readCount[2]{};
			uint64_t drawCount[2]{};
			bool invalidReadLogged = false;
			bool invalidDrawLogged = false;
		};

		bool ShouldLogBufferRegionCall(uint64_t count) {
			return count <= 4 || (count & (count - 1)) == 0;
		}

		void LogColorResourceSample(
			ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Texture2D *texture,
			char const *operation, uint64_t callCount) {
			if (device == nullptr || context == nullptr || texture == nullptr) return;

			D3D11_TEXTURE2D_DESC description{};
			texture->GetDesc(&description);
			description.Usage = D3D11_USAGE_STAGING;
			description.BindFlags = 0;
			description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			description.MiscFlags = 0;

			Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
			HRESULT result = device->CreateTexture2D(&description, nullptr, &staging);
			if (FAILED(result)) {
				LogHRESULT(LogCategory::Resource, "ID3D11Device::CreateTexture2D(buffer region sample)", result);
				return;
			}
			context->CopyResource(staging.Get(), texture);

			D3D11_MAPPED_SUBRESOURCE mapped{};
			result = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
			if (FAILED(result)) {
				LogHRESULT(LogCategory::Resource, "ID3D11DeviceContext::Map(buffer region sample)", result);
				return;
			}

			uint64_t hash = 1469598103934665603ULL;
			uint32_t nonBlack = 0;
			uint32_t const columns = description.Width < 64 ? description.Width : 64;
			uint32_t const rows = description.Height < 64 ? description.Height : 64;
			for (uint32_t row = 0; row < rows; ++row) {
				uint32_t const y = rows == 1 ? 0 : row * (description.Height - 1) / (rows - 1);
				uint8_t const *const scanline = static_cast<uint8_t const *>(mapped.pData) + y * mapped.RowPitch;
				for (uint32_t column = 0; column < columns; ++column) {
					uint32_t const x = columns == 1 ? 0 : column * (description.Width - 1) / (columns - 1);
					uint8_t const *const pixel = scanline + x * 4;
					if (pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0) ++nonBlack;
					for (uint32_t component = 0; component < 4; ++component) {
						hash = (hash ^ pixel[component]) * 1099511628211ULL;
					}
				}
			}
			context->Unmap(staging.Get(), 0);
			Log(LogCategory::Resource,
			    "buffer region color sample after %s #%llu: hash=%016llX nonblack=%u/%u",
			    operation, static_cast<unsigned long long>(callCount),
			    static_cast<unsigned long long>(hash), nonBlack, columns * rows);
		}

		BufferRegionDiagnostics &RegionDiagnostics() {
			static BufferRegionDiagnostics diagnostics;
			return diagnostics;
		}
	}

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
		bool const enabled = d3dDevice != nullptr;
		BufferRegionDiagnostics &diagnostics = RegionDiagnostics();
		if (!diagnostics.availabilityLogged) {
			diagnostics.availabilityLogged = true;
			char const *const state = enabled ? "enabled" : "unavailable before device creation";
			Log(LogCategory::Resource, "buffer regions %s", state);
		}
		return enabled;
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
		Log(LogCategory::Resource, "buffer region %u created: type=%d size=%dx%d",
		    static_cast<uint32_t>(index + 1), type, windowWidth, windowHeight);
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
			BufferRegionDiagnostics &diagnostics = RegionDiagnostics();
			if (!diagnostics.invalidReadLogged) {
				diagnostics.invalidReadLogged = true;
				Log(LogCategory::Resource,
				    "buffer region read rejected: region=%u dst=(%d,%d) size=%dx%d src=(%d,%d) target=%dx%d",
				    region, destinationX, destinationY, width, height, sourceX, sourceY, windowWidth, windowHeight);
			}
			SetLastError(DriverError::INVALID_VALUE);
			return false;
		}

		uint32_t const index = region - 1;
		BufferRegionDiagnostics &diagnostics = RegionDiagnostics();
		uint64_t const readCount = ++diagnostics.readCount[bufferRegions[index].type];
		if (ShouldLogBufferRegionCall(readCount)) {
			Log(LogCategory::Resource,
			    "buffer region read #%llu: region=%u type=%d dst=(%d,%d) size=%dx%d src=(%d,%d)",
			    static_cast<unsigned long long>(readCount), region, bufferRegions[index].type,
			    destinationX, destinationY, width, height, sourceX, sourceY);
		}
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
		if (bufferRegions[index].type == 0 && (readCount == 2 || readCount == 3)) {
			LogColorResourceSample(
				d3dDevice.Get(), d3dContext.Get(), bufferRegions[index].texture.Get(), "read", readCount);
		}
		return true;
	}

	bool cGDriver::DrawBufferRegion(
		uint32_t region, int32_t sourceX, int32_t sourceY, int32_t width, int32_t height,
		int32_t destinationX, int32_t destinationY) {
		if (!IsBufferRegion(region) || width <= 0 || height <= 0 || destinationX < 0 || destinationY < 0 ||
		    sourceX < 0 || sourceY < 0 || destinationX + width > windowWidth || destinationY + height > windowHeight ||
		    sourceX + width > windowWidth || sourceY + height > windowHeight) {
			BufferRegionDiagnostics &diagnostics = RegionDiagnostics();
			if (!diagnostics.invalidDrawLogged) {
				diagnostics.invalidDrawLogged = true;
				Log(LogCategory::Resource,
				    "buffer region draw rejected: region=%u src=(%d,%d) size=%dx%d dst=(%d,%d) target=%dx%d",
				    region, sourceX, sourceY, width, height, destinationX, destinationY, windowWidth, windowHeight);
			}
			SetLastError(DriverError::INVALID_VALUE);
			return false;
		}

		uint32_t const index = region - 1;
		BufferRegionDiagnostics &diagnostics = RegionDiagnostics();
		uint64_t const drawCount = ++diagnostics.drawCount[bufferRegions[index].type];
		if (ShouldLogBufferRegionCall(drawCount)) {
			Log(LogCategory::Resource,
			    "buffer region draw #%llu: region=%u type=%d src=(%d,%d) size=%dx%d dst=(%d,%d)",
			    static_cast<unsigned long long>(drawCount), region, bufferRegions[index].type,
			    sourceX, sourceY, width, height, destinationX, destinationY);
		}
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
		if (bufferRegions[index].type == 0 && (drawCount == 2 || drawCount == 3)) {
			LogColorResourceSample(
				d3dDevice.Get(), d3dContext.Get(), backBufferTexture.Get(), "draw", drawCount);
		}
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
