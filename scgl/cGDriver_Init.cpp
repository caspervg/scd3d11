/*
 *  SCGL - a free graphics driver for SimCity 4's SimGL interface
 */

#include <cstring>
#include <string>
#include "cGDriver.h"

namespace nSCGL
{
	bool cGDriver::Init(void) {
		WNDCLASS wc{};
		wc.style = CS_OWNDC;
		wc.lpfnWndProc = DefWindowProcA;
		wc.hInstance = GetModuleHandle(nullptr);
		wc.lpszClassName = "GDriverClass--Direct3D9";

		UnregisterClass(wc.lpszClassName, nullptr);

		if (!RegisterClass(&wc)) {
			DWORD err = GetLastError();
			if (err != ERROR_CLASS_ALREADY_EXISTS) {
				MessageBoxA(NULL, "Failed to set up a Direct3D 9 window class", "SCGL failed to start", MB_ICONERROR);
				return false;
			}
		}

		d3d = Direct3DCreate9(D3D_SDK_VERSION);
		if (d3d == nullptr) {
			MessageBoxA(NULL, "Failed to create the Direct3D 9 interface", "SCGL failed to start", MB_ICONERROR);
			return false;
		}

		if (FAILED(d3d->GetDeviceCaps(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &deviceCaps))) {
			MessageBoxA(NULL, "Failed to query Direct3D 9 device capabilities", "SCGL failed to start", MB_ICONERROR);
			return false;
		}

		supportedFeatures.hardwareTransformAndLight = (deviceCaps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT) != 0;
		supportedFeatures.pureDevice = (deviceCaps.DevCaps & D3DDEVCAPS_PUREDEVICE) != 0;
		supportedFeatures.stencilBuffer = true;
		supportedFeatures.multitexture = deviceCaps.MaxSimultaneousTextures >= 2;
		supportedFeatures.textureEnvCombine = true;
		supportedFeatures.fogCoord = true;
		supportedFeatures.textureCompression =
			SUCCEEDED(d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE, D3DFMT_DXT1)) &&
			SUCCEEDED(d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE, D3DFMT_DXT3)) &&
			SUCCEEDED(d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE, D3DFMT_DXT5));
		supportedFeatures.nvTextureEnvCombine4 = false;
		supportedFeatures.bufferRegion = true;

		D3DADAPTER_IDENTIFIER9 adapterId{};
		d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &adapterId);

		static const char unknownDriverName[] = "Direct3D9";
		driverInfo.append(unknownDriverName, sizeof(unknownDriverName) - 1);
		driverInfo.append("\n9.0c\n", 6);
		driverInfo.append(adapterId.Description, strlen(adapterId.Description));
		driverInfo.append("\n", 1);
		driverInfo.append(adapterId.Driver, strlen(adapterId.Driver));
		driverInfo.append("\n", 1);
		driverInfo.append(adapterId.Description, strlen(adapterId.Description));
		driverInfo.append("\n", 1);

		InitializeVideoModeVector();
		return true;
	}

	bool cGDriver::Shutdown(void) {
		DestroyD3DDevice();

		if (d3d != nullptr) {
			d3d->Release();
			d3d = nullptr;
		}

		UnregisterClass("GDriverClass--Direct3D9", nullptr);
		return true;
	}

	void cGDriver::DestroyD3DDevice(void) {
		DeleteAllBufferRegions();

		state.SetDevice(nullptr);

		if (d3dDevice != nullptr) {
			d3dDevice->Release();
			d3dDevice = nullptr;
		}

		if (windowHandle != nullptr) {
			DestroyWindow(static_cast<HWND>(windowHandle));
			windowHandle = nullptr;
		}
	}

	void cGDriver::ReleaseTexture(uint32_t texture) {
		D3DTextureHandle* handle = reinterpret_cast<D3DTextureHandle*>(static_cast<uintptr_t>(texture));
		if (handle == nullptr) {
			return;
		}

		if (handle->texture != nullptr) {
			handle->texture->Release();
			handle->texture = nullptr;
		}

		delete handle;
	}

	int32_t cGDriver::InitializeVideoModeVector(void) {
		if (videoModeCount != 0) {
			videoModes.clear();
			videoModeCount = 0;
		}

		DEVMODE displayMode{};
		DWORD i = 0;

		while (EnumDisplaySettingsA(nullptr, i++, &displayMode)) {
			int depth = displayMode.dmBitsPerPel;
			if (depth < 15) {
				continue;
			}

			bool isDuplicate = false;
			for (sGDMode const& it : videoModes) {
				if (it.width == displayMode.dmPelsWidth && it.height == displayMode.dmPelsHeight && it.depth == depth) {
					isDuplicate = true;
					break;
				}
			}

			if (isDuplicate) {
				continue;
			}

			sGDMode tempMode{};
			tempMode.textureStageCount = MAX_TEXTURE_UNITS;
			tempMode.isInitialized = true;
			tempMode.supportsStencilBuffer = supportedFeatures.stencilBuffer;
			tempMode.supportsMultitexture = supportedFeatures.multitexture;
			tempMode.supportsTextureEnvCombine = supportedFeatures.textureEnvCombine;
			tempMode.supportsFogCoord = supportedFeatures.fogCoord;
			tempMode.supportsDxtTextures = supportedFeatures.textureCompression;
			tempMode.supportsNvTextureEnvCombine4 = supportedFeatures.nvTextureEnvCombine4;
			tempMode.is3DAccelerated = true;
			tempMode.hasBackingStore = true;
			tempMode.__unknown5[0] = false;
			tempMode.__unknown5[1] = false;
			tempMode.__unknown5[2] = false;

			if (depth > 16) {
				tempMode.alphaColorMask = 0xff000000;
				tempMode.redColorMask = 0x00ff0000;
				tempMode.greenColorMask = 0x0000ff00;
				tempMode.blueColorMask = 0x000000ff;
			}
			else {
				tempMode.alphaColorMask = 0x1;
				tempMode.redColorMask = 0xf800;
				tempMode.greenColorMask = 0x7c0;
				tempMode.blueColorMask = 0x3e;
			}

			tempMode.index = videoModeCount++;
			tempMode.width = displayMode.dmPelsWidth;
			tempMode.height = displayMode.dmPelsHeight;
			tempMode.depth = depth;
			tempMode.isFullscreen = true;
			videoModes.push_back(tempMode);

			tempMode.index = videoModeCount++;
			tempMode.isFullscreen = false;
			videoModes.push_back(tempMode);
		}

		return videoModeCount;
	}
}
