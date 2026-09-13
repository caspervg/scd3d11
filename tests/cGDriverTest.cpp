#include "cGDriver.h"
#include "VertexFormatUtils.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace nSCD3D11 {
	// Befriended by cGDriver so the checks can reach driver state and D3D11 resources directly.
	struct cGDriverTestAccess {
		cGDriver &d;

		// Returns a subresource's texels with rows packed tightly.
		std::vector<uint8_t> Read(ID3D11Texture2D *texture, UINT level, uint32_t bytesPerPixel) {
			D3D11_TEXTURE2D_DESC description{};
			texture->GetDesc(&description);
			UINT const width = D3D11MipDimension(description.Width, level);
			UINT const height = D3D11MipDimension(description.Height, level);
			description.Width = width;
			description.Height = height;
			description.MipLevels = 1;
			description.Usage = D3D11_USAGE_STAGING;
			description.BindFlags = 0;
			description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			description.MiscFlags = 0;
			ComPtr<ID3D11Texture2D> staging;
			assert(SUCCEEDED(d.d3dDevice->CreateTexture2D(&description, nullptr, &staging)));
			d.d3dContext->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture, level, nullptr);
			D3D11_MAPPED_SUBRESOURCE mapping{};
			assert(SUCCEEDED(d.d3dContext->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapping)));
			std::vector<uint8_t> texels(static_cast<size_t>(width) * height * bytesPerPixel);
			for (UINT y = 0; y < height; ++y) {
				memcpy(texels.data() + static_cast<size_t>(y) * width * bytesPerPixel,
				       static_cast<uint8_t const *>(mapping.pData) + static_cast<size_t>(y) * mapping.RowPitch,
				       static_cast<size_t>(width) * bytesPerPixel);
			}
			d.d3dContext->Unmap(staging.Get(), 0);
			return texels;
		}

		// RGBA of the back buffer pixel at top-left coordinates.
		uint32_t Pixel(int x, int y) {
			std::vector<uint8_t> const pixels = Read(d.backBufferTexture.Get(), 0, 4);
			uint8_t const *p = pixels.data() + (static_cast<size_t>(y) * d.windowWidth + x) * 4;
			return static_cast<uint32_t>(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3];
		}

		struct ColorVertex {
			float position[3];
			uint8_t bgra[4];
		};

		// A clip-space triangle covering the whole viewport.
		void DrawFullscreen(uint8_t r, uint8_t g, uint8_t b) {
			ColorVertex const vertices[3]{
				{{-1.0f, -1.0f, 0.0f}, {b, g, r, 255}},
				{{3.0f, -1.0f, 0.0f}, {b, g, r, 255}},
				{{-1.0f, 3.0f, 0.0f}, {b, g, r, 255}},
			};
			d.InterleavedArrays(kGDVertexFormat_V3F_C4UB, 0, vertices);
			d.DrawArrays(0, 0, 3);
		}

		void DrawsGeometry() {
			d.ClearColor(0.0f, 0.0f, 1.0f, 1.0f);
			d.Clear(0x4000);
			assert(Pixel(1, 1) == 0x0000ffff);
			DrawFullscreen(255, 0, 0);
			assert(Pixel(1, 1) == 0xff0000ff);
		}

		int Run() {
			DrawsGeometry();
			return 0;
		}
	};
}

int main() {
	using nSCD3D11::cGDriver;
	cGDriver *const driver = new cGDriver();
	driver->AddRef();
	int32_t mode = -1;
	if (driver->Init()) {
		sGDMode info{};
		for (uint32_t index = 0; index < driver->CountVideoModes(); ++index) {
			driver->GetVideoModeInfo(index, info);
			if (!info.isFullscreen) {
				mode = static_cast<int32_t>(index);
				break;
			}
		}
	}
	if (mode >= 0) driver->SetVideoMode(mode, nullptr, false, false);
	if (!driver->IsDeviceReady()) {
		std::puts("no D3D11 device; skipping");
		driver->Release();
		return 77;
	}
	int const result = nSCD3D11::cGDriverTestAccess{*driver}.Run();
	driver->Shutdown();
	driver->Release();
	return result;
}
