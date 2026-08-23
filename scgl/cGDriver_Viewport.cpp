/*
 *  SCGL - a free graphics driver for SimCity 4's SimGL interface
 */

#include "cGDriver.h"
#include <cstring>

namespace nSCGL
{
	uint32_t cGDriver::CountVideoModes(void) const {
		return videoModeCount;
	}

	void cGDriver::GetVideoModeInfo(uint32_t dwIndex, sGDMode& gdMode) {
		if (dwIndex == -1 || dwIndex >= static_cast<uint32_t>(videoModeCount)) {
			SetLastError(DriverError::OUT_OF_RANGE);
			return;
		}

		gdMode = videoModes[dwIndex];
	}

	void cGDriver::GetVideoModeInfo(sGDMode& gdMode) {
		return GetVideoModeInfo(currentVideoMode, gdMode);
	}

	void cGDriver::SetVideoMode(int32_t newModeIndex, void* hwndProc, bool showWindow, bool) {
		if (newModeIndex == -1) {
			if (windowHandle != nullptr) {
				ShowWindow(static_cast<HWND>(windowHandle), SW_HIDE);
			}

			currentVideoMode = -1;
			windowWidth = 0;
			windowHeight = 0;
			SetLastError(DriverError::OK);
			return;
		}

		if (newModeIndex >= videoModeCount || d3d == nullptr) {
			SetLastError(DriverError::OUT_OF_RANGE);
			return;
		}

		sGDMode const& newMode = videoModes[newModeIndex];
		bool fullscreen = newMode.isFullscreen;

		currentVideoMode = newModeIndex;
		windowWidth = newMode.width;
		windowHeight = newMode.height;

		if (hwndProc == nullptr) {
			hwndProc = DefWindowProcA;
		}

		DestroyD3DDevice();

		DWORD dwStyle, dwExtStyle;
		RECT wndRect{ 0, 0, windowWidth, windowHeight };

		if (fullscreen) {
			dwStyle = WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_MAXIMIZE;
			dwExtStyle = WS_EX_APPWINDOW | WS_EX_TOPMOST;
		}
		else {
			dwStyle = WS_SYSMENU | WS_MINIMIZEBOX | WS_CAPTION | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
			dwExtStyle = WS_EX_APPWINDOW | WS_EX_WINDOWEDGE;
			AdjustWindowRectEx(&wndRect, dwStyle, FALSE, dwExtStyle);
			OffsetRect(&wndRect, 0, GetSystemMetrics(SM_CYCAPTION));
		}

		HWND hwnd = CreateWindowExA(
			dwExtStyle,
			"GDriverClass--Direct3D9",
			"GDriverWindow--Direct3D9",
			dwStyle,
			wndRect.left,
			wndRect.top,
			wndRect.right - wndRect.left,
			wndRect.bottom - wndRect.top,
			nullptr,
			nullptr,
			GetModuleHandle(nullptr),
			nullptr);

		if (hwnd == nullptr) {
			MessageBoxA(NULL, "Failed to create the Direct3D 9 window.", "SCGL video mode error", MB_ICONWARNING);
			return;
		}

		windowHandle = hwnd;

		memset(&presentParams, 0, sizeof(presentParams));
		presentParams.BackBufferWidth = windowWidth;
		presentParams.BackBufferHeight = windowHeight;
		presentParams.BackBufferFormat = newMode.depth == 16 ? D3DFMT_R5G6B5 : D3DFMT_X8R8G8B8;
		presentParams.BackBufferCount = 1;
		presentParams.MultiSampleType = D3DMULTISAMPLE_NONE;
		presentParams.SwapEffect = D3DSWAPEFFECT_DISCARD;
		presentParams.hDeviceWindow = hwnd;
		presentParams.Windowed = !fullscreen;
		presentParams.EnableAutoDepthStencil = TRUE;
		presentParams.AutoDepthStencilFormat = newMode.depth == 16 ? D3DFMT_D16 : D3DFMT_D24S8;
		presentParams.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

		DWORD behaviorFlags = D3DCREATE_FPU_PRESERVE;
		if (supportedFeatures.hardwareTransformAndLight) {
			behaviorFlags |= D3DCREATE_HARDWARE_VERTEXPROCESSING;
			if (supportedFeatures.pureDevice) {
				behaviorFlags |= D3DCREATE_PUREDEVICE;
			}
		}
		else {
			behaviorFlags |= D3DCREATE_SOFTWARE_VERTEXPROCESSING;
		}

		HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, behaviorFlags, &presentParams, &d3dDevice);
		if (FAILED(hr) && (behaviorFlags & D3DCREATE_HARDWARE_VERTEXPROCESSING)) {
			behaviorFlags &= ~(D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_PUREDEVICE);
			behaviorFlags |= D3DCREATE_SOFTWARE_VERTEXPROCESSING;
			hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, behaviorFlags, &presentParams, &d3dDevice);
		}

		if (FAILED(hr)) {
			MessageBoxA(NULL, "Failed to create the Direct3D 9 device.", "SCGL video mode error", MB_ICONWARNING);
			DestroyD3DDevice();
			return;
		}

		SetWindowLongPtr(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hwndProc));
		ShowWindow(hwnd, showWindow ? SW_SHOWNORMAL : SW_HIDE);

		state.SetDevice(d3dDevice);
		ConfigureD3DDeviceState();
		d3dDevice->BeginScene();

		SetViewport();
		SetLastError(DriverError::OK);
	}

	bool cGDriver::IsDeviceReady(void) {
		return d3dDevice != nullptr && d3dDevice->TestCooperativeLevel() != D3DERR_DEVICELOST;
	}

	void cGDriver::Flush(void) {
		if (d3dDevice != nullptr) {
			d3dDevice->EndScene();
			HRESULT hr = d3dDevice->Present(nullptr, nullptr, nullptr, nullptr);
			if (hr == D3DERR_DEVICELOST) {
				if (d3dDevice->TestCooperativeLevel() == D3DERR_DEVICENOTRESET && ResetD3DDevice()) {
					d3dDevice->BeginScene();
				}
				return;
			}

			d3dDevice->BeginScene();
		}
	}

	void cGDriver::ConfigureD3DDeviceState(void) {
		if (d3dDevice == nullptr) {
			return;
		}

		D3DMATRIX identity{};
		identity._11 = 1.0f;
		identity._22 = 1.0f;
		identity._33 = 1.0f;
		identity._44 = 1.0f;
		state.SetTransform(D3DTS_WORLD, identity);
		state.SetRenderState(D3DRS_LIGHTING, TRUE);
		state.SetRenderState(D3DRS_NORMALIZENORMALS, FALSE);
		state.SetRenderState(D3DRS_SPECULARENABLE, FALSE);

		D3DLIGHT9 light{};
		light.Type = D3DLIGHT_DIRECTIONAL;
		light.Direction.x = -1.0f;
		light.Direction.y = -1.0f;
		light.Direction.z = 0.0f;
		light.Diffuse.r = 1.0f;
		light.Diffuse.g = 1.0f;
		light.Diffuse.b = 1.0f;
		light.Diffuse.a = 1.0f;
		d3dDevice->SetLight(0, &light);
		d3dDevice->LightEnable(0, TRUE);
	}

	bool cGDriver::ResetD3DDevice(void) {
		if (d3dDevice == nullptr) {
			return false;
		}

		DeleteAllBufferRegions();
		state.SetDevice(nullptr);

		HRESULT hr = d3dDevice->Reset(&presentParams);
		if (FAILED(hr)) {
			state.SetDevice(d3dDevice);
			return false;
		}

		state.SetDevice(d3dDevice);
		ConfigureD3DDeviceState();
		SetViewport();
		return true;
	}

	void cGDriver::SetViewport(void) {
		SetViewport(0, 0, windowWidth, windowHeight);
	}

	void cGDriver::SetViewport(int32_t x, int32_t y, int32_t width, int32_t height) {
		if (d3dDevice != nullptr) {
			D3DVIEWPORT9 viewport{};
			viewport.X = x;
			viewport.Y = y;
			viewport.Width = width;
			viewport.Height = height;
			viewport.MinZ = 0.0f;
			viewport.MaxZ = 1.0f;
			d3dDevice->SetViewport(&viewport);

			RECT scissor{ x, y, x + width, y + height };
			d3dDevice->SetScissorRect(&scissor);
			state.SetRenderState(D3DRS_SCISSORTESTENABLE, x != 0 || y != 0 || width != windowWidth || height != windowHeight);
		}

		viewportX = x;
		viewportY = y;
		viewportWidth = width;
		viewportHeight = height;
	}

	void cGDriver::GetViewport(int32_t dimensions[4]) {
		dimensions[0] = viewportX;
		dimensions[1] = viewportY;
		dimensions[2] = viewportX + viewportWidth;
		dimensions[3] = viewportY + viewportHeight;
	}
}
