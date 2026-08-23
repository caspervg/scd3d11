#pragma once

#include <cstdint>
#include <d3d11.h>
#include <dxgi.h>
#include <Windows.h>

enum SCGLD3D11Event : uint32_t
{
	SCGL_D3D11_EVENT_RENDER = 1,
	SCGL_D3D11_EVENT_BEFORE_DEVICE_DESTROY = 2,
};

struct SCGLD3D11FrameContext
{
	uint32_t structSize;
	uint32_t apiVersion;
	SCGLD3D11Event event;
	uint32_t deviceGeneration;
	ID3D11Device* device;
	ID3D11DeviceContext* context;
	IDXGISwapChain* swapChain;
	ID3D11RenderTargetView* renderTargetView;
	HWND window;
};

using SCGLD3D11FrameCallback = void(__stdcall*)(SCGLD3D11FrameContext const* frame, void* userData);

extern "C" __declspec(dllexport) BOOL __stdcall SCGLRegisterD3D11FrameCallback(
	SCGLD3D11FrameCallback callback, void* userData);
extern "C" __declspec(dllexport) BOOL __stdcall SCGLUnregisterD3D11FrameCallback(
	SCGLD3D11FrameCallback callback, void* userData);

namespace nSCGL
{
	uint32_t NextD3D11DeviceGeneration();
	void InvokeD3D11FrameCallback(SCGLD3D11FrameContext const& frame);
}
