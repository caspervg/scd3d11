#include "SCGLD3D11Service.h"

#include <cassert>

namespace
{
	int calls;
	void* receivedUser;
	SCGLD3D11FrameContext received{};

	void __stdcall Callback(SCGLD3D11FrameContext const* frame, void* userData) {
		++calls;
		received = *frame;
		receivedUser = userData;
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
	return 0;
}
