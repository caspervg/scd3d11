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

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class cIGZCmdLine;

// Looks at the rest of the game for things known to stop SCD3D11 or confuse players: other graphics
// drivers, DirectX wrappers, plugins that take over the window or resolution, compatibility mode and
// mistyped launch switches. Each finding becomes one line in SCD3D11.log that says what to change.
namespace nSCD3D11::Doctor {
	enum class Severity { Note, Warning, Problem };

	struct Finding {
		Severity severity;
		std::string text;
	};

	// A DLL by full path, with the product name and version from its version resource. Both are empty
	// when it has none.
	struct Module {
		std::string path;
		std::string product;
		std::string version;
	};

	struct Size {
		uint32_t width = 0;
		uint32_t height = 0;
	};

	// Everything the checks read, gathered in one place so they can be tested without a game.
	struct Environment {
		// SC4's arguments as it split them, without the program path.
		std::vector<std::string> arguments;
		// The command line as Windows passed it, quotes and all.
		std::string rawCommandLine;
		// DLLs loaded into the game.
		std::vector<Module> modules;
		// Copies of Windows graphics DLLs in the Apps folder, loaded or not.
		std::vector<Module> appsFiles;
		// The __COMPAT_LAYER variable Windows sets when the exe runs in compatibility mode.
		std::string compatibilityLayer;
		// SC4GraphicsOptions.ini by lower-case key, when SC4GraphicsOptions.dll is loaded. A key the
		// INI lacks is absent.
		std::optional<std::map<std::string, std::string>> graphicsOptions;
		// -UserDir names a folder that does not exist.
		bool userDirMissing = false;
		// The screen modes Windows offers, and the screen size.
		std::vector<Size> displayModes;
		Size screen;
		// What SC4 ended up with. Unknown when the graphics system could not be asked.
		std::optional<bool> driverIsSCD3D11;
		std::string activeDriverModule;
		std::optional<Size> gameSize;
		bool gameFullScreen = false;
		bool gameVersion641 = true;
	};

	std::vector<Finding> Diagnose(Environment const &environment);

	// A DLL some check is about, whose version is always worth logging.
	bool IsWatched(std::string const &path);

	// The version in a GitHub "latest release" reply, without the v of its tag. Empty when there is none.
	std::string ReleaseVersion(std::string_view json);
	// Whether a dotted version is newer than another. Missing parts count as 0.
	bool IsNewer(std::string const &version, std::string const &than);

	// Gathers the environment and logs the findings. With Doctor=on it also logs every DLL and
	// relevant INI value it saw. Called once SC4 has set up its graphics.
	void Run(cIGZCmdLine *commandLine);
}
