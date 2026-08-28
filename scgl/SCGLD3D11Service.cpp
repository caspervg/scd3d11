#include "SCGLD3D11Service.h"
#include "Diagnostics.h"

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace
{
	std::mutex g_callbackMutex;
	std::condition_variable g_callbackIdle;
	SCGLD3D11FrameCallback g_callback{};
	void* g_userData{};
	uint32_t g_callbacksInFlight{};
	thread_local bool g_insideCallback{};
	std::atomic<uint32_t> g_deviceGeneration{};
}

extern "C" BOOL __stdcall SCGLRegisterD3D11FrameCallback(
	SCGLD3D11FrameCallback callback, void* userData)
{
	if (callback == nullptr) return FALSE;
	nSCGL::Log(nSCGL::LogCategory::Grid, "SCGLRegisterD3D11FrameCallback(callback=%p, userData=%p)",
	           reinterpret_cast<void*>(callback), userData);
	std::lock_guard<std::mutex> const lock(g_callbackMutex);
	if (g_callback != nullptr) return g_callback == callback && g_userData == userData;
	g_callback = callback;
	g_userData = userData;
	return TRUE;
}

extern "C" BOOL __stdcall SCGLUnregisterD3D11FrameCallback(
	SCGLD3D11FrameCallback callback, void* userData)
{
	std::unique_lock<std::mutex> lock(g_callbackMutex);
	if (callback == nullptr || g_callback != callback || g_userData != userData) return FALSE;
	g_callback = nullptr;
	g_userData = nullptr;
	// A callback may unregister itself. In that case the call itself is the only
	// remaining execution barrier; an external unloader must still wait for its
	// callback invocation to return before unloading.
	if (!g_insideCallback) g_callbackIdle.wait(lock, [] { return g_callbacksInFlight == 0; });
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
			if (callback != nullptr) ++g_callbacksInFlight;
		}
		if (callback == nullptr) return;

		struct InvocationGuard {
			~InvocationGuard() {
				std::lock_guard<std::mutex> const lock(g_callbackMutex);
				--g_callbacksInFlight;
				g_callbackIdle.notify_all();
			}
		} guard;
		struct ActiveCallbackGuard {
			ActiveCallbackGuard() { g_insideCallback = true; }
			~ActiveCallbackGuard() { g_insideCallback = false; }
		} activeGuard;
		callback(&frame, userData);
	}
}
