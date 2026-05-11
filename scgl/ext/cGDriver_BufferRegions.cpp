/*
 *  SCGL - a free graphics driver for SimCity 4's SimGL interface
 */

#include <cassert>
#include "../cGDriver.h"

namespace nSCGL
{
	int cGDriver::FindFreeBufferRegionIndex(void) {
		if (bufferRegionFlags == UINT8_MAX) {
			return -1;
		}

		for (uint32_t i = 0; i < MAX_BUFFER_REGIONS; i++) {
			if ((bufferRegionFlags & (1 << i)) == 0) {
				return i;
			}
		}

		assert(false);
		return -1;
	}

	bool cGDriver::BufferRegionEnabled(void) {
		return supportedFeatures.bufferRegion;
	}

	uint32_t cGDriver::NewBufferRegion(int32_t gdBufferRegionType) {
		if (d3dDevice == nullptr) {
			return 0;
		}

		uint32_t bufferRegionIndex = FindFreeBufferRegionIndex();
		if (bufferRegionIndex == static_cast<uint32_t>(-1)) {
			return 0;
		}

		IDirect3DSurface9* surface = nullptr;
		HRESULT hr;
		if (gdBufferRegionType == 0) {
			hr = d3dDevice->CreateRenderTarget(
				windowWidth,
				windowHeight,
				presentParams.BackBufferFormat,
				D3DMULTISAMPLE_NONE,
				0,
				FALSE,
				&surface,
				nullptr);
		}
		else {
			hr = d3dDevice->CreateDepthStencilSurface(
				windowWidth,
				windowHeight,
				presentParams.AutoDepthStencilFormat,
				D3DMULTISAMPLE_NONE,
				0,
				FALSE,
				&surface,
				nullptr);
		}

		if (FAILED(hr)) {
			return 0;
		}

		bufferRegionFlags |= 1 << bufferRegionIndex;
		bufferRegions[bufferRegionIndex].surface = surface;
		bufferRegions[bufferRegionIndex].type = gdBufferRegionType;

		return bufferRegionIndex + 1;
	}

	bool cGDriver::DeleteBufferRegion(int32_t region) {
		uint32_t bufferRegionIndex = region - 1;
		if (bufferRegionIndex >= MAX_BUFFER_REGIONS || (bufferRegionFlags & (1 << bufferRegionIndex)) == 0) {
			return true;
		}

		if (bufferRegions[bufferRegionIndex].surface != nullptr) {
			bufferRegions[bufferRegionIndex].surface->Release();
			bufferRegions[bufferRegionIndex].surface = nullptr;
		}

		bufferRegionFlags &= ~(1 << bufferRegionIndex);
		return true;
	}

	bool cGDriver::ReadBufferRegion(uint32_t region, int32_t dstX, int32_t dstY, int32_t width, int32_t height, int32_t srcX, int32_t srcY) {
		uint32_t bufferRegionIndex = region - 1;
		if (d3dDevice == nullptr || bufferRegionIndex >= MAX_BUFFER_REGIONS || bufferRegions[bufferRegionIndex].surface == nullptr) {
			return false;
		}

		IDirect3DSurface9* source = nullptr;
		if (bufferRegions[bufferRegionIndex].type == 0) {
			d3dDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &source);
		}
		else {
			d3dDevice->GetDepthStencilSurface(&source);
		}

		if (source == nullptr) {
			return false;
		}

		RECT src{ srcX, srcY, srcX + width, srcY + height };
		RECT dst{ dstX, dstY, dstX + width, dstY + height };
		HRESULT hr = d3dDevice->StretchRect(source, &src, bufferRegions[bufferRegionIndex].surface, &dst, D3DTEXF_NONE);
		source->Release();
		return SUCCEEDED(hr);
	}

	bool cGDriver::DrawBufferRegion(uint32_t region, int32_t srcX, int32_t srcY, int32_t width, int32_t height, int32_t dstX, int32_t dstY) {
		uint32_t bufferRegionIndex = region - 1;
		if (d3dDevice == nullptr || bufferRegionIndex >= MAX_BUFFER_REGIONS || bufferRegions[bufferRegionIndex].surface == nullptr) {
			return false;
		}

		IDirect3DSurface9* dest = nullptr;
		if (bufferRegions[bufferRegionIndex].type == 0) {
			d3dDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &dest);
		}
		else {
			d3dDevice->GetDepthStencilSurface(&dest);
		}

		if (dest == nullptr) {
			return false;
		}

		RECT src{ srcX, srcY, srcX + width, srcY + height };
		RECT dst{ dstX, dstY, dstX + width, dstY + height };
		HRESULT hr = d3dDevice->StretchRect(bufferRegions[bufferRegionIndex].surface, &src, dest, &dst, D3DTEXF_NONE);
		dest->Release();
		return SUCCEEDED(hr);
	}

	bool cGDriver::IsBufferRegion(uint32_t region) {
		if (region == 0 || region > MAX_BUFFER_REGIONS) {
			return false;
		}

		return (bufferRegionFlags & (1 << (region - 1))) != 0;
	}

	bool cGDriver::CanDoPartialRegionWrites(void) {
		return true;
	}

	bool cGDriver::CanDoOffsetReads(void) {
		return true;
	}

	bool cGDriver::DeleteAllBufferRegions(void) {
		for (size_t i = 1; i <= MAX_BUFFER_REGIONS; i++) {
			if (IsBufferRegion(i)) {
				DeleteBufferRegion(static_cast<int32_t>(i));
			}
		}

		bufferRegionFlags = 0;
		return true;
	}
}

