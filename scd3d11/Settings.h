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

#include <string>
#include <string_view>

class cIGZCmdLine;

// SCD3D11's settings. Each is looked up on SC4's command line first (-Name or
// -Name:value) and then in SCD3D11.ini next to the DLL (Name=value under
// [SCD3D11]), so the INI holds a player's usual settings and a switch
// overrides them for one session.
namespace nSCD3D11::Settings {
	// Every setting, read once by Load. Until then, and in the tests, the
	// defaults below apply.
	struct Values {
		bool liveShadows = false;
		bool networkShadowMasks = false;
		bool vsync = true;
		bool borderless = false;
		bool flipModel = true;
		// GPU=default: the adapter Windows picks instead of the high-performance one.
		bool defaultAdapter = false;
		bool viewportCull = true;
		bool reshade = true;
		bool drawStats = false;
		// LogLevel=trace.
		bool traceLog = false;
		// Record=<file>: write every SimGL call to this file as an SCDW stream (see wire/Wire.h).
		std::string record;
		// Renderer=server: draw in SCD3D12Server.exe instead of in the game (Renderer=local).
		bool server = false;
		// ServerPresent=composition: the server presents itself on SC4's window through DirectComposition
		// instead of handing each frame back to be presented here (ServerPresent=copy).
		bool serverComposition = false;
		// ServerRelight=<hour> or <azimuth>,<elevation>: the server relights every frame with that sun
		// (see SCD3D12Server's --relight). Empty or off draws the game's own light.
		std::string serverRelight;
		// ServerLight=rays,terrain,lamps,world (any of them): with ServerRelight, light from the server's own copy
		// of the city (see SCD3D12Server's --light). rays and lamps need ServerScene=owned. Empty or off for none.
		std::string serverLight;
		// ServerScene=substitute: the server draws the city's models, networks and lot textures from its own
		// copy of the game's data (see SCD3D12Server's --scene). check also compares each with the game's. owned
		// keeps the whole static view on the server and draws it every frame without SC4's backing store.
		std::string serverScene;
		// ServerSkipModels=on: with ServerScene=owned, the game's static model draws are left out while the server
		// draws the static view, and only their ModelInstance records are sent.
		bool serverSkipModels = false;
		// FreeCamera=on: drag with the middle mouse button to turn and tilt the city view, scroll for smooth zoom,
		// hold ALT for the classic angle (see FreeCamera.h). Works with every renderer.
		bool freeCamera = false;
	};

	// Reads every setting from SC4's command line (may be null) and the INI.
	// Called once from the COM director's OnStart, before SC4 starts the
	// threads that read them, so they need no locking.
	void Load(cIGZCmdLine *commandLine);

	Values const &Get();

	// Exposed for tests: reads value as a flag the way sc4-dll-utilities does
	// (on/off, true/false, enabled/disabled, 1/0, any case), with an empty
	// value - a bare -Name - meaning on. False when it is none of those.
	bool ParseFlag(std::string_view value, bool &flag);
	// Exposed for tests: ServerLight, off or a comma separated list of rays, terrain, lamps and world.
	bool ParseServerLight(std::string_view value, std::string &features);
} // namespace nSCD3D11::Settings
