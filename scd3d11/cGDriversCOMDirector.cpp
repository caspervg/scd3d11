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

#include <cIGZFrameWork.h>
#include <cISC4App.h>
#include <cRZBaseString.h>
#include <cRZCOMDllDirector.h>
#include <GZServPtrs.h>
#include "cGDriver.h"
#include "cGDriverWire.h"
#include "Diagnostics.h"
#include "DrawOwnerHooks.h"
#include "FreeCamera.h"
#include "LiveShadowHooks.h"
#include "OccupantTracker.h"
#include "Settings.h"
#include "ShadowRegistry.h"
#include "TerrainShadows.h"
#include "ThumbnailFocusGuard.h"

namespace nSCD3D11
{
	static const uint32_t kSCVKGDriverPluginCOMDirectorID = 0xCB6EC543;

	// Tells the server which folders SC4 read its packages from, so its asset store holds the same data.
	static void RecordGameFolders()
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

	class cGDriversCOMDirector : public cRZCOMDllDirector
	{
	public:
		cGDriversCOMDirector()
		{
			AddCls(cGDriver::kSCD3D11GDriverGZCLSID, CreateDriver);
		}

	public:
		uint32_t GetDirectorID() const {
			return kSCVKGDriverPluginCOMDirectorID;
		}

		bool PreFrameWorkInit() {
			return true;
		}

		bool PostAppInit() {
			OccupantTracker::Install();
			FreeCamera::Install();
			RecordGameFolders();
			return true;
		}

		bool PreAppShutdown() {
			OccupantTracker::Uninstall();
			FreeCamera::Uninstall();
			DrawOwnerHooks::Uninstall();
			TerrainShadows::Uninstall();
			ShadowRegistry::Uninstall();
			LiveShadowHooks::Uninstall();
			ThumbnailFocusGuard::Uninstall();
			return true;
		}

		bool OnStart(cIGZCOM* pCOM) {
			cIGZFrameWork* const pFramework = RZGetFrameWork();
			Settings::Load(pFramework != nullptr ? pFramework->CommandLine() : nullptr);
			LiveShadowHooks::Install();
			if (LiveShadowHooks::Enabled()) ShadowRegistry::Install();
			TerrainShadows::Install();
			DrawOwnerHooks::Install();
			if (pFramework) {
				if (pFramework->GetState() < cIGZFrameWork::kStatePreAppInit) {
					pFramework->AddHook(this);
				}
				else {
					PreAppInit();
				}
			}
			return true;
		}

		// The game's own DirectX driver has this class ID too. The higher
		// version is what makes SC4 pick SCD3D11.
		void EnumClassObjects(ClassObjectEnumerationCallback pCallback, void* pContext) {
			pCallback(cGDriver::kSCD3D11GDriverGZCLSID, 1000000, pContext);
		}
	};
}

cRZCOMDllDirector* RZGetCOMDllDirector() {
	static nSCD3D11::cGDriversCOMDirector sDirector;
	return &sDirector;
}
