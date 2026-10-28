/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include "wire/SharedRing.h"
#include "wire/WireStream.h"

#include <d3d11_4.h>
#include <wrl/client.h>

#include <cstdint>

namespace nSCD3D11 {
	// Renderer=server: SCD3D12Server.exe, next to SCD3D11.dll, draws the game in its own process. This side
	// starts it, streams the SimGL calls to it through a SharedRing and shows the frames it sends back.
	//
	// Anything going wrong (the server failing to start, dying, or falling a few seconds behind) makes this
	// inactive for the rest of the session, and cGDriver draws locally again.
	class RemoteRenderer {
	public:
		~RemoteRenderer();

		bool Start();
		bool Active() const { return active; }
		void Stop(char const *reason);

		// Where cGDriverWire writes the stream.
		nSCDWire::Writer::Sink Sink();
		// The adapter SC4's device runs on. The server waits for it before creating its own device.
		void SetAdapter(LUID adapter);

		// Waits for the server's frame `frame` and copies it into target. False when it did not come, and
		// the caller should draw this frame itself.
		bool CopyFrame(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Texture2D *target, uint64_t frame);
		// Once per Flush, whether or not CopyFrame ran, so the server can reuse the texture of `frame`.
		void EndFrame(ID3D11Device *device, ID3D11DeviceContext *context, uint64_t frame);

	private:
		bool ServerAlive() const;
		bool OpenGeneration(ID3D11Device *device);

		nSCDWire::SharedRing ring;
		HANDLE process = nullptr;
		HANDLE job = nullptr;
		bool active = false;
		uint32_t generation = 0;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> textures[2];
		Microsoft::WRL::ComPtr<ID3D11Fence> frameFence;
		Microsoft::WRL::ComPtr<ID3D11Fence> consumedFence;
	};
}
