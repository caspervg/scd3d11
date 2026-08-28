#include "SCGLD3D11Service.h"

#include <cassert>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace
{
	int calls;
	void* receivedUser;
	SCGLD3D11FrameContext received{};
	std::mutex blockingMutex;
	std::condition_variable blockingCondition;
	bool blockingCallbackStarted;
	bool releaseBlockingCallback;

	void __stdcall Callback(SCGLD3D11FrameContext const* frame, void* userData) {
		++calls;
		received = *frame;
		receivedUser = userData;
	}

	void __stdcall BlockingCallback(SCGLD3D11FrameContext const*, void*) {
		std::unique_lock<std::mutex> lock(blockingMutex);
		blockingCallbackStarted = true;
		blockingCondition.notify_all();
		blockingCondition.wait(lock, [] { return releaseBlockingCallback; });
	}
}

int main() {
	int owner;
	assert(!SCGLRegisterD3D11FrameCallback(nullptr, nullptr));
	assert(SCGLRegisterD3D11FrameCallback(Callback, &owner));
	assert(SCGLRegisterD3D11FrameCallback(Callback, &owner));
	SCGLD3D11FrameContext frame{ sizeof(frame), 1, SCGL_D3D11_EVENT_RENDER,
		nSCGL::NextD3D11DeviceGeneration(), nullptr, nullptr, nullptr, nullptr, nullptr };
	nSCGL::InvokeD3D11FrameCallback(frame);
	assert(calls == 1 && receivedUser == &owner);
	assert(received.structSize == sizeof(frame) && received.apiVersion == 1 && received.deviceGeneration == 1);
	assert(!SCGLUnregisterD3D11FrameCallback(Callback, nullptr));
	assert(SCGLUnregisterD3D11FrameCallback(Callback, &owner));
	nSCGL::InvokeD3D11FrameCallback(frame);
	assert(calls == 1);

	assert(SCGLRegisterD3D11FrameCallback(BlockingCallback, &owner));
	std::thread invocation([&] { nSCGL::InvokeD3D11FrameCallback(frame); });
	{
		std::unique_lock<std::mutex> lock(blockingMutex);
		blockingCondition.wait(lock, [] { return blockingCallbackStarted; });
	}
	std::atomic<bool> unregisterReturned{};
	std::thread unregistration([&] {
		assert(SCGLUnregisterD3D11FrameCallback(BlockingCallback, &owner));
		unregisterReturned = true;
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	assert(!unregisterReturned);
	{
		std::lock_guard<std::mutex> const lock(blockingMutex);
		releaseBlockingCallback = true;
	}
	blockingCondition.notify_all();
	invocation.join();
	unregistration.join();
	assert(unregisterReturned);
	return 0;
}
