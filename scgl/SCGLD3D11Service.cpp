#include "SCGLD3D11Service.h"

#include <atomic>
#include <mutex>

namespace
{
	std::mutex g_callbackMutex;
	SCGLD3D11FrameCallback g_callback{};
	void* g_userData{};
	std::atomic<uint32_t> g_deviceGeneration{};
}

extern "C" BOOL __stdcall SCGLRegisterD3D11FrameCallback(
	SCGLD3D11FrameCallback callback, void* userData)
{
	if (callback == nullptr) return FALSE;
	std::lock_guard<std::mutex> const lock(g_callbackMutex);
	if (g_callback != nullptr) return g_callback == callback && g_userData == userData;
	g_callback = callback;
	g_userData = userData;
	return TRUE;
}

extern "C" BOOL __stdcall SCGLUnregisterD3D11FrameCallback(
	SCGLD3D11FrameCallback callback, void* userData)
{
	std::lock_guard<std::mutex> const lock(g_callbackMutex);
	if (callback == nullptr || g_callback != callback || g_userData != userData) return FALSE;
	g_callback = nullptr;
	g_userData = nullptr;
	return TRUE;
}

namespace nSCGL
{
	uint32_t NextD3D11DeviceGeneration() {
		return g_deviceGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
	}

	void InvokeD3D11FrameCallback(SCGLD3D11FrameContext const& frame) {
		SCGLD3D11FrameCallback callback;
		void* userData;
		{
			std::lock_guard<std::mutex> const lock(g_callbackMutex);
			callback = g_callback;
			userData = g_userData;
		}
		if (callback != nullptr) callback(&frame, userData);
	}
}
