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
#include <cRZCOMDllDirector.h>
#include "cGDriver.h"
#include "LiveShadowHooks.h"
#include "Settings.h"
#include "ShadowRegistry.h"
#include "TerrainShadows.h"
#include "ThumbnailFocusGuard.h"

namespace nSCD3D11
{
	static const uint32_t kSCVKGDriverPluginCOMDirectorID = 0xCB6EC543;

	class cGDriversCOMDirector : public cRZCOMDllDirector
	{
	public:
		cGDriversCOMDirector()
		{
			AddCls(cGDriver::kSCD3D11GDriverGZCLSID, cGDriver::FactoryFunctionPtr2);
		}

	public:
		uint32_t GetDirectorID() const {
			return kSCVKGDriverPluginCOMDirectorID;
		}

		bool PreFrameWorkInit() {
			return true;
		}

		bool PreAppShutdown() {
			TerrainShadows::Uninstall();
			ShadowRegistry::Uninstall();
			LiveShadowHooks::Uninstall();
			ThumbnailFocusGuard::Uninstall();
			return true;
		}

		bool OnStart(cIGZCOM* pCOM) {
			Settings::Load();
			LiveShadowHooks::Install();
			if (LiveShadowHooks::Enabled()) ShadowRegistry::Install();
			TerrainShadows::Install();
			cIGZFrameWork* const pFramework = RZGetFrameWork();
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

		// The game's own DirectX driver has this class ID too; the higher
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
