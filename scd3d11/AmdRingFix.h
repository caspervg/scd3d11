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

#include <cstddef>
#include <cstdint>
#include <optional>

// A fix for a crash in AMD's 32-bit D3D11 driver (amdxx32.dll, seen on 32.0.32015.7029 and older).
//
// The driver records D3D11 calls on SC4's thread into a ring of 1 MB segments, and a worker thread of its own replays
// them. Each record is a dword opcode and a payload. When a record reaches the end of a segment, the recording side
// writes a trailer {0x7890ABCD, next segment} and wraps. Every replay handler checks for that end and wraps too, except
// the one for opcode 0x3E, which has no payload. When a 0x3E lands exactly on a segment end, the worker reads the
// trailer as commands and then replays stale bytes, crashing at a different place each time.
//
// Install points the replay table's 0x3E slot at a handler that does the missing wrap and then calls AMD's. It only
// does so when the driver's code matches the layout this was written against byte for byte, so a driver that fixes
// the bug, or lays the ring out differently, is left alone.
namespace nSCD3D11::AmdRingFix {
	enum class Result {
		NotAmd,      // amdxx32.dll is not loaded.
		NoMatch,     // The driver's code differs from the known layout. Nothing changed.
		Patched,
		AlreadyPatched,
		Failed,      // The layout matched, but the table could not be written.
	};

	// After SCD3D11's device exists. Does nothing unless the AMDCrashFix setting is on (the default).
	void Install();

	// Install without looking at the setting, for tests.
	Result Apply();

	// At shutdown: how often the fix ran, and how often it wrapped where AMD's handler would have crashed.
	void LogCounts();

	// The rest is exposed for tests.

	// The replay loop's dispatch in a code range: the address of the opcode handler table and the opcode count.
	// Requires the exact read-loop and dispatch bytes this fix was written against, found exactly once.
	struct Dispatch {
		uintptr_t table = 0;
		uint32_t opcodes = 0;
	};
	std::optional<Dispatch> FindDispatch(uint8_t const *code, size_t size);

	// Whether a handler's code is the payloadless handler that returns without the wrap step.
	bool IsUnwrappedHandler(uint8_t const *code, size_t size);

	// Whether a handler's code contains the wrap step at the offsets the fix assumes.
	bool HasWrapStep(uint8_t const *code, size_t size);

	// The wrap step every other handler ends with. `context` is the worker's ring context. True when it wrapped.
	bool WrapAtSegmentEnd(uint8_t *context);

	// How often the worker ran the replacement handler, and how often it had to wrap.
	struct Counts {
		uint32_t calls = 0;
		uint32_t wraps = 0;
	};
	Counts GetCounts();

	// The replay handler table Apply patched, or null.
	void *const *PatchedHandlerSlot();
} // namespace nSCD3D11::AmdRingFix
