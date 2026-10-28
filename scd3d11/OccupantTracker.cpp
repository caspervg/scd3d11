/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#include "OccupantTracker.h"

#include "cGDriverWire.h"
#include "Diagnostics.h"

#include <GZCLSIDDefs.h>
#include <GZServPtrs.h>
#include <cIGZMessage2Standard.h>
#include <cIGZMessageServer2.h>
#include <cIGZMessageTarget2.h>
#include <cISC4App.h>
#include <cISC4City.h>
#include <cISC4Occupant.h>
#include <cISC4OccupantManager.h>
#include <cS3DVector3.h>

#include <cstring>

namespace nSCD3D11::OccupantTracker {
	namespace {
		// Posted by cSC4OccupantManager through cISC4Occupant::PostOccupantMessage: void1 is the occupant
		// and data3 its type.
		constexpr uint32_t kInsert = 0x99EF1142;
		constexpr uint32_t kRemove = 0x99EF1143;
		constexpr uint32_t kMove = 0x99EF1146;
		// Occupants loaded with a saved city post no Insert, so the whole city is listed once it is ready.
		constexpr uint32_t kPostCityInit = 0x26D31EC1;
		constexpr uint32_t kMessages[]{kInsert, kRemove, kMove, kPostCityInit};

		nSCDWire::OccupantEvent Describe(nSCDWire::OccupantEventKind kind, cIGZUnknown *unknown, int32_t type) {
			nSCDWire::OccupantEvent event{};
			event.kind = kind;
			event.id = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(unknown));
			event.type = type;
			cISC4Occupant *occupant = nullptr;
			if (kind != nSCDWire::OccupantEventKind::Remove && unknown != nullptr &&
			    unknown->QueryInterface(GZIID_cISC4Occupant, reinterpret_cast<void **>(&occupant))) {
				cS3DVector3 low, high;
				occupant->GetBoundingBox(&low, &high);
				float const box[6]{low.fX, low.fY, low.fZ, high.fX, high.fY, high.fZ};
				std::memcpy(event.box, box, sizeof(box));
				occupant->Release();
			}
			return event;
		}

		bool RecordExisting(cISC4Occupant *occupant, void *wire) {
			static_cast<cGDriverWire *>(wire)->RecordOccupant(
				Describe(nSCDWire::OccupantEventKind::Insert, occupant, occupant->GetType()));
			return true;
		}

		void RecordCity(cGDriverWire &wire) {
			cISC4AppPtr app;
			cISC4City *const city = app ? app->GetCity() : nullptr;
			cISC4OccupantManager *const manager = city != nullptr ? city->GetOccupantManager() : nullptr;
			if (manager == nullptr) return;
			// Slot 44: null bounds walk every cell, a null filter keeps every occupant.
			manager->IterateOccupants(RecordExisting, &wire, nullptr, nullptr, static_cast<cISC4OccupantFilter *>(nullptr));
		}

		// The target lives for the whole process, so its reference count only has to satisfy GZCOM.
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
				cGDriverWire *const wire = cGDriverWire::Current();
				cIGZMessage2Standard *standard = nullptr;
				if (wire == nullptr || message == nullptr ||
				    !message->QueryInterface(GZIID_cIGZMessage2Standard, reinterpret_cast<void **>(&standard))) {
					return true;
				}
				uint32_t const type = standard->GetType();
				if (type == kPostCityInit) {
					standard->Release();
					RecordCity(*wire);
					return true;
				}
				auto *const unknown = static_cast<cIGZUnknown *>(standard->GetVoid1());
				nSCDWire::OccupantEvent const event = Describe(
					type == kInsert ? nSCDWire::OccupantEventKind::Insert
					: type == kRemove ? nSCDWire::OccupantEventKind::Remove
					: nSCDWire::OccupantEventKind::Move,
					unknown, static_cast<int32_t>(standard->GetData3()));
				standard->Release();
				if (unknown != nullptr) wire->RecordOccupant(event);
				return true;
			}
		};

		Target gTarget;
		bool gInstalled = false;
	}

	void Install() {
		if (gInstalled) return;
		cIGZMessageServer2Ptr server;
		if (!server) {
			Log(LogCategory::Initialization, "occupants: no message server, occupants are not tracked");
			return;
		}
		for (uint32_t message: kMessages) server->AddNotification(&gTarget, message);
		gInstalled = true;
	}

	void Uninstall() {
		if (!gInstalled) return;
		cIGZMessageServer2Ptr server;
		if (server) {
			for (uint32_t message: kMessages) server->RemoveNotification(&gTarget, message);
		}
		gInstalled = false;
	}
}
