/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// How SCD3D11 runs sc4-modern-camera's camera: a persistent view input control from city init to city
// shutdown takes the mouse, a framework tick applies what it queued once per game loop, and the native angles
// come back before a save (the region thumbnail is drawn with them) and when the city closes. No code is
// patched. Holding ALT polls the key, so the game and other plugins keep it.

#include "FreeCamera.h"

#include "camera/Camera.h"
#include "camera/ModernCameraControl.h"
#include "cGDriverWire.h"
#include "Diagnostics.h"
#include "GameVersion.h"
#include "Settings.h"

#include <GZCLSIDDefs.h>
#include <GZServPtrs.h>
#include <cIGZFrameWork.h>
#include <cIGZMessage2Standard.h>
#include <cIGZMessageServer2.h>
#include <cIGZMessageTarget2.h>
#include <cISC4View3DWin.h>
#include <cRZBaseSystemService.h>
#include <SC4UI.h>

#include <cstring>

#include <windows.h>

namespace nSCD3D11::FreeCamera {
	namespace {
		constexpr uint32_t kTickServiceID = 0x5F1C2A15;
		constexpr uint32_t kPostCityInit = 0x26D31EC1;
		constexpr uint32_t kPreCityShutdown = 0x26D31EC2;
		constexpr uint32_t kPreSave = 0x26C63343;
		constexpr uint32_t kMessages[]{kPostCityInit, kPreCityShutdown, kPreSave};

		Camera gCamera;
		ModernCameraControl *gControl = nullptr;
		// What the server was last told to draw from.
		nSCDWire::FreeCamera gSent{};
		// The server's free camera turns the other way from the game's yaw.
		constexpr float kYawSign = -1.0f;
		constexpr float kDegrees = 180.0f / 3.14159265358979f;

		// While the server draws the static view itself, a drag turns only the server's camera, and the game's
		// camera follows when the drag ends.
		void SendDeferredView() {
			cGDriverWire *const wire = cGDriverWire::Current();
			gCamera.deferDrags = wire != nullptr && wire->Inner().RemoteOwnsView();
			if (wire == nullptr) return;
			nSCDWire::FreeCamera next{};
			float yaw = 0.0f, pitch = 0.0f;
			if (gCamera.DeferredView(yaw, pitch)) next = {1, kYawSign * yaw * kDegrees, pitch * kDegrees, 1.0f};
			if (std::memcmp(&next, &gSent, sizeof(next)) == 0) return;
			wire->RecordFreeCamera(next);
			gSent = next;
		}

		bool InForeground() {
			DWORD process = 0;
			GetWindowThreadProcessId(GetForegroundWindow(), &process);
			return process == GetCurrentProcessId();
		}

		// Once per game loop: ALT for the classic angle, then the turns and zoom queued since the last one.
		class Tick final : public cRZBaseSystemService {
		public:
			Tick() : cRZBaseSystemService(kTickServiceID, 0) { AddRef(); }
			bool Init() override { return true; }
			bool Shutdown() override { return true; }
			bool OnTick(uint32_t) override {
				bool const alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
				bool const control = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
				gCamera.SetClassicHold(alt && !control && InForeground());
				gCamera.Update();
				SendDeferredView();
				return true;
			}
		};
		Tick gTick;

		void CityInit() {
			cISC4View3DWin *const view = SC4UI::GetView3DWin();
			if (view == nullptr || gControl != nullptr) return;
			gControl = new ModernCameraControl(gCamera);
			gControl->AddRef();
			if (!view->AddPersistentViewInputControl(gControl)) {
				gControl->Release();
				gControl = nullptr;
				Log(LogCategory::Initialization, "free camera: the city view took no input control");
				return;
			}
			bool const enabled = gCamera.Enable();
			Log(LogCategory::Initialization, "free camera: %s", enabled ? "on" : "the city view has no camera control");
		}

		void CityShutdown() {
			// Before the city goes: the native angle tables are global.
			gCamera.Disable();
			if (gControl == nullptr) return;
			if (cISC4View3DWin *const view = SC4UI::GetView3DWin()) view->RemovePersistentViewInputControl(gControl);
			gControl->Shutdown();
			gControl->Release();
			gControl = nullptr;
		}

		// Lives for the whole process, so its reference count only has to satisfy GZCOM.
		class Target final : public cIGZMessageTarget2 {
		public:
			bool QueryInterface(uint32_t riid, void **ppvObj) override {
				if (riid != GZIID_cIGZUnknown && riid != static_cast<uint32_t>(GZCLSID::kcIGZMessageTarget2)) return false;
				*ppvObj = static_cast<cIGZMessageTarget2 *>(this);
				return true;
			}
			uint32_t AddRef(void) override { return 1; }
			uint32_t Release(void) override { return 1; }
			bool DoMessage(cIGZMessage2 *message) override {
				if (message == nullptr) return true;
				switch (message->GetType()) {
					case kPostCityInit: CityInit(); break;
					case kPreCityShutdown: CityShutdown(); break;
					case kPreSave: gCamera.PrepareForSave(); break;
					default: break;
				}
				return true;
			}
		};
		Target gTarget;
		bool gInstalled = false;
	}

	void Install() {
		if (gInstalled || !Settings::Get().freeCamera) return;
		// The camera control's addresses are 1.1.641's.
		if (!IsGameVersion641()) {
			Log(LogCategory::Initialization, "free camera: not this version of SimCity 4");
			return;
		}
		cIGZMessageServer2Ptr server;
		cIGZFrameWork *const framework = RZGetFrameWork();
		if (!server || framework == nullptr) return;
		for (uint32_t message: kMessages) server->AddNotification(&gTarget, message);
		gTick.SetServiceRunning(true);
		framework->AddSystemService(&gTick);
		framework->AddToTick(&gTick);
		gInstalled = true;
	}

	void Uninstall() {
		if (!gInstalled) return;
		CityShutdown();
		cIGZMessageServer2Ptr server;
		if (server) {
			for (uint32_t message: kMessages) server->RemoveNotification(&gTarget, message);
		}
		if (cIGZFrameWork *const framework = RZGetFrameWork()) framework->RemoveFromTick(&gTick);
		gInstalled = false;
	}
}
