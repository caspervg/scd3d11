#pragma once

#include <cstdint>
#include <d3d11.h>
#include <dxgi.h>
#include <Windows.h>

enum SCD3D11Event : uint32_t
{
	// Raised once per presented frame. It is tied to presentation, not to the game's frame loop,
	// so it does not arrive while there is nothing to present into: a minimized window (which is
	// what DXGI does to an exclusive-fullscreen window on alt-tab), a fully occluded one, or a
	// device that is being recreated. Do not drive timing from its arrival rate.
	SCD3D11_EVENT_RENDER = 1,
	SCD3D11_EVENT_BEFORE_DEVICE_DESTROY = 2,
};

struct SCD3D11FrameContext
{
	uint32_t structSize;
	uint32_t apiVersion;
	SCD3D11Event event;
	uint32_t deviceGeneration;
	ID3D11Device* device;
	ID3D11DeviceContext* context;
	IDXGISwapChain* swapChain;
	ID3D11RenderTargetView* renderTargetView;
	HWND window;
};

using SCD3D11FrameCallback = void(__stdcall*)(SCD3D11FrameContext const* frame, void* userData);

extern "C" __declspec(dllexport) BOOL __stdcall SCD3D11RegisterFrameCallback(
	SCD3D11FrameCallback callback, void* userData);
extern "C" __declspec(dllexport) BOOL __stdcall SCD3D11UnregisterFrameCallback(
	SCD3D11FrameCallback callback, void* userData);

namespace nSCD3D11
{
	uint32_t NextD3D11DeviceGeneration();
	void InvokeD3D11FrameCallback(SCD3D11FrameContext const& frame);
}
