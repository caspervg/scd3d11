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

#include <GZCLSIDDefs.h>
#include <GZServPtrs.h>
#include <cIGZCheatCodeManager.h>
#include <cIGZFrameWork.h>
#include <cIGZMessage2Standard.h>
#include <cIGZMessageTarget2.h>
#include <cISC4App.h>
#include <cRZBaseString.h>
#include <cRZCOMDllDirector.h>
#include <SC4NotificationDialog.h>
#include "cGDriver.h"
#include "AmdRingFix.h"
#include "CrashDump.h"
#include "Doctor.h"
#include "LiveShadowHooks.h"
#include "Settings.h"
#include "ShadowRegistry.h"
#include "TerrainShadows.h"
#include "ThumbnailFocusGuard.h"

namespace nSCD3D11
{
	static const uint32_t kSCVKGDriverPluginCOMDirectorID = 0xCB6EC543;

	// SCD3D11's cheats, typed in SC4's cheat box (Ctrl+X). The box sends kCheatIssued with the cheat's ID in data 1.
	constexpr uint32_t kCheatIssued = 0x230E27AC;
	constexpr uint32_t kLiveShadowsCheat = 0x3D11C4EA;
	constexpr uint32_t kFrameRateCheat = 0x3D11F9A7;

	// Lives as long as the game, so it does not count references.
	class CheatTarget final : public cIGZMessageTarget2
	{
	public:
		bool QueryInterface(uint32_t riid, void** object) override {
			if (riid != GZIID_cIGZUnknown && riid != static_cast<uint32_t>(GZCLSID::kcIGZMessageTarget2)) {
				*object = nullptr;
				return false;
			}
			*object = static_cast<cIGZMessageTarget2*>(this);
			return true;
		}

		uint32_t AddRef() override { return 1; }
		uint32_t Release() override { return 1; }

		bool DoMessage(cIGZMessage2* message) override {
			if (message == nullptr || message->GetType() != kCheatIssued) return true;
			uint32_t const cheat = static_cast<cIGZMessage2Standard*>(message)->GetData1();
			if (cheat == kLiveShadowsCheat && LiveShadowHooks::Enabled()) LiveShadowHooks::TogglePause();
			else if (cheat == kLiveShadowsCheat) {
				// The patches only go in at startup, and SC4 makes its shadow records as a city loads.
				SC4NotificationDialog::ShowDialog(
					cRZBaseString("Live shadows are off in SCD3D11.ini. Set LiveShadows=on and restart SimCity 4, "
					              "then this cheat pauses and resumes them."),
					cRZBaseString("SCD3D11"));
			}
			else if (cheat == kFrameRateCheat) cGDriver::ToggleFrameRate();
			return true;
		}

		void Register() {
			cISC4AppPtr app;
			cIGZCheatCodeManager* const cheats = app ? app->GetCheatCodeManager() : nullptr;
			if (cheats == nullptr || !cheats->AddNotification2(this, 0)) return;
			cheats->RegisterCheatCode(kLiveShadowsCheat, cRZBaseString("LiveShadows"));
			// SC4's own fps cheat already exists.
			cheats->RegisterCheatCode(kFrameRateCheat, cRZBaseString("FrameRate"));
		}
	} gCheatTarget;

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

		// SC4 has picked its driver and screen mode by now, so the doctor can see what it ended up with.
		bool PostAppInit() {
			cIGZFrameWork* const pFramework = RZGetFrameWork();
			Doctor::Run(pFramework != nullptr ? pFramework->CommandLine() : nullptr);
			gCheatTarget.Register();
			return true;
		}

		bool PreAppShutdown() {
			TerrainShadows::Uninstall();
			ShadowRegistry::Uninstall();
			LiveShadowHooks::Uninstall();
			ThumbnailFocusGuard::Uninstall();
			CrashDump::Uninstall();
			AmdRingFix::LogCounts();
			return true;
		}

		bool OnStart(cIGZCOM* pCOM) {
			cIGZFrameWork* const pFramework = RZGetFrameWork();
			Settings::Load(pFramework != nullptr ? pFramework->CommandLine() : nullptr);
			LiveShadowHooks::Install();
			if (LiveShadowHooks::Enabled()) ShadowRegistry::Install();
			TerrainShadows::Install();
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
