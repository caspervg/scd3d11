/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#include "GameFolders.h"

#include "cGDriverWire.h"
#include "Diagnostics.h"

#include <GZServPtrs.h>
#include <cISC4App.h>
#include <cRZBaseString.h>

#include <string>

namespace nSCD3D11 {
	// Tells the server which folders SC4 read its packages from, so its asset store holds the same data.
	void RecordGameFolders()
	{
		cGDriverWire* const wire = cGDriverWire::Current();
		cISC4AppPtr app;
		if (wire == nullptr || !app) return;
		cRZBaseString paths[static_cast<size_t>(nSCDWire::GameFolder::Count)];
		// The same calls cSC4App::SetupResources (0x004572B0) makes. What they return is not a reliable flag,
		// so a folder counts as named when its path is not empty.
		app->GetDataDirectory(paths[0], -1);
		app->GetDataDirectory(paths[1], 0);
		app->GetSkuSpecificDirectory(paths[2]);
		app->GetPluginDirectory(paths[3]);
		app->GetUserPluginDirectory(paths[4]);
		if (paths[0].Strlen() == 0) {
			Log(LogCategory::Initialization, "server: SC4 did not name its install folder");
			return;
		}
		std::wstring folders[static_cast<size_t>(nSCDWire::GameFolder::Count)];
		for (size_t i = 0; i < std::size(paths); ++i) {
			// SC4 keeps paths in the ANSI code page, with a trailing separator.
			std::string path(paths[i].ToChar(), paths[i].Strlen());
			while (!path.empty() && (path.back() == '\\' || path.back() == '/')) path.pop_back();
			Log(LogCategory::Initialization, "server: game folder %zu is %s", i, path.c_str());
			int const units = MultiByteToWideChar(CP_ACP, 0, path.data(), static_cast<int>(path.size()), nullptr, 0);
			folders[i].resize(units);
			MultiByteToWideChar(CP_ACP, 0, path.data(), static_cast<int>(path.size()), folders[i].data(), units);
		}
		wire->RecordGameFolders(folders);
	}

}
