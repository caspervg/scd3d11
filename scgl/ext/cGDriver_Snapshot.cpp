/*
 *  SCGL - a free graphics driver for SimCity 4's SimGL interface
 */

#include <cIGZCOM.h>
#include <cRZCOMDllDirector.h>
#include <cRZSysServPtr.h>
#include "../cGDriver.h"

extern cRZCOMSlimDllDirector* RZGetCOMDllDirector();

static const uint32_t GZIID_cIGZGraphicSystem = 0x73283c;
static const uint32_t RZSRVID_GraphicSystem = 0xc416025c;

class cIGZGraphicSystem : public cIGZUnknown
{
public:
	virtual bool CreateBuffer(cIGZBuffer** ppvObj) = 0;
};

namespace nSCGL
{
	static inline cIGZBuffer* CreateBufferFromGraphicsSystem()
	{
		cRZSysServPtr<cIGZGraphicSystem, GZIID_cIGZGraphicSystem, RZSRVID_GraphicSystem> pGraphicsSystem;
		if ((cIGZGraphicSystem*)pGraphicsSystem == nullptr) {
			return nullptr;
		}

		cIGZBuffer* newBuffer = nullptr;
		if (!pGraphicsSystem->CreateBuffer(&newBuffer)) {
			return nullptr;
		}

		return newBuffer;
	}

	cIGZBuffer* cGDriver::CopyColorBuffer(int32_t x, int32_t y, int32_t width, int32_t height, cIGZBuffer* buffer) {
		if (d3dDevice == nullptr) {
			return buffer;
		}

		int32_t startX = x < 0 ? 0 : x;
		int32_t startY = y < 0 ? 0 : y;
		int32_t endX = x + width;
		int32_t endY = y + height;

		if (endX > viewportWidth) { endX = viewportWidth; }
		if (endY > viewportHeight) { endY = viewportHeight; }

		width = endX - startX;
		height = endY - startY;
		if (width <= 0 || height <= 0) {
			return buffer;
		}

		if (buffer == nullptr || !buffer->IsReady()) {
			buffer = CreateBufferFromGraphicsSystem();
			if (buffer == nullptr) {
				return nullptr;
			}
		}
		else if (buffer->GetColorType() != cGZBufferColorType::A8R8G8B8) {
			return nullptr;
		}

		IDirect3DSurface9* backBuffer = nullptr;
		if (FAILED(d3dDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer))) {
			return buffer;
		}

		IDirect3DSurface9* systemSurface = nullptr;
		HRESULT hr = d3dDevice->CreateOffscreenPlainSurface(windowWidth, windowHeight, presentParams.BackBufferFormat, D3DPOOL_SYSTEMMEM, &systemSurface, nullptr);
		if (SUCCEEDED(hr)) {
			hr = d3dDevice->GetRenderTargetData(backBuffer, systemSurface);
		}

		backBuffer->Release();

		if (FAILED(hr)) {
			if (systemSurface != nullptr) {
				systemSurface->Release();
			}
			return buffer;
		}

		RECT lockRect{ startX, startY, startX + width, startY + height };
		D3DLOCKED_RECT locked{};
		if (FAILED(systemSurface->LockRect(&locked, &lockRect, D3DLOCK_READONLY))) {
			systemSurface->Release();
			return buffer;
		}

		if ((buffer->IsReady() || buffer->Init(width, height, cGZBufferColorType::A8R8G8B8, 32)) && buffer->Lock(cIGZBuffer::eLockFlags::IsDirtyUpdate)) {
			uint8_t const* row = reinterpret_cast<uint8_t const*>(locked.pBits);
			for (int32_t yy = 0; yy < height; yy++) {
				uint32_t const* pixels = reinterpret_cast<uint32_t const*>(row);
				for (int32_t xx = 0; xx < width; xx++) {
					buffer->SetPixel(xx, yy, 0xff000000 | (pixels[xx] & 0x00ffffff));
				}

				row += locked.Pitch;
			}

			buffer->Unlock(cIGZBuffer::eLockFlags::IsDirtyUpdate);
		}

		systemSurface->UnlockRect();
		systemSurface->Release();
		return buffer;
	}
}

