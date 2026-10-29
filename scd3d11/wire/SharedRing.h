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

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <windows.h>

// The shared memory between SCD3D11 in SimCity 4 (32-bit) and SCD3D12Server (64-bit): a one-way byte ring
// carrying the SCDW stream, and a small control block the server uses to hand back its frames.
//
// The ring has one writer and one reader. Each side only advances its own counter, and both counters only
// grow, so no locks are needed. Two events wake the other side: "data" when bytes arrive, "space" when bytes
// were consumed.
namespace nSCDWire {
	constexpr uint32_t kRingMagic = 0x52444353; // "SCDR"
	constexpr uint32_t kRingVersion = 1;

	struct alignas(8) RingControl {
		uint32_t magic;
		uint32_t version;
		uint64_t capacity;
		std::atomic<uint64_t> written;
		std::atomic<uint64_t> read;
		// Set by the writer when the stream ends on purpose.
		std::atomic<uint32_t> closed;
		// Frames, written by the server. Handles are already duplicated into the shim's process, and a new
		// generation means new ones, for example after a resize. Frame n (counting Flush calls from zero)
		// goes into frameTextures[n % 2]. The server signals frameFence to n + 1 once it is there, and the
		// shim signals consumedFence to n + 1 once it has copied it out, so the server can reuse the texture.
		std::atomic<uint32_t> frameGeneration;
		uint32_t frameWidth;
		uint32_t frameHeight;
		uint64_t frameTextures[2];
		uint64_t frameFence;
		uint64_t consumedFence;
		// The adapter SC4's own device runs on, written by the shim before the first frame. Shared
		// textures only work between devices on one adapter.
		uint32_t adapterLow;
		int32_t adapterHigh;
		std::atomic<uint32_t> adapterSet;
		// Set by the server once it is ready to read.
		std::atomic<uint32_t> serverReady;
		// ServerPresent=composition: the server presents its own swap chain on a DirectComposition surface
		// the shim shows on SC4's window. presentMode is written by the shim before the server starts, the
		// surface handle (already in the shim's process) and its generation by the server.
		uint64_t compositionSurface;
		std::atomic<uint32_t> compositionGeneration;
		uint32_t presentMode;
		// Set by the server while it draws the static view itself (ServerScene=owned), so the shim may leave out the
		// game's own draws of what the server keeps (ServerSkipModels).
		std::atomic<uint32_t> serverOwnsView;
		uint32_t reserved[3];
	};
	constexpr uint32_t kPresentCopy = 0;
	constexpr uint32_t kPresentComposition = 1;
	static_assert(std::atomic<uint64_t>::is_always_lock_free, "the ring needs lock-free 64-bit counters");
	static_assert(sizeof(RingControl) == 128, "both processes must agree on the layout");

	class SharedRing {
	public:
		~SharedRing();

		// Creates the mapping and events. name is used for the mapping and as a prefix for the events.
		bool Create(std::wstring const &name, uint64_t capacity);
		bool Open(std::wstring const &name);

		RingControl *Control() const { return control; }
		// Set by the server when a frame is ready. A Direct3D 11 fence does not wake its own waiters for
		// signals from another process, so the server's Direct3D 12 fence sets this one instead.
		HANDLE FrameEvent() const { return frameEvent; }

		// Writer. Blocks while the ring is full. alive is asked while waiting, so a dead reader
		// ends the wait. False when the reader is gone.
		bool Write(void const *data, size_t size, std::function<bool()> const &alive);
		void Close();

		// Reader. Fills exactly size bytes, waiting for the writer. False when the writer closed the
		// stream, or when alive returns false while waiting.
		bool Read(void *data, size_t size, std::function<bool()> const &alive);

	private:
		HANDLE mapping = nullptr;
		HANDLE dataEvent = nullptr;
		HANDLE spaceEvent = nullptr;
		HANDLE frameEvent = nullptr;
		RingControl *control = nullptr;
		uint8_t *bytes = nullptr;
		// The reader takes bytes out in chunks, so it signals the writer once per chunk.
		std::vector<uint8_t> chunk;
		size_t chunkOffset = 0;
	};
}
