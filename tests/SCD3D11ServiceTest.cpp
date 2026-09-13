#include "SCD3D11Service.h"

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
	SCD3D11FrameContext received{};
	std::mutex blockingMutex;
	std::condition_variable blockingCondition;
	bool blockingCallbackStarted;
	bool releaseBlockingCallback;

	void __stdcall Callback(SCD3D11FrameContext const* frame, void* userData) {
		++calls;
		received = *frame;
		receivedUser = userData;
	}

	void __stdcall BlockingCallback(SCD3D11FrameContext const*, void*) {
		std::unique_lock<std::mutex> lock(blockingMutex);
		blockingCallbackStarted = true;
		blockingCondition.notify_all();
		blockingCondition.wait(lock, [] { return releaseBlockingCallback; });
	}
}

int main() {
	int owner;
	assert(!SCD3D11RegisterFrameCallback(nullptr, nullptr));
	assert(SCD3D11RegisterFrameCallback(Callback, &owner));
	assert(SCD3D11RegisterFrameCallback(Callback, &owner));
	SCD3D11FrameContext frame{ sizeof(frame), 1, SCD3D11_EVENT_RENDER,
		nSCD3D11::NextD3D11DeviceGeneration(), nullptr, nullptr, nullptr, nullptr, nullptr };
	nSCD3D11::InvokeD3D11FrameCallback(frame);
	assert(calls == 1 && receivedUser == &owner);
	assert(received.structSize == sizeof(frame) && received.apiVersion == 1 && received.deviceGeneration == 1);
	assert(!SCD3D11UnregisterFrameCallback(Callback, nullptr));
	assert(SCD3D11UnregisterFrameCallback(Callback, &owner));
	nSCD3D11::InvokeD3D11FrameCallback(frame);
	assert(calls == 1);

	assert(SCD3D11RegisterFrameCallback(BlockingCallback, &owner));
	std::thread invocation([&] { nSCD3D11::InvokeD3D11FrameCallback(frame); });
	{
		std::unique_lock<std::mutex> lock(blockingMutex);
		blockingCondition.wait(lock, [] { return blockingCallbackStarted; });
	}
	std::atomic<bool> unregisterReturned{};
	std::thread unregistration([&] {
		assert(SCD3D11UnregisterFrameCallback(BlockingCallback, &owner));
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
