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
#include <cIGZMessage2.h>
#include <cIGZMessage2Standard.h>
#include <cIGZMessageServer2.h>
#include <cIGZMessageTarget2.h>
#include <cRZCOMDllDirector.h>
#include <GZCLSIDDefs.h>
#include "cGDriver.h"
#include "CpuScheduling.h"
#include "Diagnostics.h"
#include "StartupProgress.h"
#include "StartupResourceLoadPatches.h"
#include "ParallelRenderCull.h"
#include "SimTickBudget.h"

namespace nSCD3D11
{
	static const uint32_t kSCVKGDriverPluginCOMDirectorID = 0xCB6EC543;
	static constexpr uint32_t kSC4MessageCityLoadFinished = 0x0EA8AE29;
	static constexpr uint32_t kSC4MessagePostRegionInit = 0xCBB5BB45;
	static constexpr uint32_t kSC4MessagePreRegionShutdown = 0x8BB5BB46;
	static const uint32_t kGZMessageServer2ServiceId = 83526747ul;
	static const uint32_t kGZMessageServer2InterfaceId = 1696765127ul;

	class cGDriversCOMDirector : public cRZCOMSlimDllDirector, public cIGZMessageTarget2
	{
	private:
		cIGZMessageServer2 *messageServer = nullptr;

	public:
		cGDriversCOMDirector() :
			cRZCOMSlimDllDirector(cGDriver::kSCD3D11GDriverGZCLSID, cGDriver::FactoryFunctionPtr2)
		{
		}

	public:
		uint32_t GetDirectorID() const {
			return kSCVKGDriverPluginCOMDirectorID;
		}

		bool PreFrameWorkInit() {
			return true;
		}

		bool QueryInterface(uint32_t riid, void **ppvObj) override {
			if (ppvObj == nullptr) return false;
			if (riid == GZCLSID::kcIGZMessageTarget2) {
				*ppvObj = static_cast<cIGZMessageTarget2 *>(this);
				AddRef();
				return true;
			}
			return cRZCOMSlimDllDirector::QueryInterface(riid, ppvObj);
		}

		uint32_t AddRef(void) override {
			return cRZCOMSlimDllDirector::AddRef();
		}

		uint32_t Release(void) override {
			return cRZCOMSlimDllDirector::Release();
		}

		bool DoMessage(cIGZMessage2 *message) override {
			if (message == nullptr) return false;
			uint32_t const type = message->GetType();
			Log(LogCategory::Grid, "message2 type=0x%08X%s%s%s", type,
			    type == kSC4MessagePostRegionInit ? " (post-region-init)" : "",
			    type == kSC4MessagePreRegionShutdown ? " (pre-region-shutdown)" : "",
			    type == kSC4MessageCityLoadFinished ? " (city-load-finished)" : "");
			if (type == kSC4MessagePostRegionInit) {
				cGDriver::MarkStartupComplete();
			}
			void *standardObject = nullptr;
			if (message->QueryInterface(GZCLSID::kcIGZMessage2Standard, &standardObject)) {
				auto *standard = static_cast<cIGZMessage2Standard *>(standardObject);
				Log(LogCategory::Grid, "message2 standard data=%p/%p/%p/%p",
				    standard->GetHasData1() ? reinterpret_cast<void *>(standard->GetData1()) : nullptr,
				    standard->GetHasData2() ? reinterpret_cast<void *>(standard->GetData2()) : nullptr,
				    standard->GetHasData3() ? reinterpret_cast<void *>(standard->GetData3()) : nullptr,
				    standard->GetHasData4() ? reinterpret_cast<void *>(standard->GetData4()) : nullptr);
				standard->Release();
			}
			return true;
		}

		bool PostAppInit() {
			StartupProgress::MarkResourceLoadingComplete();
			cGDriver::MarkPostAppInit();
			cIGZFrameWork *const framework = RZGetFrameWork();
			if (framework != nullptr) {
				void *service = nullptr;
				if (framework->GetSystemService(kGZMessageServer2ServiceId,
				                                kGZMessageServer2InterfaceId, &service) && service != nullptr) {
					messageServer = static_cast<cIGZMessageServer2 *>(service);
					bool const regionSubscribed = messageServer->AddNotification(this, kSC4MessagePostRegionInit);
					bool const shutdownSubscribed = messageServer->AddNotification(this, kSC4MessagePreRegionShutdown);
					bool const citySubscribed = messageServer->AddNotification(this, kSC4MessageCityLoadFinished);
					Log(LogCategory::Grid, "message subscriptions: post-region-init=%s pre-region-shutdown=%s city-load=%s",
					    regionSubscribed ? "succeeded" : "failed",
					    shutdownSubscribed ? "succeeded" : "failed",
					    citySubscribed ? "succeeded" : "failed");
				}
			}
			return true;
		}

		bool PreAppShutdown() {
			if (messageServer != nullptr) {
				messageServer->RemoveNotification(this, kSC4MessagePostRegionInit);
				messageServer->RemoveNotification(this, kSC4MessagePreRegionShutdown);
				messageServer->RemoveNotification(this, kSC4MessageCityLoadFinished);
				messageServer->Release();
				messageServer = nullptr;
			}
			StartupResourceLoadPatches::Uninstall();
			ParallelRenderCull::Uninstall();
			SimTickBudget::Uninstall();
			return true;
		}

		bool OnStart(cIGZCOM* pCOM) {
			unsigned const logicalProcessors = CpuScheduling::Apply();
			StartupResourceLoadPatches::Install();
			ParallelRenderCull::Install(logicalProcessors);
			SimTickBudget::Install();
			cIGZFrameWork* const pFramework = RZGetFrameWork();
			if (pFramework) {
				if (pFramework->GetState() < cIGZFrameWork::kStatePostAppInit) {
					pFramework->AddHook(this);
					if (pFramework->GetState() >= cIGZFrameWork::kStatePreAppInit) {
						PreAppInit();
					}
				}
				else {
					PostAppInit();
				}
			}
			return true;
		}

		void EnumClassObjects(ClassObjectEnumerationCallback pCallback, void* pContext) {
			pCallback(classId, 1000000, pContext);
		}
	};
}

cRZCOMSlimDllDirector* RZGetCOMDllDirector() {
	static nSCD3D11::cGDriversCOMDirector sDirector;
	return &sDirector;
}
