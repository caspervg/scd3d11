/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2025  Nelson Gomez (nsgomez) <nelson@ngomez.me>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, see <https://www.gnu.org/licenses/>.
 */

#pragma once
#include <cIGZUnknown.h>

static const uint32_t GZIID_cIGZGAsyncSnapshotExtension = 0xA5C1D3E7;

// Reads rectangles of the back buffer without making the CPU wait for the GPU at the call. A copy is
// queued behind the draws already submitted, so later draws do not change it. Pixels are tightly packed
// RGBA8 rows (R, G, B, then A = 255), top row first, in game window pixels with a top-left origin, the same
// rectangle convention as CopyColorBuffer. Drivers that answer this IID also declare
// cIGZBuffer::GetColorType the way the game implements it, so callers may pass a ready `reuse` buffer to
// CopyColorBuffer.
//
// Not a game interface: SCD3D11 adds it for plugins. Other drivers do not answer the IID.
class cIGZGAsyncSnapshotExtension : public cIGZUnknown
{
public:
	// 1 for this layout.
	virtual uint32_t GetAsyncSnapshotVersion() = 0;
	// Queues a copy of the rectangle. Returns a ticket (never 0), or 0 when the rectangle is not inside the
	// back buffer, kMaxPending (4) copies are in flight, or the device is not ready. Never waits.
	virtual uint32_t BeginCopy(int32_t x, int32_t y, int32_t width, int32_t height) = 0;
	// Waits up to timeoutMs for the ticket's copy, then writes width * height * 4 bytes to pixels. False when
	// the ticket is unknown, capacity is too small, the wait timed out or the device was lost. The ticket is
	// released either way.
	virtual bool EndCopy(uint32_t ticket, uint8_t* pixels, uint32_t capacity, uint32_t timeoutMs) = 0;
	// Releases a ticket without reading it. Unknown tickets are ignored.
	virtual void CancelCopy(uint32_t ticket) = 0;
};
