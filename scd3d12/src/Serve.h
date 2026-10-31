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

#include "cGDriver12.h"
#include "WorldLight.h"

#include <string>

#include <windows.h>

namespace nSCD3D12 {
	// --scene: off, substitute (the scene lane draws what it can from the game's data), check (and compares) or
	// owned (the server keeps the whole static view, OwnedScene.h).
	enum class SceneMode : uint8_t { Off, Substitute, Check, Owned };

	// Draws the game whose SCD3D11 created the shared ring ringName, until the game exits. A non-null
	// relight relights every frame with that sun, and a non-null light adds light from the server's own copy of
	// the city to it (WorldLight.h). With logStats, logs the frame rate and the CPU and GPU time per frame every
	// second. depth16 is SCD3D11's Depth16 setting (cGDriver12::Depth16).
	int RunServe(std::wstring const &ringName, DWORD parentId, cGDriver12::Sun const *relight, bool logStats,
	             SceneMode scene = SceneMode::Off, WorldLight::Options const *light = nullptr, bool depth16 = true);

	// Plays a capture into `serve` the way SCD3D11 streams the game, to try the live path offline. It stands in
	// for the shim: it makes the shared ring, names the adapter, starts this program with serve and the given
	// arguments, writes the capture into the ring and hands back each frame as copied out. With savePath it
	// saves frame saveFrame (counting Flush calls from zero) as the shim would show it.
	int RunFeed(std::wstring const &capture, std::wstring const &serveArguments, uint64_t saveFrame,
	            std::wstring const &savePath);
}
