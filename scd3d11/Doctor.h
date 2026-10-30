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

	// A GPU Windows reports, and whether SCD3D11 draws on it.
	struct Gpu {
		std::string name;
		uint32_t dedicatedMB = 0;
		bool inUse = false;
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
		// File names of DLLs in the Plugins folders that import imgui.dll, as plugins built for
		// SC4RenderServices before v0.8 do. Loaded or not, since they fail to load once imgui.dll is gone.
		std::vector<std::string> imguiPlugins;
		// The __COMPAT_LAYER variable Windows sets when the exe runs in compatibility mode.
		std::string compatibilityLayer;
		// Where it can come from: this exe's own Properties, Compatibility settings for the current user
		// and for all users. When neither asks for a mode, the program that started SC4 passed it on.
		std::string exePath;
		std::string compatibilityLayerUser;
		std::string compatibilityLayerAllUsers;
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
		// The driver's GZCOM class ID, and whether it is one of SC4's own inside the game exe.
		uint32_t activeDriverClass = 0;
		bool activeDriverBuiltIn = false;
		std::optional<Size> gameSize;
		bool gameFullScreen = false;
		bool gameVersion641 = true;
		// The exe has the large-address-aware flag the 4GB patch sets, so it can use 4 GB instead of 2 GB.
		bool largeAddressAware = true;
		// The Wine version and its host system, such as Linux, when the game runs under Wine or Proton.
		std::string wineVersion;
		std::string wineHost;
		// What SCD3D11 did when SC4 asked for a full-screen size the screen lacks. Empty when it did not.
		std::string screenModeWarning;
		// Where Windows lives, whose own DLLs are never a plugin's duplicate.
		std::string windowsFolder;
		// The GPUs in the machine, without Windows' software renderer, in the order GPU=<n> counts them.
		std::vector<Gpu> gpus;
		// The GPU setting: GPU=default, or the number from GPU=<n>, 0 when it has none.
		bool gpuSystemDefault = false;
		uint32_t gpuNumber = 0;
	};

	std::vector<Finding> Diagnose(Environment const &environment);

	// Whether the screen offers a size for full screen. True when the list of sizes is unknown.
	bool ScreenOffers(std::vector<Size> const &modes, uint32_t width, uint32_t height);
	// The largest sizes the screen offers, for a message.
	std::string LargestSizes(std::vector<Size> modes);

	// Called at the end of cGDriver::Init, just before SC4 picks its screen mode. Full screen without a
	// size from -r or SC4GraphicsOptions gets the screen's own size. A size or colour depth the screen lacks
	// would make SC4 close without a word, so it gets the screen's own size too and the report says why.
	// When SC4 does not take the new size, a message box says why first, since no game window exists yet.
	void CheckRequestedScreenMode();

	// Called once SCD3D11 has its device, with the LUID of the GPU it draws on, so the report can name it.
	void SetActiveAdapter(uint64_t luid);

	// A DLL some check is about, whose version is always worth logging.
	bool IsWatched(std::string const &path);

	// The version in a GitHub "latest release" reply, without the v of its tag. Empty when there is none.
	std::string ReleaseVersion(std::string_view json);
	// Whether a dotted version is newer than another. Missing parts count as 0.
	bool IsNewer(std::string const &version, std::string const &than);
	// A version as players read it: 1.4.1.0 becomes v1.4.1.
	std::string VersionLabel(std::string version);

	// Gathers the environment and logs the findings. With Doctor=on it also logs every DLL and
	// relevant INI value it saw. Called once SC4 has set up its graphics.
	void Run(cIGZCmdLine *commandLine);
}
