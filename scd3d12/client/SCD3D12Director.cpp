/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// SCD3D12.dll: SimCity 4's driver when SCD3D12Server draws. It claims the same class ID as SC4's DirectX driver
// and SCD3D11, with a higher version, so SC4 picks it whenever it is installed.

#include "cGDriverClient.h"
#include "cGDriverWire.h"
#include "Diagnostics.h"
#include "DrawOwnerHooks.h"
#include "FreeCamera.h"
#include "GameFolders.h"
#include "OccupantTracker.h"
#include "Settings.h"
#include "ThumbnailFocusGuard.h"

#include <cIGZFrameWork.h>
#include <cRZCOMDllDirector.h>

#include <cstdio>
#include <memory>

using namespace nSCD3D11;

namespace nSCD3D12 {
	namespace {
		constexpr uint32_t kDirectorID = 0x3D12C11E;
		// SCD3D11 reports 1000000 for the class ID, SC4's DirectX driver less.
		constexpr uint32_t kClassVersion = 1000001;

		bool gDriverCreated = false;

		bool CreateDriver(uint32_t riid, void **ppvObj) {
			if (!gDriverCreated) {
				gDriverCreated = true;
				DrawOwnerHooks::Install(true);
			}
			Settings::Values const &settings = Settings::Get();
			std::shared_ptr<std::FILE> record;
			if (!settings.record.empty()) {
				std::FILE *file = nullptr;
				if (fopen_s(&file, settings.record.c_str(), "wb") == 0 && file != nullptr) {
					record.reset(file, std::fclose);
					Log(LogCategory::Initialization, "record: writing every SimGL call to %s", settings.record.c_str());
				} else {
					Log(LogCategory::Initialization, "record: cannot open %s, not recording", settings.record.c_str());
				}
			}
			auto *const client = new cGDriverClient();
			auto *const wire = new cGDriverWire(client, client->Connect(record));
			if (!wire->QueryInterface(riid, ppvObj)) {
				delete wire;
				return false;
			}
			return true;
		}
	}

	class cSCD3D12Director final : public cRZCOMDllDirector {
	public:
		cSCD3D12Director() { AddCls(cGDriverClient::kGZCLSID, CreateDriver); }

		uint32_t GetDirectorID() const { return kDirectorID; }

		bool OnStart(cIGZCOM *) {
			cIGZFrameWork *const framework = RZGetFrameWork();
			Settings::Load(framework != nullptr ? framework->CommandLine() : nullptr);
			if (framework != nullptr) {
				if (framework->GetState() < cIGZFrameWork::kStatePreAppInit) framework->AddHook(this);
				else PreAppInit();
			}
			return true;
		}

		bool PostAppInit() {
			if (!gDriverCreated) return true;
			OccupantTracker::Install();
			FreeCamera::Install();
			RecordGameFolders();
			return true;
		}

		bool PreAppShutdown() {
			OccupantTracker::Uninstall();
			FreeCamera::Uninstall();
			DrawOwnerHooks::Uninstall();
			ThumbnailFocusGuard::Uninstall();
			return true;
		}

		void EnumClassObjects(ClassObjectEnumerationCallback callback, void *context) {
			callback(cGDriverClient::kGZCLSID, kClassVersion, context);
		}
	};
}

cRZCOMDllDirector *RZGetCOMDllDirector() {
	static nSCD3D12::cSCD3D12Director director;
	return &director;
}
